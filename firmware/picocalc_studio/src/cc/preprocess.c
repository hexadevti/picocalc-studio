// This file implements the C preprocessor.
//
// The preprocessor takes a list of tokens as an input and returns a
// new list of tokens as an output.
//
// The preprocessing language is designed in such a way that that's
// guaranteed to stop even if there is a recursive macro.
// Informally speaking, a macro is applied only once for each token.
// That is, if a macro token T appears in a result of direct or
// indirect macro expansion of T, T won't be expanded any further.
// For example, if T is defined as U, and U is defined as T, then
// token T is expanded to U and then to T and the macro expansion
// stops at that point.
//
// To achieve the above behavior, we attach for each token a set of
// macro names from which the token is expanded. The set is called
// "hideset". Hideset is initially empty, and every time we expand a
// macro, the macro name is added to the resulting tokens' hidesets.
//
// The above macro expansion algorithm is explained in this document
// written by Dave Prossor, which is used as a basis for the
// standard's wording:
// https://github.com/rui314/chibicc/wiki/cpp.algo.pdf

#include "chibicc.h"
#include <time.h>

typedef struct MacroParam MacroParam;
struct MacroParam {
  MacroParam *next;
  char *name;
};

typedef struct MacroArg MacroArg;
struct MacroArg {
  MacroArg *next;
  char *name;
  bool is_va_args;
  Token *tok;
};

typedef Token *macro_handler_fn(Token *);

typedef struct Macro Macro;
struct Macro {
  char *name;
  bool is_objlike; // Object-like or function-like
  MacroParam *params;
  char *va_args_name;
  // The body is not kept as tokens but lexed again from its text for each expansion (most
  // macros in the headers are never used): where its first token is, or "" if it is empty.
  File *file;
  char *loc;
  int line_no;
  bool has_space;
  macro_handler_fn *handler;
};

// `#if` can be nested, so we use a stack to manage nested `#if`s.
typedef struct CondIncl CondIncl;
struct CondIncl {
  CondIncl *next;
  enum { IN_THEN, IN_ELIF, IN_ELSE } ctx;
  Token *tok;
  bool included;
  char *guard;       // include guard macro, if this is the #ifndef at the top of guard_file
  char *guard_path;
  File *guard_file;
};

typedef struct Hideset Hideset;
struct Hideset {
  Hideset *next;
  char *name;
};

static HashMap macros;
static CondIncl *cond_incl;
static HashMap pragma_once;
static int include_next_idx;
static HashMap include_path_cache;
static HashMap include_guards;
static int counter;

static int depth;                     // preprocess2() nesting; 1 is the top-level pass
static Token *root_src, *root_copy;  // last origin root keep_token() copied, and its copy
static Token *raw_root;              // origin root of the last kept token
static Token *pp_rest;               // where the top-level pass resumes for the next declaration
static Token *batch_roots;           // origin roots copied for the current declaration
static int nest;                     // ( [ { nesting of the top-level output
static bool assigned, fn_body;       // "=" seen in this declaration; the open "{" is a function body
static Token *guard_hash;            // a just-included file's "#ifndef X" / "#define X" opening
static char *guard_macro, *guard_path;

void preprocess_reset(void) {
  macros = pragma_once = include_path_cache = include_guards = (HashMap){0};
  guard_hash = pp_rest = batch_roots = NULL;
  nest = 0;
  assigned = fn_body = false;
  cond_incl = NULL;
  include_next_idx = 0;
  counter = 0;
  depth = 0;
  root_src = root_copy = raw_root = NULL;
}

static Token *preprocess2(Token *tok);
static Macro *find_macro(Token *tok);

static bool is_hash(Token *tok) {
  return tok->at_bol && equal(tok, "#");
}

// Some preprocessor directives such as #include allow extraneous
// tokens before newline. This function skips such tokens.
static Token *skip_line(Token *tok) {
  if (tok->at_bol)
    return tok;
  warn_tok(tok, "extra token");
  while (tok->at_bol)
    tok = tok->next;
  return tok;
}

static Token *copy_token(Token *tok) {
  Token *t = cc_tok_alloc();
  *t = *tok;
  t->next = NULL;
  return t;
}

// Copies a finished token of the top-level pass into the main arena, which the scratch arena
// (raw tokens, macro bodies, expansion copies, hidesets) does not outlive. Files already live in
// main. What survives: the token, a string literal's type and contents, and for a macro-expanded
// token the root of its origin chain (codegen's line table only looks at the root).
// A macro expansion's tokens come out together, so once one with another root is kept, the last
// root (the macro name, a raw token) is free.
static Token *keep_token(Token *t) {
  if (raw_root && t->origin != raw_root) {
    if (raw_root == root_src)
      root_src = NULL;
    cc_tok_free(raw_root);
  }
  raw_root = t->origin;
  bool keep = cc_keep(true);
  Token *c = copy_token(t);
  c->hideset = NULL;
  if (t->kind == TK_STR) {
    Type *ty = calloc(1, sizeof(Type));
    *ty = *t->ty;
    c->ty = ty;
    c->str = calloc(1, ty->size);
    memcpy(c->str, t->str, ty->size);
  }
  if (t->origin) {
    Token *r = t->origin;  // a root (expand_macro())
    if (r != root_src) {
      root_src = r;
      root_copy = copy_token(r);
      root_copy->origin = NULL;
      root_copy->hideset = NULL;
      root_copy->ty = NULL;
      root_copy->next = batch_roots;
      batch_roots = root_copy;
    }
    c->origin = root_copy;
  }
  cc_keep(keep);
  return c;
}

static Token *new_eof(Token *tok) {
  Token *t = copy_token(tok);
  t->kind = TK_EOF;
  t->len = 0;
  return t;
}

static Hideset *new_hideset(char *name) {
  Hideset *hs = calloc(1, sizeof(Hideset));
  hs->name = name;
  return hs;
}

static Hideset *hideset_union(Hideset *hs1, Hideset *hs2) {
  Hideset head = {};
  Hideset *cur = &head;

  for (; hs1; hs1 = hs1->next)
    cur = cur->next = new_hideset(hs1->name);
  cur->next = hs2;
  return head.next;
}

static bool hideset_contains(Hideset *hs, char *s, int len) {
  for (; hs; hs = hs->next)
    if (strlen(hs->name) == len && !strncmp(hs->name, s, len))
      return true;
  return false;
}

static Hideset *hideset_intersection(Hideset *hs1, Hideset *hs2) {
  Hideset head = {};
  Hideset *cur = &head;

  for (; hs1; hs1 = hs1->next)
    if (hideset_contains(hs2, hs1->name, strlen(hs1->name)))
      cur = cur->next = new_hideset(hs1->name);
  return head.next;
}

// Append tok2 to the end of tok1, dropping tok1's EOF. tok1 is relinked in place (upstream
// chibicc copies it), so it must be a list nobody else holds: a fresh macro expansion.
static Token *splice(Token *tok1, Token *tok2) {
  if (tok1->kind == TK_EOF)
    return tok2;

  Token *t = tok1;
  while (t->next->kind != TK_EOF)
    t = t->next;
  t->next = tok2;
  return tok1;
}

// Skipped tokens are raw ones nobody points at: at the top level they are recycled.
static Token *drop_token(Token *tok) {
  Token *next = tok->next;
  if (depth == 1)
    cc_tok_free(tok);
  return next;
}

static Token *skip_cond_incl2(Token *tok) {
  while (more(tok)->kind != TK_EOF) {
    if (is_hash(tok) &&
        (equal(tok->next, "if") || equal(tok->next, "ifdef") ||
         equal(tok->next, "ifndef"))) {
      tok = skip_cond_incl2(drop_token(drop_token(tok)));
      continue;
    }
    if (is_hash(tok) && equal(tok->next, "endif"))
      return drop_token(drop_token(tok));
    tok = drop_token(tok);
  }
  return tok;
}

// Skip until next `#else`, `#elif` or `#endif`.
// Nested `#if` and `#endif` are skipped.
static Token *skip_cond_incl(Token *tok) {
  while (more(tok)->kind != TK_EOF) {
    if (is_hash(tok) &&
        (equal(tok->next, "if") || equal(tok->next, "ifdef") ||
         equal(tok->next, "ifndef"))) {
      tok = skip_cond_incl2(drop_token(drop_token(tok)));
      continue;
    }

    if (is_hash(tok) &&
        (equal(tok->next, "elif") || equal(tok->next, "else") ||
         equal(tok->next, "endif")))
      break;
    tok = drop_token(tok);
  }
  return tok;
}

// Double-quote a given string and returns it.
static char *quote_string(char *str) {
  int bufsize = 3;
  for (int i = 0; str[i]; i++) {
    if (str[i] == '\\' || str[i] == '"')
      bufsize++;
    bufsize++;
  }

  char *buf = calloc(1, bufsize);
  char *p = buf;
  *p++ = '"';
  for (int i = 0; str[i]; i++) {
    if (str[i] == '\\' || str[i] == '"')
      *p++ = '\\';
    *p++ = str[i];
  }
  *p++ = '"';
  *p++ = '\0';
  return buf;
}

static Token *new_str_token(char *str, Token *tmpl) {
  char *buf = quote_string(str);
  return tokenize(new_file(tmpl->file->name, tmpl->file->file_no, buf));
}

// Copy all tokens until the next newline, terminate them with
// an EOF token and then returns them. This function is used to
// create a new list of tokens for `#if` arguments.
static Token *copy_line(Token **rest, Token *tok) {
  Token head = {};
  Token *cur = &head;

  for (; !tok->at_bol; tok = tok->next)
    cur = cur->next = copy_token(tok);

  cur->next = new_eof(tok);
  *rest = tok;
  return head.next;
}

// Like copy_line, but the returned list is the line's own tokens (raw scratch tokens nobody else
// points at), cut off with an EOF.
static Token *take_line(Token **rest, Token *tok) {
  Token head = {};
  Token *cur = &head;

  for (; !tok->at_bol; tok = tok->next)
    cur = cur->next = tok;

  cur->next = new_eof(tok);
  *rest = tok;
  return head.next;
}

// Recycles the directive tokens from tok up to (not including) end.
// With end NULL, up to and including an EOF.
static void free_tokens(Token *tok, Token *end) {
  while (tok != end) {
    Token *next = tok->kind == TK_EOF ? NULL : tok->next;
    cc_tok_free(tok);
    tok = next;
  }
}

static Token *new_num_token(int val, Token *tmpl) {
  char *buf = format("%d\n", val);
  return tokenize(new_file(tmpl->file->name, tmpl->file->file_no, buf));
}

static Token *read_const_expr(Token **rest, Token *tok) {
  tok = copy_line(rest, tok);

  Token head = {};
  Token *cur = &head;

  while (tok->kind != TK_EOF) {
    // "defined(foo)" or "defined foo" becomes "1" if macro "foo"
    // is defined. Otherwise "0".
    if (equal(tok, "defined")) {
      Token *start = tok;
      bool has_paren = consume(&tok, tok->next, "(");

      if (tok->kind != TK_IDENT)
        error_tok(start, "macro name must be an identifier");
      Macro *m = find_macro(tok);
      tok = tok->next;

      if (has_paren)
        tok = skip(tok, ")");

      cur = cur->next = new_num_token(m ? 1 : 0, start);
      continue;
    }

    cur = cur->next = tok;
    tok = tok->next;
  }

  cur->next = tok;
  return head.next;
}

// Read and evaluate a constant expression.
static long eval_const_expr(Token **rest, Token *tok) {
  Token *start = tok;
  Token *expr = read_const_expr(rest, tok->next);
  expr = preprocess2(expr);

  if (expr->kind == TK_EOF)
    error_tok(start, "no expression");

  // [https://www.sigbus.info/n1570#6.10.1p4] The standard requires
  // we replace remaining non-macro identifiers with "0" before
  // evaluating a constant expression. For example, `#if foo` is
  // equivalent to `#if 0` if foo is not defined.
  for (Token *t = expr; t->kind != TK_EOF; t = t->next) {
    if (t->kind == TK_IDENT) {
      Token *next = t->next;
      *t = *new_num_token(0, t);
      t->next = next;
    }
  }

  // Convert pp-numbers to regular numbers
  convert_pp_tokens(expr);

  Token *rest2;
  long val = const_expr(&rest2, expr);
  if (rest2->kind != TK_EOF)
    error_tok(rest2, "extra token");
  return val;
}

static CondIncl *push_cond_incl(Token *tok, bool included) {
  CondIncl *ci = calloc(1, sizeof(CondIncl));
  ci->next = cond_incl;
  ci->ctx = IN_THEN;
  ci->tok = tok;
  ci->included = included;
  cond_incl = ci;
  return ci;
}

static Macro *find_macro(Token *tok) {
  if (tok->kind != TK_IDENT)
    return NULL;
  return hashmap_get2(&macros, tok->loc, tok->len);
}

// body is a line's raw tokens ending in an EOF (NULL for a built-in); they are recycled.
static Macro *add_macro(char *name, bool is_objlike, Token *body) {
  Macro *m = calloc(1, sizeof(Macro));
  m->name = name;
  m->is_objlike = is_objlike;
  if (body) {
    m->file = body->file;
    m->line_no = body->line_no;
    m->has_space = body->has_space;
    m->loc = body->kind == TK_EOF ? "" : body->loc;
    free_tokens(body, NULL);
  }
  hashmap_put(&macros, name, m);
  return m;
}

static Token *macro_body(Macro *m) {
  Token *body = tokenize_line(m->file, m->loc, m->line_no);
  body->at_bol = false;
  body->has_space = m->has_space;
  return body;
}

static MacroParam *read_macro_params(Token **rest, Token *tok, char **va_args_name) {
  MacroParam head = {};
  MacroParam *cur = &head;

  while (!equal(tok, ")")) {
    if (cur != &head)
      tok = skip(tok, ",");

    if (equal(tok, "...")) {
      *va_args_name = "__VA_ARGS__";
      *rest = skip(tok->next, ")");
      return head.next;
    }

    if (tok->kind != TK_IDENT)
      error_tok(tok, "expected an identifier");

    if (equal(tok->next, "...")) {
      *va_args_name = strndup(tok->loc, tok->len);
      *rest = skip(tok->next->next, ")");
      return head.next;
    }

    MacroParam *m = calloc(1, sizeof(MacroParam));
    m->name = strndup(tok->loc, tok->len);
    cur = cur->next = m;
    tok = tok->next;
  }

  *rest = tok->next;
  return head.next;
}

static void read_macro_definition(Token **rest, Token *tok) {
  if (tok->kind != TK_IDENT)
    error_tok(tok, "macro name must be an identifier");
  char *name = strndup(tok->loc, tok->len);
  Token *name_tok = tok;
  tok = tok->next;

  if (!tok->has_space && equal(tok, "(")) {
    // Function-like macro
    char *va_args_name = NULL;
    MacroParam *params = read_macro_params(&tok, tok->next, &va_args_name);
    free_tokens(name_tok, tok);

    Macro *m = add_macro(name, false, take_line(rest, tok));
    m->params = params;
    m->va_args_name = va_args_name;
  } else {
    // Object-like macro
    free_tokens(name_tok, tok);
    add_macro(name, true, take_line(rest, tok));
  }
}

static MacroArg *read_macro_arg_one(Token **rest, Token *tok, bool read_rest) {
  Token head = {};
  Token *cur = &head;
  int level = 0;

  for (;;) {
    more(tok);
    if (level == 0 && equal(tok, ")"))
      break;
    if (level == 0 && !read_rest && equal(tok, ","))
      break;

    if (tok->kind == TK_EOF)
      error_tok(tok, "premature end of input");

    if (equal(tok, "("))
      level++;
    else if (equal(tok, ")"))
      level--;

    cur = cur->next = copy_token(tok);
    tok = tok->next;
  }

  cur->next = new_eof(tok);

  MacroArg *arg = calloc(1, sizeof(MacroArg));
  arg->tok = head.next;
  *rest = tok;
  return arg;
}

static MacroArg *
read_macro_args(Token **rest, Token *tok, MacroParam *params, char *va_args_name) {
  Token *start = tok;
  tok = tok->next->next;

  MacroArg head = {};
  MacroArg *cur = &head;

  MacroParam *pp = params;
  for (; pp; pp = pp->next) {
    if (cur != &head)
      tok = skip(tok, ",");
    cur = cur->next = read_macro_arg_one(&tok, tok, false);
    cur->name = pp->name;
  }

  if (va_args_name) {
    MacroArg *arg;
    if (equal(more(tok), ")")) {
      arg = calloc(1, sizeof(MacroArg));
      arg->tok = new_eof(tok);
    } else {
      if (pp != params)
        tok = skip(tok, ",");
      arg = read_macro_arg_one(&tok, tok, true);
    }
    arg->name = va_args_name;;
    arg->is_va_args = true;
    cur = cur->next = arg;
  } else if (pp) {
    error_tok(start, "too many arguments");
  }

  skip(tok, ")");
  *rest = tok;
  return head.next;
}

static MacroArg *find_arg(MacroArg *args, Token *tok) {
  for (MacroArg *ap = args; ap; ap = ap->next)
    if (tok->len == strlen(ap->name) && !strncmp(tok->loc, ap->name, tok->len))
      return ap;
  return NULL;
}

// Concatenates all tokens in `tok` and returns a new string.
static char *join_tokens(Token *tok, Token *end) {
  // Compute the length of the resulting token.
  int len = 1;
  for (Token *t = tok; t != end && t->kind != TK_EOF; t = t->next) {
    if (t != tok && t->has_space)
      len++;
    len += t->len;
  }

  char *buf = calloc(1, len);

  // Copy token texts.
  int pos = 0;
  for (Token *t = tok; t != end && t->kind != TK_EOF; t = t->next) {
    if (t != tok && t->has_space)
      buf[pos++] = ' ';
    strncpy(buf + pos, t->loc, t->len);
    pos += t->len;
  }
  buf[pos] = '\0';
  return buf;
}

// Concatenates all tokens in `arg` and returns a new string token.
// This function is used for the stringizing operator (#).
static Token *stringize(Token *hash, Token *arg) {
  // Create a new string token. We need to set some value to its
  // source location for error reporting function, so we use a macro
  // name token as a template.
  char *s = join_tokens(arg, NULL);
  return new_str_token(s, hash);
}

// Concatenate two tokens to create a new token.
static Token *paste(Token *lhs, Token *rhs) {
  // Paste the two tokens.
  char *buf = format("%.*s%.*s", lhs->len, lhs->loc, rhs->len, rhs->loc);

  // Tokenize the resulting string.
  Token *tok = tokenize(new_file(lhs->file->name, lhs->file->file_no, buf));
  if (tok->next->kind != TK_EOF)
    error_tok(lhs, "pasting forms '%s', an invalid token", buf);
  return tok;
}

static bool has_varargs(MacroArg *args) {
  for (MacroArg *ap = args; ap; ap = ap->next)
    if (!strcmp(ap->name, "__VA_ARGS__"))
      return ap->tok->kind != TK_EOF;
  return false;
}

// Replace func-like macro parameters with given arguments.
static Token *subst(Token *tok, MacroArg *args) {
  Token head = {};
  Token *cur = &head;

  while (tok->kind != TK_EOF) {
    // "#" followed by a parameter is replaced with stringized actuals.
    if (equal(tok, "#")) {
      MacroArg *arg = find_arg(args, tok->next);
      if (!arg)
        error_tok(tok->next, "'#' is not followed by a macro parameter");
      cur = cur->next = stringize(tok, arg->tok);
      tok = tok->next->next;
      continue;
    }

    // [GNU] If __VA_ARG__ is empty, `,##__VA_ARGS__` is expanded
    // to the empty token list. Otherwise, its expaned to `,` and
    // __VA_ARGS__.
    if (equal(tok, ",") && equal(tok->next, "##")) {
      MacroArg *arg = find_arg(args, tok->next->next);
      if (arg && arg->is_va_args) {
        if (arg->tok->kind == TK_EOF) {
          tok = tok->next->next->next;
        } else {
          cur = cur->next = copy_token(tok);
          tok = tok->next->next;
        }
        continue;
      }
    }

    if (equal(tok, "##")) {
      if (cur == &head)
        error_tok(tok, "'##' cannot appear at start of macro expansion");

      if (tok->next->kind == TK_EOF)
        error_tok(tok, "'##' cannot appear at end of macro expansion");

      MacroArg *arg = find_arg(args, tok->next);
      if (arg) {
        if (arg->tok->kind != TK_EOF) {
          *cur = *paste(cur, arg->tok);
          for (Token *t = arg->tok->next; t->kind != TK_EOF; t = t->next)
            cur = cur->next = copy_token(t);
        }
        tok = tok->next->next;
        continue;
      }

      *cur = *paste(cur, tok->next);
      tok = tok->next->next;
      continue;
    }

    MacroArg *arg = find_arg(args, tok);

    if (arg && equal(tok->next, "##")) {
      Token *rhs = tok->next->next;

      if (arg->tok->kind == TK_EOF) {
        MacroArg *arg2 = find_arg(args, rhs);
        if (arg2) {
          for (Token *t = arg2->tok; t->kind != TK_EOF; t = t->next)
            cur = cur->next = copy_token(t);
        } else {
          cur = cur->next = copy_token(rhs);
        }
        tok = rhs->next;
        continue;
      }

      for (Token *t = arg->tok; t->kind != TK_EOF; t = t->next)
        cur = cur->next = copy_token(t);
      tok = tok->next;
      continue;
    }

    // If __VA_ARG__ is empty, __VA_OPT__(x) is expanded to the
    // empty token list. Otherwise, __VA_OPT__(x) is expanded to x.
    if (equal(tok, "__VA_OPT__") && equal(tok->next, "(")) {
      MacroArg *arg = read_macro_arg_one(&tok, tok->next->next, true);
      if (has_varargs(args))
        for (Token *t = arg->tok; t->kind != TK_EOF; t = t->next)
          cur = cur->next = t;
      tok = skip(tok, ")");
      continue;
    }

    // Handle a macro token. Macro arguments are completely macro-expanded
    // before they are substituted into a macro body.
    if (arg) {
      Token *t = preprocess2(arg->tok);
      t->at_bol = tok->at_bol;
      t->has_space = tok->has_space;
      for (; t->kind != TK_EOF; t = t->next)
        cur = cur->next = copy_token(t);
      tok = tok->next;
      continue;
    }

    // Handle a non-macro token.
    cur = cur->next = copy_token(tok);
    tok = tok->next;
    continue;
  }

  cur->next = tok;
  return head.next;
}

// If tok is a macro, expand it and return true.
// Otherwise, do nothing and return false.
static bool expand_macro(Token **rest, Token *tok) {
  if (hideset_contains(tok->hideset, tok->loc, tok->len))
    return false;

  Macro *m = find_macro(tok);
  if (!m)
    return false;

  // Built-in dynamic macro application such as __LINE__
  if (m->handler) {
    *rest = m->handler(tok);
    (*rest)->next = tok->next;
    return true;
  }

  // Object-like macro application
  // Expanded tokens point at the root of the macro token's origin chain, so only a root is
  // ever an origin (keep_token() frees it after use). The rest of the invocation is freed here
  // at the top level; below it, expansions are for a caller that copies what it needs.
  Token *root = tok->origin ? tok->origin : tok;

  if (m->is_objlike) {
    Hideset *hs = hideset_union(tok->hideset, new_hideset(m->name));
    Token *body = macro_body(m), *eof = body;
    for (; eof->kind != TK_EOF; eof = eof->next) {
      eof->hideset = hs;
      eof->origin = root;
    }
    *rest = splice(body, tok->next);
    (*rest)->at_bol = tok->at_bol;
    (*rest)->has_space = tok->has_space;
    cc_tok_free(eof);
    if (depth == 1 && (tok != root || body == eof))
      cc_tok_free(tok);
    return true;
  }

  // If a funclike macro token is not followed by an argument list,
  // treat it as a normal identifier.
  if (!equal(more(tok->next), "("))
    return false;

  // Function-like macro application
  bool top = depth == 1;
  if (top)
    cc_tok_track(true);
  Token *macro_token = tok;
  MacroArg *args = read_macro_args(&tok, tok, m->params, m->va_args_name);
  Token *rparen = tok;

  // Tokens that consist a func-like macro invocation may have different
  // hidesets, and if that's the case, it's not clear what the hideset
  // for the new tokens should be. We take the interesection of the
  // macro token and the closing parenthesis and use it as a new hideset
  // as explained in the Dave Prossor's algorithm.
  Hideset *hs = hideset_intersection(macro_token->hideset, rparen->hideset);
  hs = hideset_union(hs, new_hideset(m->name));

  // subst() returns fresh tokens ending in the EOF of def, so tag them in place; def is
  // garbage after the splice.
  Token *def = macro_body(m);
  Token *body = subst(def, args);
  for (Token *t = body; t->kind != TK_EOF; t = t->next) {
    t->hideset = hideset_union(t->hideset, hs);
    t->origin = root;
    t->marked = top;
  }
  *rest = splice(body, tok->next);
  (*rest)->at_bol = macro_token->at_bol;
  (*rest)->has_space = macro_token->has_space;
  free_tokens(def, NULL);
  if (top) {
    cc_tok_sweep();
    for (Token *t = macro_token->next, *next; t != rparen; t = next) {
      next = t->next;
      cc_tok_free(t);
    }
    cc_tok_free(rparen);
    if (macro_token != root || body->kind == TK_EOF)
      cc_tok_free(macro_token);
  }
  return true;
}

char *search_include_paths(char *filename) {
  if (filename[0] == '/')
    return filename;

  char *cached = hashmap_get(&include_path_cache, filename);
  if (cached)
    return cached;

  // Search a file from the include paths.
  for (int i = 0; i < include_paths.len; i++) {
    char *path = format("%s/%s", include_paths.data[i], filename);
    if (!file_exists(path))
      continue;
    hashmap_put(&include_path_cache, filename, path);
    include_next_idx = i + 1;
    return path;
  }
  return NULL;
}

static char *search_include_next(char *filename) {
  for (; include_next_idx < include_paths.len; include_next_idx++) {
    char *path = format("%s/%s", include_paths.data[include_next_idx], filename);
    if (file_exists(path))
      return path;
  }
  return NULL;
}

// Read an #include argument.
static char *read_include_filename(Token **rest, Token *tok, bool *is_dquote) {
  // Pattern 1: #include "foo.h"
  if (tok->kind == TK_STR) {
    // A double-quoted filename for #include is a special kind of
    // token, and we don't want to interpret any escape sequences in it.
    // For example, "\f" in "C:\foo" is not a formfeed character but
    // just two non-control characters, backslash and f.
    // So we don't want to use token->str.
    *is_dquote = true;
    *rest = skip_line(tok->next);
    return strndup(tok->loc + 1, tok->len - 2);
  }

  // Pattern 2: #include <foo.h>
  if (equal(tok, "<")) {
    // Reconstruct a filename from a sequence of tokens between
    // "<" and ">".
    Token *start = tok;

    // Find closing ">".
    for (; !equal(tok, ">"); tok = tok->next)
      if (tok->at_bol || tok->kind == TK_EOF)
        error_tok(tok, "expected '>'");

    *is_dquote = false;
    *rest = skip_line(tok->next);
    return join_tokens(start->next, tok);
  }

  // Pattern 3: #include FOO
  // In this case FOO must be macro-expanded to either
  // a single string token or a sequence of "<" ... ">".
  if (tok->kind == TK_IDENT) {
    Token *tok2 = preprocess2(copy_line(rest, tok));
    return read_include_filename(&tok2, tok2, is_dquote);
  }

  error_tok(tok, "expected a filename");
}

static Token *include_file(Token *tok, char *path, Token *filename_tok) {
  // Check for "#pragma once"
  if (hashmap_get(&pragma_once, path))
    return tok;

  // If we read the same file before, and if the file was guarded
  // by the usual #ifndef ... #endif pattern, we may be able to
  // skip the file without opening it.
  char *guard_name = hashmap_get(&include_guards, path);
  if (guard_name && hashmap_get(&macros, guard_name))
    return tok;

  Token *tok2 = tokenize_file(path, tok);
  if (!tok2)
    error_tok(filename_tok, "%s: cannot open file (no such file)", path);

  // A file opening with "#ifndef FOO" / "#define FOO" may be guarded. The #ifndef handler picks
  // this up, and the matching #endif confirms it if it turns out to end the file.
  Token *t = more(tok2);
  if (is_hash(t) && !strcmp(t->file->name, path) && equal(t->next, "ifndef") &&
      t->next->next->kind == TK_IDENT) {
    char *macro = strndup(t->next->next->loc, t->next->next->len);
    Token *d = more(t->next->next->next);
    if (is_hash(d) && equal(d->next, "define") && equal(d->next->next, macro)) {
      guard_hash = t;
      guard_macro = macro;
      guard_path = path;
    }
  }
  return tok2;
}

// Read #line arguments
static void read_line_marker(Token **rest, Token *tok) {
  Token *start = tok;
  tok = preprocess(copy_line(rest, tok));

  if (tok->kind != TK_NUM || tok->ty->kind != TY_INT)
    error_tok(tok, "invalid line marker");
  start->file->line_delta = tok->val - start->line_no;

  tok = tok->next;
  if (tok->kind == TK_EOF)
    return;

  if (tok->kind != TK_STR)
    error_tok(tok, "filename expected");
  bool keep = cc_keep(true);
  start->file->display_name = strdup(tok->str);
  cc_keep(keep);
}

// Does output token t (after prev) end a top-level declaration? That is a ";" outside any
// brackets, or the "}" closing a function body: a "{" right after ")" with no "=" before it in the
// declaration (which would make it a compound literal). A missed end only makes a batch longer.
static bool ends_decl(Token *t, Token *prev) {
  if (t->kind != TK_PUNCT || t->len != 1)
    return false;
  switch (*t->loc) {
  case '(': case '[':
    nest++;
    return false;
  case ')': case ']':
    nest--;
    return false;
  case '{':
    if (nest++ == 0)
      fn_body = !assigned && prev && equal(prev, ")");
    return false;
  case '=':
    if (nest == 0)
      assigned = true;
    return false;
  case '}':
    if (--nest != 0 || !fn_body)
      return false;
    fn_body = false;
    break;
  case ';':
    if (nest != 0)
      return false;
    break;
  default:
    return false;
  }
  assigned = false;
  return true;
}

// Visit all tokens in `tok` while evaluating preprocessing
// macros and directives.
// At the top level, each output token is copied into the main arena as it is produced and its
// scratch slot recycled, and the pass stops after each top-level declaration, ending the list
// with a TK_MORE token for preprocess_more(). Nested calls (#if, macro arguments, #include and
// #line operands) produce scratch lists their callers still work on.
static Token *preprocess2(Token *tok) {
  Token head = {};
  Token *cur = &head;
  bool top = ++depth == 1;

  while (more(tok)->kind != TK_EOF) {
    // If it is a macro, expand it.
    if (expand_macro(&tok, tok))
      continue;

    // Pass through if it is not a "#".
    if (!is_hash(tok)) {
      tok->line_no += tok->file->line_delta;
      Token *next = tok->next;
      if (!top) {
        cur = cur->next = tok;
        tok = next;
        continue;
      }
      Token *prev = cur == &head ? NULL : cur;
      cur = cur->next = keep_token(tok);
      cc_tok_free(tok);
      tok = next;
      if (ends_decl(cur, prev)) {
        pp_rest = tok;
        root_src = NULL;  // the next declaration's tokens get roots of their own
        bool keep = cc_keep(true);
        Token *m = cur->next = copy_token(cur);
        cc_keep(keep);
        m->kind = TK_MORE;
        m->len = 0;
        m->origin = NULL;
        depth--;
        return head.next;
      }
      continue;
    }

    Token *start = tok;
    tok = tok->next;

    if (equal(tok, "include")) {
      bool is_dquote;
      char *filename = read_include_filename(&tok, tok->next, &is_dquote);

      if (filename[0] != '/' && is_dquote) {
        char *path = format("%s/%s", cc_dirname(start->file->name), filename);
        if (file_exists(path)) {
          tok = include_file(tok, path, start->next->next);
          continue;
        }
      }

      char *path = search_include_paths(filename);
      tok = include_file(tok, path ? path : filename, start->next->next);
      continue;
    }

    if (equal(tok, "include_next")) {
      bool ignore;
      char *filename = read_include_filename(&tok, tok->next, &ignore);
      char *path = search_include_next(filename);
      tok = include_file(tok, path ? path : filename, start->next->next);
      continue;
    }

    if (equal(tok, "define")) {
      Token *name = tok->next;
      read_macro_definition(&tok, name);
      free_tokens(start, name);
      continue;
    }

    if (equal(tok, "undef")) {
      tok = tok->next;
      if (tok->kind != TK_IDENT)
        error_tok(tok, "macro name must be an identifier");
      undef_macro(strndup(tok->loc, tok->len));
      tok = skip_line(tok->next);
      continue;
    }

    if (equal(tok, "if")) {
      long val = eval_const_expr(&tok, tok);
      push_cond_incl(start, val);
      if (!val)
        tok = skip_cond_incl(tok);
      continue;
    }

    if (equal(tok, "ifdef")) {
      bool defined = find_macro(tok->next);
      push_cond_incl(tok, defined);
      tok = skip_line(tok->next->next);
      if (!defined)
        tok = skip_cond_incl(tok);
      continue;
    }

    if (equal(tok, "ifndef")) {
      bool defined = find_macro(tok->next);
      CondIncl *ci = push_cond_incl(tok, !defined);
      if (start == guard_hash) {
        ci->guard = guard_macro;
        ci->guard_path = guard_path;
        ci->guard_file = start->file;
      }
      guard_hash = NULL;
      tok = skip_line(tok->next->next);
      if (defined)
        tok = skip_cond_incl(tok);
      continue;
    }

    if (equal(tok, "elif")) {
      if (!cond_incl || cond_incl->ctx == IN_ELSE)
        error_tok(start, "stray #elif");
      cond_incl->ctx = IN_ELIF;

      if (!cond_incl->included && eval_const_expr(&tok, tok))
        cond_incl->included = true;
      else
        tok = skip_cond_incl(tok);
      continue;
    }

    if (equal(tok, "else")) {
      if (!cond_incl || cond_incl->ctx == IN_ELSE)
        error_tok(start, "stray #else");
      cond_incl->ctx = IN_ELSE;
      tok = skip_line(tok->next);

      if (cond_incl->included)
        tok = skip_cond_incl(tok);
      continue;
    }

    if (equal(tok, "endif")) {
      if (!cond_incl)
        error_tok(start, "stray #endif");
      CondIncl *ci = cond_incl;
      cond_incl = ci->next;
      tok = skip_line(tok->next);
      // The guard's #endif ends the file: including it again does nothing while FOO is defined.
      if (ci->guard && ci->ctx == IN_THEN &&
          (more(tok)->kind == TK_EOF || tok->file != ci->guard_file))
        hashmap_put(&include_guards, ci->guard_path, ci->guard);
      continue;
    }

    if (equal(tok, "line")) {
      read_line_marker(&tok, tok->next);
      continue;
    }

    if (tok->kind == TK_PP_NUM) {
      read_line_marker(&tok, tok);
      continue;
    }

    if (equal(tok, "pragma") && equal(tok->next, "once")) {
      hashmap_put(&pragma_once, tok->file->name, (void *)1);
      tok = skip_line(tok->next->next);
      continue;
    }

    if (equal(tok, "pragma")) {
      do {
        tok = tok->next;
      } while (!tok->at_bol);
      continue;
    }

    if (equal(tok, "error")) {
      // The message is the rest of the line as written, like gcc's "#error stop here".
      char *s = tok->loc + tok->len;
      while (*s == ' ' || *s == '\t')
        s++;
      int n = strcspn(s, "\r\n");
      error_tok(tok, "#error %.*s", n, s);
    }

    // `#`-only line is legal. It's called a null directive.
    if (tok->at_bol)
      continue;

    error_tok(tok, "invalid preprocessor directive");
  }

  if (top && cond_incl)
    error_tok(cond_incl->tok, "unterminated conditional directive");
  cur->next = top ? keep_token(tok) : tok;
  depth--;
  return head.next;
}

void define_macro(char *name, char *buf) {
  Token *tok = tokenize(new_file("<built-in>", 1, buf));
  add_macro(name, true, tok);
}

void undef_macro(char *name) {
  hashmap_delete(&macros, name);
}

static Macro *add_builtin(char *name, macro_handler_fn *fn) {
  Macro *m = add_macro(name, true, NULL);
  m->handler = fn;
  return m;
}

static Token *file_macro(Token *tmpl) {
  while (tmpl->origin)
    tmpl = tmpl->origin;
  return new_str_token(tmpl->file->display_name, tmpl);
}

static Token *line_macro(Token *tmpl) {
  while (tmpl->origin)
    tmpl = tmpl->origin;
  int i = tmpl->line_no + tmpl->file->line_delta;
  return new_num_token(i, tmpl);
}

// __COUNTER__ is expanded to serial values starting from 0.
static Token *counter_macro(Token *tmpl) {
  return new_num_token(counter++, tmpl);
}

// __TIMESTAMP__ is expanded to a string describing the last
// modification time of the current file. E.g.
// "Fri Jul 24 01:32:50 2020"
// (Files are not stat'ed; the value is a fixed placeholder.)
static Token *timestamp_macro(Token *tmpl) {
  return new_str_token("??? ??? ?? ??:??:?? ????", tmpl);
}

static Token *base_file_macro(Token *tmpl) {
  return new_str_token(base_file, tmpl);
}

// __DATE__ is expanded to the current date, e.g. "May 17 2020".
static char *format_date(struct tm *tm) {
  static char mon[][4] = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec",
  };

  return format("\"%s %2d %d\"", mon[tm->tm_mon], tm->tm_mday, tm->tm_year + 1900);
}

// __TIME__ is expanded to the current time, e.g. "13:34:03".
static char *format_time(struct tm *tm) {
  return format("\"%02d:%02d:%02d\"", tm->tm_hour, tm->tm_min, tm->tm_sec);
}

void init_macros(void) {
  // Predefined macros for the RV32IMFC ilp32f target (as riscv32-esp-elf-gcc
  // defines them where headers care).
  define_macro("__C99_MACRO_WITH_VA_ARGS", "1");
  define_macro("__ELF__", "1");
  define_macro("__ILP32__", "1");
  define_macro("_ILP32", "1");
  define_macro("__riscv", "1");
  define_macro("__riscv_xlen", "32");
  define_macro("__riscv_flen", "32");
  define_macro("__riscv_float_abi_single", "1");
  define_macro("__riscv_mul", "1");
  define_macro("__riscv_div", "1");
  define_macro("__riscv_compressed", "1");
  define_macro("__CHAR_UNSIGNED__", "1");
  define_macro("__CHAR_BIT__", "8");
  define_macro("__SIZEOF_DOUBLE__", "8");
  define_macro("__SIZEOF_FLOAT__", "4");
  define_macro("__SIZEOF_INT__", "4");
  define_macro("__SIZEOF_LONG_DOUBLE__", "8");
  define_macro("__SIZEOF_LONG_LONG__", "8");
  define_macro("__SIZEOF_LONG__", "4");
  define_macro("__SIZEOF_POINTER__", "4");
  define_macro("__SIZEOF_PTRDIFF_T__", "4");
  define_macro("__SIZEOF_SHORT__", "2");
  define_macro("__SIZEOF_SIZE_T__", "4");
  define_macro("__SIZEOF_WCHAR_T__", "4");
  define_macro("__SIZE_TYPE__", "unsigned int");
  define_macro("__PTRDIFF_TYPE__", "int");
  define_macro("__WCHAR_TYPE__", "int");
  define_macro("__INT_MAX__", "0x7fffffff");
  define_macro("__LONG_MAX__", "0x7fffffffL");
  define_macro("__LONG_LONG_MAX__", "0x7fffffffffffffffLL");
  define_macro("__STDC_HOSTED__", "1");
  define_macro("__STDC_NO_ATOMICS__", "1");
  define_macro("__STDC_NO_COMPLEX__", "1");
  define_macro("__STDC_NO_THREADS__", "1");
  define_macro("__STDC_UTF_16__", "1");
  define_macro("__STDC_UTF_32__", "1");
  define_macro("__STDC_VERSION__", "201112L");
  define_macro("__STDC__", "1");
  define_macro("__USER_LABEL_PREFIX__", "");
  define_macro("__alignof__", "_Alignof");
  define_macro("__chibicc__", "1");
  define_macro("__PICOCALC__", "1");
  define_macro("__const__", "const");
  define_macro("__const", "const");
  define_macro("__inline__", "inline");
  define_macro("__inline", "inline");
  define_macro("__signed__", "signed");
  define_macro("__typeof__", "typeof");
  define_macro("__volatile__", "volatile");
  define_macro("__extension__", "");
  define_macro("__asm__", "asm");

  add_builtin("__FILE__", file_macro);
  add_builtin("__LINE__", line_macro);
  add_builtin("__COUNTER__", counter_macro);
  add_builtin("__TIMESTAMP__", timestamp_macro);
  add_builtin("__BASE_FILE__", base_file_macro);

  time_t now = time(NULL);
  struct tm *tm = localtime(&now);
  define_macro("__DATE__", format_date(tm));
  define_macro("__TIME__", format_time(tm));
}

typedef enum {
  STR_NONE, STR_UTF8, STR_UTF16, STR_UTF32, STR_WIDE,
} StringKind;

static StringKind getStringKind(Token *tok) {
  if (!strcmp(tok->loc, "u8"))
    return STR_UTF8;

  switch (tok->loc[0]) {
  case '"': return STR_NONE;
  case 'u': return STR_UTF16;
  case 'U': return STR_UTF32;
  case 'L': return STR_WIDE;
  }
  unreachable();
}

// Concatenate adjacent string literals into a single string literal
// as per the C spec.
static void join_adjacent_string_literals(Token *tok) {
  // First pass: If regular string literals are adjacent to wide
  // string literals, regular string literals are converted to a wide
  // type before concatenation. In this pass, we do the conversion.
  for (Token *tok1 = tok; tok1->kind != TK_EOF && tok1->kind != TK_MORE;) {
    if (tok1->kind != TK_STR || tok1->next->kind != TK_STR) {
      tok1 = tok1->next;
      continue;
    }

    StringKind kind = getStringKind(tok1);
    Type *basety = tok1->ty->base;

    for (Token *t = tok1->next; t->kind == TK_STR; t = t->next) {
      StringKind k = getStringKind(t);
      if (kind == STR_NONE) {
        kind = k;
        basety = t->ty->base;
      } else if (k != STR_NONE && kind != k) {
        error_tok(t, "unsupported non-standard concatenation of string literals");
      }
    }

    if (basety->size > 1)
      for (Token *t = tok1; t->kind == TK_STR; t = t->next)
        if (t->ty->base->size == 1)
          *t = *tokenize_string_literal(t, basety);

    while (tok1->kind == TK_STR)
      tok1 = tok1->next;
  }

  // Second pass: concatenate adjacent string literals.
  for (Token *tok1 = tok; tok1->kind != TK_EOF && tok1->kind != TK_MORE;) {
    if (tok1->kind != TK_STR || tok1->next->kind != TK_STR) {
      tok1 = tok1->next;
      continue;
    }

    Token *tok2 = tok1->next;
    while (tok2->kind == TK_STR)
      tok2 = tok2->next;

    int len = tok1->ty->array_len;
    for (Token *t = tok1->next; t != tok2; t = t->next)
      len = len + t->ty->array_len - 1;

    char *buf = calloc(tok1->ty->base->size, len);

    int i = 0;
    for (Token *t = tok1; t != tok2; t = t->next) {
      memcpy(buf + i, t->str, t->ty->size);
      i = i + t->ty->size - t->ty->base->size;
    }

    *tok1 = *copy_token(tok1);
    tok1->ty = array_of(tok1->ty->base, len);
    tok1->str = buf;
    tok1->next = tok2;
    tok1 = tok2;
  }
}

static Token *finish(Token *tok, bool top) {
  bool keep = cc_keep(top);  // the top-level list is in main; so must be what these build
  convert_pp_tokens(tok);
  join_adjacent_string_literals(tok);
  cc_keep(keep);
  return tok;
}

// Entry point function of the preprocessor. At the top level it returns only the first top-level
// declaration; the parser gets the rest from preprocess_more().
Token *preprocess(Token *tok) {
  return finish(preprocess2(tok), depth == 0);
}

// Replaces t, the TK_MORE token ending the parser's last declaration, with the next one.
Token *preprocess_more(Token *t) {
  bool keep = cc_keep(false);
  Token *first = finish(preprocess2(pp_rest), true);
  cc_keep(keep);
  *t = *first;
  cc_tok_free(first);
  return t;
}

// Recycles the tokens [tok, end) of a parsed declaration and the origin roots copied for them,
// except pinned ones; with keep, all of them stay (a function compiled only at the end).
void preprocess_release(Token *tok, Token *end, bool keep) {
  for (Token *next; !keep && tok != end; tok = next) {
    next = tok->next;
    if (!tok->pinned)
      cc_tok_free(tok);
  }
  for (Token *r = batch_roots, *next; !keep && r; r = next) {
    next = r->next;
    if (!r->pinned)
      cc_tok_free(r);
  }
  batch_roots = NULL;
}

// Keeps a token (and its origin root) alive after its declaration, for a type that refers to it.
void pin_token(Token *tok) {
  tok->pinned = true;
  if (tok->origin)
    tok->origin->pinned = true;
}
