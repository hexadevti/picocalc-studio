#include "chibicc.h"

// Input file
static File *current_file;
static int line_no;  // line being lexed

// A list of all input files.
static File **input_files;

// True if the current position is at the beginning of a line
static bool at_bol;

// True if the current position follows a space character
static bool has_space;

jmp_buf cc_error_jmp;
char cc_errbuf[512];

// Reports an error without a source location and unwinds to cc_error_jmp.
void error(char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int n = snprintf(cc_errbuf, sizeof(cc_errbuf), "error: ");
  vsnprintf(cc_errbuf + n, sizeof(cc_errbuf) - n, fmt, ap);
  va_end(ap);
  longjmp(cc_error_jmp, 1);
}

// Formats a diagnostic into buf as
//
// foo.c:10:7: error: <message>
//   x = y + 1;
//       ^
static void verror_at(char *buf, int bufsize, char *kind, char *filename, char *input,
                      int line_no, char *loc, char *fmt, va_list ap) {
  // Find a line containing `loc`.
  char *line = loc;
  while (input < line && line[-1] != '\n')
    line--;

  char *end = loc;
  while (*end && *end != '\n')
    end++;

  int col = display_width(line, loc - line);
  int n = snprintf(buf, bufsize, "%s:%d:%d: %s: ", filename, line_no, col + 1, kind);
  if (n < bufsize)
    n += vsnprintf(buf + n, bufsize - n, fmt, ap);
  if (n < bufsize)
    snprintf(buf + n, bufsize - n, "\n  %.*s\n  %*s^\n", (int)(end - line), line, col, "");
}

void error_at(char *loc, char *fmt, ...) {
  int line_no = 1;
  for (char *p = current_file->contents; p < loc; p++)
    if (*p == '\n')
      line_no++;

  va_list ap;
  va_start(ap, fmt);
  verror_at(cc_errbuf, sizeof(cc_errbuf), "error", current_file->name, current_file->contents,
            line_no, loc, fmt, ap);
  va_end(ap);
  longjmp(cc_error_jmp, 1);
}

void error_tok(Token *tok, char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  verror_at(cc_errbuf, sizeof(cc_errbuf), "error", tok->file->name, tok->file->contents,
            tok->line_no, tok->loc, fmt, ap);
  va_end(ap);
  longjmp(cc_error_jmp, 1);
}

void warn_tok(Token *tok, char *fmt, ...) {
  char buf[512];
  va_list ap;
  va_start(ap, fmt);
  verror_at(buf, sizeof(buf), "warning", tok->file->name, tok->file->contents,
            tok->line_no, tok->loc, fmt, ap);
  va_end(ap);
  cc_diag("%s", buf);
}

// Consumes the current token if it matches `op`.
bool equal(Token *tok, char *op) {
  return memcmp(tok->loc, op, tok->len) == 0 && op[tok->len] == '\0';
}

// Ensure that the current token is `op`.
Token *skip(Token *tok, char *op) {
  if (!equal(tok, op))
    error_tok(tok, "expected '%s'", op);
  return tok->next;
}

bool consume(Token **rest, Token *tok, char *str) {
  if (equal(tok, str)) {
    *rest = tok->next;
    return true;
  }
  *rest = tok;
  return false;
}

// Create a new token.
static Token *new_token(TokenKind kind, char *start, char *end) {
  Token *tok = cc_tok_alloc();
  tok->kind = kind;
  tok->loc = start;
  if (end - start >= 1 << 21)  // Token::len is 22 bits
    error_at(start, "token too long");
  tok->len = end - start;
  tok->file = current_file;
  tok->line_no = line_no;
  tok->at_bol = at_bol;
  tok->has_space = has_space;

  at_bol = has_space = false;
  return tok;
}

static bool startswith(char *p, char *q) {
  return strncmp(p, q, strlen(q)) == 0;
}

// Read an identifier and returns the length of it.
// If p does not point to a valid identifier, 0 is returned.
static int read_ident(char *start) {
  char *p = start;
  uint32_t c = decode_utf8(&p, p);
  if (!is_ident1(c))
    return 0;

  for (;;) {
    char *q;
    c = decode_utf8(&q, p);
    if (!is_ident2(c))
      return p - start;
    p = q;
  }
}

static int from_hex(char c) {
  if ('0' <= c && c <= '9')
    return c - '0';
  if ('a' <= c && c <= 'f')
    return c - 'a' + 10;
  return c - 'A' + 10;
}

// Read a punctuator token from p and returns its length.
static int read_punct(char *p) {
  static char *kw[] = {
    "<<=", ">>=", "...", "==", "!=", "<=", ">=", "->", "+=",
    "-=", "*=", "/=", "++", "--", "%=", "&=", "|=", "^=", "&&",
    "||", "<<", ">>", "##",
  };

  for (int i = 0; i < sizeof(kw) / sizeof(*kw); i++)
    if (startswith(p, kw[i]))
      return strlen(kw[i]);

  return ispunct(*p) ? 1 : 0;
}

static HashMap keyword_map;

static bool is_keyword(Token *tok) {
  HashMap *map = &keyword_map;

  if (map->capacity == 0) {
    static char *kw[] = {
      "return", "if", "else", "for", "while", "int", "sizeof", "char",
      "struct", "union", "short", "long", "void", "typedef", "_Bool",
      "enum", "static", "goto", "break", "continue", "switch", "case",
      "default", "extern", "_Alignof", "_Alignas", "do", "signed",
      "unsigned", "const", "volatile", "auto", "register", "restrict",
      "__restrict", "__restrict__", "_Noreturn", "float", "double",
      "typeof", "asm", "_Thread_local", "__thread", "_Atomic",
      "__attribute__",
    };

    for (int i = 0; i < sizeof(kw) / sizeof(*kw); i++)
      hashmap_put(map, kw[i], (void *)1);
  }

  return hashmap_get2(map, tok->loc, tok->len);
}

static int read_escaped_char(char **new_pos, char *p) {
  if ('0' <= *p && *p <= '7') {
    // Read an octal number.
    int c = *p++ - '0';
    if ('0' <= *p && *p <= '7') {
      c = (c << 3) + (*p++ - '0');
      if ('0' <= *p && *p <= '7')
        c = (c << 3) + (*p++ - '0');
    }
    *new_pos = p;
    return c;
  }

  if (*p == 'x') {
    // Read a hexadecimal number.
    p++;
    if (!isxdigit(*p))
      error_at(p, "invalid hex escape sequence");

    int c = 0;
    for (; isxdigit(*p); p++)
      c = (c << 4) + from_hex(*p);
    *new_pos = p;
    return c;
  }

  *new_pos = p + 1;

  // Escape sequences are defined using themselves here. E.g.
  // '\n' is implemented using '\n'. This tautological definition
  // works because the compiler that compiles our compiler knows
  // what '\n' actually is. In other words, we "inherit" the ASCII
  // code of '\n' from the compiler that compiles our compiler,
  // so we don't have to teach the actual code here.
  //
  // This fact has huge implications not only for the correctness
  // of the compiler but also for the security of the generated code.
  // For more info, read "Reflections on Trusting Trust" by Ken Thompson.
  // https://github.com/rui314/chibicc/wiki/thompson1984.pdf
  switch (*p) {
  case 'a': return '\a';
  case 'b': return '\b';
  case 't': return '\t';
  case 'n': return '\n';
  case 'v': return '\v';
  case 'f': return '\f';
  case 'r': return '\r';
  // [GNU] \e for the ASCII escape character is a GNU C extension.
  case 'e': return 27;
  default: return *p;
  }
}

// Find a closing double-quote (NULL if the line ends first).
static char *string_literal_end(char *p) {
  for (; *p != '"'; p++) {
    if (*p == '\n' || *p == '\0')
      return NULL;
    if (*p == '\\' && p[1] != '\n' && p[1] != '\0')
      p++;
  }
  return p;
}

// An unterminated literal is only an error if it survives preprocessing (a skipped
// #if 0 block may contain prose with apostrophes), so it becomes a token covering the rest
// of the line that convert_pp_tokens() rejects.
static Token *bad_literal(char *start, char *msg) {
  char *end = start;
  while (*end && *end != '\n')
    end++;
  Token *tok = new_token(TK_PUNCT, start, end);
  tok->bad_literal = msg;
  return tok;
}

// The Type of a narrow string literal of len chars; short ones share one per length, so a table
// of strings does not take a Type each. Nothing modifies these (join_adjacent_string_literals()
// makes a new one).
static Type *str_types[32];

static Type *str_type(int len) {
  if (len >= 32)
    return array_of(ty_uchar, len);
  if (!str_types[len]) {
    bool fn_types = cc_fn_types(false);
    str_types[len] = array_of(ty_uchar, len);
    cc_fn_types(fn_types);
  }
  return str_types[len];
}

static Token *read_string_literal(char *start, char *quote) {
  char *end = string_literal_end(quote + 1);
  if (!end)
    return bad_literal(start, "unclosed string literal");
  char *buf = calloc(1, end - quote);
  int len = 0;

  for (char *p = quote + 1; p < end;) {
    if (*p == '\\')
      buf[len++] = read_escaped_char(&p, p + 1);
    else
      buf[len++] = *p++;
  }

  Token *tok = new_token(TK_STR, start, end + 1);
  tok->ty = str_type(len + 1);
  tok->str = buf;
  return tok;
}

// Read a UTF-8-encoded string literal and transcode it in UTF-16.
//
// UTF-16 is yet another variable-width encoding for Unicode. Code
// points smaller than U+10000 are encoded in 2 bytes. Code points
// equal to or larger than that are encoded in 4 bytes. Each 2 bytes
// in the 4 byte sequence is called "surrogate", and a 4 byte sequence
// is called a "surrogate pair".
static Token *read_utf16_string_literal(char *start, char *quote) {
  char *end = string_literal_end(quote + 1);
  if (!end)
    return bad_literal(start, "unclosed string literal");
  uint16_t *buf = calloc(2, end - start);
  int len = 0;

  for (char *p = quote + 1; p < end;) {
    if (*p == '\\') {
      buf[len++] = read_escaped_char(&p, p + 1);
      continue;
    }

    uint32_t c = decode_utf8(&p, p);
    if (c < 0x10000) {
      // Encode a code point in 2 bytes.
      buf[len++] = c;
    } else {
      // Encode a code point in 4 bytes.
      c -= 0x10000;
      buf[len++] = 0xd800 + ((c >> 10) & 0x3ff);
      buf[len++] = 0xdc00 + (c & 0x3ff);
    }
  }

  Token *tok = new_token(TK_STR, start, end + 1);
  tok->ty = array_of(ty_ushort, len + 1);
  tok->str = (char *)buf;
  return tok;
}

// Read a UTF-8-encoded string literal and transcode it in UTF-32.
//
// UTF-32 is a fixed-width encoding for Unicode. Each code point is
// encoded in 4 bytes.
static Token *read_utf32_string_literal(char *start, char *quote, Type *ty) {
  char *end = string_literal_end(quote + 1);
  if (!end)
    return bad_literal(start, "unclosed string literal");
  uint32_t *buf = calloc(4, end - quote);
  int len = 0;

  for (char *p = quote + 1; p < end;) {
    if (*p == '\\')
      buf[len++] = read_escaped_char(&p, p + 1);
    else
      buf[len++] = decode_utf8(&p, p);
  }

  Token *tok = new_token(TK_STR, start, end + 1);
  tok->ty = array_of(ty, len + 1);
  tok->str = (char *)buf;
  return tok;
}

static Token *read_char_literal(char *start, char *quote, Type *ty) {
  char *p = quote + 1;
  if (*p == '\0' || *p == '\n')
    return bad_literal(start, "unclosed char literal");

  int c;
  if (*p == '\\')
    c = read_escaped_char(&p, p + 1);
  else
    c = decode_utf8(&p, p);

  char *end = p;
  while (*end && *end != '\n' && *end != '\'')
    end++;
  if (*end != '\'')
    return bad_literal(start, "unclosed char literal");

  Token *tok = new_token(TK_NUM, start, end + 1);
  tok->val = c;
  tok->ty = ty;
  return tok;
}

static bool convert_pp_int(Token *tok) {
  char *p = tok->loc;

  // Read a binary, octal, decimal or hexadecimal number.
  int base = 10;
  if (!strncasecmp(p, "0x", 2) && isxdigit(p[2])) {
    p += 2;
    base = 16;
  } else if (!strncasecmp(p, "0b", 2) && (p[2] == '0' || p[2] == '1')) {
    p += 2;
    base = 2;
  } else if (*p == '0') {
    base = 8;
  }

  uint64_t val = strtoull(p, &p, base);

  // Read U, L or LL suffixes. `long` is 32 bits on this target, so a single L
  // changes nothing; only LL selects TY_LONG (long long).
  bool l = false;
  bool u = false;

  if (startswith(p, "LLU") || startswith(p, "LLu") ||
      startswith(p, "llU") || startswith(p, "llu") ||
      startswith(p, "ULL") || startswith(p, "Ull") ||
      startswith(p, "uLL") || startswith(p, "ull")) {
    p += 3;
    l = u = true;
  } else if (!strncasecmp(p, "lu", 2) || !strncasecmp(p, "ul", 2)) {
    p += 2;
    u = true;
  } else if (startswith(p, "LL") || startswith(p, "ll")) {
    p += 2;
    l = true;
  } else if (*p == 'L' || *p == 'l') {
    p++;
  } else if (*p == 'U' || *p == 'u') {
    p++;
    u = true;
  }

  if (p != tok->loc + tok->len)
    return false;

  // Infer a type.
  Type *ty;
  if (l && u)
    ty = ty_ulong;
  else if (l)
    ty = (base != 10 && (val >> 63)) ? ty_ulong : ty_long;
  else if (u)
    ty = (val >> 32) ? ty_ulong : ty_uint;
  else if (base == 10)
    ty = (val >> 31) ? ty_long : ty_int;
  else if (val >> 63)
    ty = ty_ulong;
  else if (val >> 32)
    ty = ty_long;
  else if (val >> 31)
    ty = ty_uint;
  else
    ty = ty_int;

  tok->kind = TK_NUM;
  tok->val = val;
  tok->ty = ty;
  return true;
}

// The definition of the numeric literal at the preprocessing stage
// is more relaxed than the definition of that at the later stages.
// In order to handle that, a numeric literal is tokenized as a
// "pp-number" token first and then converted to a regular number
// token after preprocessing.
//
// This function converts a pp-number token to a regular number token.
static void convert_pp_number(Token *tok) {
  // Try to parse as an integer constant.
  if (convert_pp_int(tok))
    return;

  // If it's not an integer, it must be a floating point constant.
  char *end;
  double val = strtod(tok->loc, &end);

  Type *ty;
  if (*end == 'f' || *end == 'F') {
    ty = ty_float;
    end++;
  } else if (*end == 'l' || *end == 'L') {
    ty = ty_ldouble;
    end++;
  } else {
    ty = ty_double;
  }

  if (tok->loc + tok->len != end)
    error_tok(tok, "invalid numeric constant");

  tok->kind = TK_NUM;
  tok->fval = val;
  tok->ty = ty;
}

void convert_pp_tokens(Token *tok) {
  for (Token *t = tok; t->kind != TK_EOF && t->kind != TK_MORE; t = t->next) {
    if (t->kind == TK_PUNCT && t->bad_literal)
      error_tok(t, "%s", t->bad_literal);
    if (is_keyword(t))
      t->kind = TK_KEYWORD;
    else if (t->kind == TK_PP_NUM)
      convert_pp_number(t);
  }
}

Token *tokenize_string_literal(Token *tok, Type *basety) {
  Token *t;
  if (basety->size == 2)
    t = read_utf16_string_literal(tok->loc, tok->loc);
  else
    t = read_utf32_string_literal(tok->loc, tok->loc, basety);
  t->file = tok->file;
  t->line_no = tok->line_no;
  t->next = tok->next;
  return t;
}

// Lines lexed per chunk of a lazily tokenized file, and at most about this many tokens of one.
#define LEX_LINES 32
#define LEX_STEPS 1024

// Is the line starting at p a directive? (Or could it be, after a comment.)
static bool directive_line(char *p) {
  while (*p == ' ' || *p == '\t')
    p++;
  return *p == '#' || *p == '/';
}

// Is the line starting at p an #include? A chunk ends after one, so the includer's next lines
// are not held as tokens while the included file is read.
static bool include_line(char *p) {
  while (*p == ' ' || *p == '	')
    p++;
  if (*p++ != '#')
    return false;
  while (*p == ' ' || *p == '	')
    p++;
  return startswith(p, "include");
}

// Tokenizes file from p, which starts line `line`. Lazily, it stops at the first line break
// after LEX_LINES lines with a TK_MORE token that tokenize_more() resumes from, so raw tokens are
// produced only as the preprocessor gets to them and their slots can be recycled. A long line is
// cut between tokens too, but a directive line is always whole. At the end of the file the list
// continues with file->after, or an EOF token. With one_line, it stops at the end of the line with
// an EOF. bol and space are what the first token gets (where p is in its line).
static Token *lex(File *file, char *p, int line, bool lazy, bool one_line, bool bol, bool space) {
  current_file = file;
  line_no = line;
  int stop = line + LEX_LINES, steps = 0;
  char *line_start = p;

  Token head = {};
  Token *cur = &head;

  at_bol = bol;
  has_space = space;

  while (*p) {
    if (lazy && ++steps > LEX_STEPS && *p != '\n' && !directive_line(line_start)) {
      cur->next = new_token(TK_MORE, p, p);  // keeps at_bol and has_space for the resumption
      return head.next;
    }

    // Skip line comments.
    if (startswith(p, "//")) {
      p += 2;
      while (*p != '\n')
        p++;
      has_space = true;
      continue;
    }

    // Skip block comments.
    if (startswith(p, "/*")) {
      char *q = strstr(p + 2, "*/");
      if (!q)
        error_at(p, "unclosed block comment");
      for (; p < q; p++)
        if (*p == '\n')
          line_no++;
      p = q + 2;
      has_space = true;
      continue;
    }

    // Skip newline.
    if (*p == '\n') {
      if (one_line)
        break;
      p++;
      line_no++;
      at_bol = true;
      has_space = false;
      bool after_include = lazy && include_line(line_start);
      line_start = p;
      if (lazy && (line_no >= stop || after_include) && *p) {
        cur->next = new_token(TK_MORE, p, p);
        return head.next;
      }
      continue;
    }

    // Skip whitespace characters.
    if (isspace(*p)) {
      p++;
      has_space = true;
      continue;
    }

    // Numeric literal
    if (isdigit(*p) || (*p == '.' && isdigit(p[1]))) {
      char *q = p++;
      for (;;) {
        if (p[0] && p[1] && strchr("eEpP", p[0]) && strchr("+-", p[1]))
          p += 2;
        else if (isalnum(*p) || *p == '.')
          p++;
        else
          break;
      }
      cur = cur->next = new_token(TK_PP_NUM, q, p);
      continue;
    }

    // String literal
    if (*p == '"') {
      cur = cur->next = read_string_literal(p, p);
      p += cur->len;
      continue;
    }

    // UTF-8 string literal
    if (startswith(p, "u8\"")) {
      cur = cur->next = read_string_literal(p, p + 2);
      p += cur->len;
      continue;
    }

    // UTF-16 string literal
    if (startswith(p, "u\"")) {
      cur = cur->next = read_utf16_string_literal(p, p + 1);
      p += cur->len;
      continue;
    }

    // Wide string literal
    if (startswith(p, "L\"")) {
      cur = cur->next = read_utf32_string_literal(p, p + 1, ty_int);
      p += cur->len;
      continue;
    }

    // UTF-32 string literal
    if (startswith(p, "U\"")) {
      cur = cur->next = read_utf32_string_literal(p, p + 1, ty_uint);
      p += cur->len;
      continue;
    }

    // Character literal
    if (*p == '\'') {
      cur = cur->next = read_char_literal(p, p, ty_int);
      cur->val = (unsigned char)cur->val; // plain char is unsigned on RISC-V
      p += cur->len;
      continue;
    }

    // UTF-16 character literal
    if (startswith(p, "u'")) {
      cur = cur->next = read_char_literal(p, p + 1, ty_ushort);
      cur->val &= 0xffff;
      p += cur->len;
      continue;
    }

    // Wide character literal
    if (startswith(p, "L'")) {
      cur = cur->next = read_char_literal(p, p + 1, ty_int);
      p += cur->len;
      continue;
    }

    // UTF-32 character literal
    if (startswith(p, "U'")) {
      cur = cur->next = read_char_literal(p, p + 1, ty_uint);
      p += cur->len;
      continue;
    }

    // Identifier or keyword
    int ident_len = read_ident(p);
    if (ident_len) {
      cur = cur->next = new_token(TK_IDENT, p, p + ident_len);
      p += cur->len;
      continue;
    }

    // Punctuators
    int punct_len = read_punct(p);
    if (punct_len) {
      cur = cur->next = new_token(TK_PUNCT, p, p + punct_len);
      p += cur->len;
      continue;
    }

    error_at(p, "invalid token");
  }

  cur->next = file->after && !one_line ? file->after : new_token(TK_EOF, p, p);
  return head.next;
}

// Tokenizes the rest of the line at p (a macro body) again, ending it with an EOF.
Token *tokenize_line(File *file, char *p, int line) {
  return lex(file, p, line, false, true, true, false);
}

// Tokenize a given string and returns new tokens.
Token *tokenize(File *file) {
  return lex(file, file->contents, 1, false, false, true, false);
}

// Replaces a TK_MORE token, in place, with the first token of the next chunk (which may be
// another TK_MORE, or the includer's token after the file), and recycles that token's slot.
void tokenize_more(Token *tok) {
  bool track = cc_tok_track(false);  // the input, not part of a macro expansion
  Token *first = lex(tok->file, tok->loc, tok->line_no, true, false, tok->at_bol, tok->has_space);
  *tok = *first;
  cc_tok_free(first);
  cc_tok_track(track);
}

// Returns the contents of a given file. cc_read_file_hook, when set, is asked
// first (the IDE uses it to compile unsaved editor buffers).
char *(*cc_read_file_hook)(const char *path, size_t *len);

static char *read_file(char *path) {
  size_t len = 0;
  char *data = cc_read_file_hook ? cc_read_file_hook(path, &len) : NULL;
  char *buf;

  if (data) {
    buf = calloc(1, len + 2);
    memcpy(buf, data, len);
  } else {
    FILE *fp = fopen(path, "rb");
    if (!fp)
      return NULL;
    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (size < 0) {
      fclose(fp);
      return NULL;
    }
    buf = calloc(1, size + 2);
    len = fread(buf, 1, size, fp);
    fclose(fp);
  }

  // Make sure that the last line is properly terminated with '\n'.
  if (len == 0 || buf[len - 1] != '\n')
    buf[len++] = '\n';
  buf[len] = '\0';
  return buf;
}

File **get_input_files(void) {
  return input_files;
}

// Files outlive the preprocessor's scratch arena (tokens point into their contents), so they and
// their contents always go in the main arena. name must already live there.
File *new_file(char *name, int file_no, char *contents) {
  bool keep = cc_keep(true);
  File *file = calloc(1, sizeof(File));
  file->name = name;
  file->display_name = name;
  file->file_no = file_no;
  file->contents = strdup(contents);
  cc_keep(keep);
  return file;
}

// Replaces \r or \r\n with \n.
static void canonicalize_newline(char *p) {
  int i = 0, j = 0;

  while (p[i]) {
    if (p[i] == '\r' && p[i + 1] == '\n') {
      i += 2;
      p[j++] = '\n';
    } else if (p[i] == '\r') {
      i++;
      p[j++] = '\n';
    } else {
      p[j++] = p[i++];
    }
  }

  p[j] = '\0';
}

// Removes backslashes followed by a newline.
static void remove_backslash_newline(char *p) {
  int i = 0, j = 0;

  // We want to keep the number of newline characters so that
  // the logical line number matches the physical one.
  // This counter maintain the number of newlines we have removed.
  int n = 0;

  while (p[i]) {
    if (p[i] == '\\' && p[i + 1] == '\n') {
      i += 2;
      n++;
    } else if (p[i] == '\n') {
      p[j++] = p[i++];
      for (; n > 0; n--)
        p[j++] = '\n';
    } else {
      p[j++] = p[i++];
    }
  }

  for (; n > 0; n--)
    p[j++] = '\n';
  p[j] = '\0';
}

static uint32_t read_universal_char(char *p, int len) {
  uint32_t c = 0;
  for (int i = 0; i < len; i++) {
    if (!isxdigit(p[i]))
      return 0;
    c = (c << 4) | from_hex(p[i]);
  }
  return c;
}

// Replace \u or \U escape sequences with corresponding UTF-8 bytes.
static void convert_universal_chars(char *p) {
  char *q = p;

  while (*p) {
    if (startswith(p, "\\u")) {
      uint32_t c = read_universal_char(p + 2, 4);
      if (c) {
        p += 6;
        q += encode_utf8(q, c);
      } else {
        *q++ = *p++;
      }
    } else if (startswith(p, "\\U")) {
      uint32_t c = read_universal_char(p + 2, 8);
      if (c) {
        p += 10;
        q += encode_utf8(q, c);
      } else {
        *q++ = *p++;
      }
    } else if (p[0] == '\\') {
      *q++ = *p++;
      *q++ = *p++;
    } else {
      *q++ = *p++;
    }
  }

  *q = '\0';
}

static int file_no;

void tokenize_reset(void) {
  current_file = NULL;
  input_files = NULL;
  file_no = 0;
  keyword_map = (HashMap){0};
  memset(str_types, 0, sizeof(str_types));
}

// Tokenizes a file lazily (see lex()); its tokens are followed by `after`, or EOF if NULL.
Token *tokenize_file(char *path, Token *after) {
  bool keep = cc_keep(true);  // the File and its contents outlive the scratch arena
  char *p = read_file(path);
  if (!p) {
    cc_keep(keep);
    return NULL;
  }

  // UTF-8 texts may start with a 3-byte "BOM" marker sequence.
  // If exists, just skip them because they are useless bytes.
  // (It is actually not recommended to add BOM markers to UTF-8
  // texts, but it's not uncommon particularly on Windows.)
  if (!memcmp(p, "\xef\xbb\xbf", 3))
    p += 3;

  canonicalize_newline(p);
  remove_backslash_newline(p);
  convert_universal_chars(p);

  File *file = calloc(1, sizeof(File));
  file->name = file->display_name = strdup(path);
  file->file_no = file_no + 1;
  file->contents = p;
  file->after = after;

  input_files = cc_realloc(input_files, sizeof(File *) * (file_no + 1),
                           sizeof(File *) * (file_no + 2));
  input_files[file_no] = file;
  input_files[file_no + 1] = NULL;
  file_no++;
  cc_keep(keep);

  return lex(file, p, 1, true, false, true, false);
}
