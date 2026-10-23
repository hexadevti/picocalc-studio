// chibicc (https://github.com/rui314/chibicc, MIT, see LICENSE) adapted for PicoCalc Studio:
// RV32 ilp32f target (4-byte pointers and long, 8-byte long long, long double = double),
// arena memory freed after each build, and errors that longjmp back to the caller instead of
// exiting. Internal header; users of the compiler include cc.h.
#include <assert.h>
#include <ctype.h>
#include <limits.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "cc.h"

#ifndef noreturn
# define noreturn _Noreturn
#endif

#define MAX(x, y) ((x) < (y) ? (y) : (x))
#define MIN(x, y) ((x) < (y) ? (x) : (y))

#ifndef __GNUC__
# define __attribute__(x)
#endif

//
// alloc.c: arena allocator. Everything the compiler allocates lives until cc_arena_free().
//

void *cc_calloc(size_t n, size_t size);
void *cc_realloc(void *p, size_t old_size, size_t new_size);
char *cc_strndup(const char *s, size_t n);
char *cc_strdup(const char *s);
void cc_arena_free(void);
size_t cc_arena_used(void);
size_t cc_arena_peak(void);  // most chunk memory held at once since cc_arena_free()
// Scratch arena for the preprocessor: allocations between begin and end are dropped at end.
void cc_scratch_begin(void);
void cc_scratch_end(void);
void *cc_tok_alloc(void);  // a zeroed Token, recycling freed scratch slots
void cc_tok_free(void *p);
bool cc_tok_track(bool on);  // record the tokens allocated from now on; returns the old setting
void cc_tok_sweep(void);     // ...free the unmarked ones and stop
void cc_fn_begin(void);  // allocate a function's Nodes and locals in their own arena...
void cc_fn_end(void);    // ...and drop them
bool cc_fn_is_open(void);
typedef struct { void *chunk, *next; size_t used, total; } CcArenaMark;
typedef struct { CcArenaMark fn, node; } CcFnMark;
CcFnMark cc_fn_mark(void);
void cc_fn_release(CcFnMark m);   // drop what the function arena and its Nodes got since the mark
void cc_node_release(CcFnMark m);  // ...only the Nodes
void *cc_fn_calloc(size_t size);
void *cc_node_calloc(size_t size);  // a Node: in a separate part of the function arena
bool cc_fn_types(bool on);       // Types in the function arena too (while one is open); returns the old setting
void *cc_type_calloc(size_t size);
bool cc_in_fn_arena(void *p);
void *cc_grow(void *p, size_t old_size, size_t new_size);
void cc_grow_free(void *p);  // frees what cc_grow() allocated
void *cc_grow_keep(void *p, size_t n);
void *cc_image_alloc(size_t n);  // the linked image: lent memory first, else malloc()  // trims a grown array to n and gives it to the object
bool cc_keep(bool on);  // on: allocate in the main arena even inside a scratch section; returns the old setting

#define calloc(n, size) cc_calloc((n), (size))
#define strndup(s, n) cc_strndup((s), (n))
#define strdup(s) cc_strdup(s)

// Error recovery: error*() format the message into cc_errbuf and longjmp to cc_error_jmp.
extern jmp_buf cc_error_jmp;
extern char cc_errbuf[512];
void cc_diag(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

typedef struct Type Type;
typedef struct Node Node;
typedef struct Member Member;
typedef struct Relocation Relocation;
typedef struct Hideset Hideset;

//
// strings.c
//

typedef struct {
  char **data;
  int capacity;
  int len;
} StringArray;

void strarray_push(StringArray *arr, char *s);
char *format(char *fmt, ...) __attribute__((format(printf, 1, 2)));

//
// tokenize.c
//

// Token
typedef enum {
  TK_IDENT,   // Identifiers
  TK_PUNCT,   // Punctuators
  TK_KEYWORD, // Keywords
  TK_STR,     // String literals
  TK_NUM,     // Numeric literals
  TK_PP_NUM,  // Preprocessing numbers
  TK_EOF,     // End-of-file markers
  TK_MORE,    // Rest of a lazily tokenized file (more()), or of the program (preprocess_more())
} TokenKind;

typedef struct {
  char *name;
  int file_no;
  char *contents;

  // For #line directive
  char *display_name;
  int line_delta;

  struct Token *after;  // what follows the file's last token (the includer's next token), or NULL
} File;

// Token type
typedef struct Token Token;
// Packed for the device (40 bytes on RV32, was 72): tokens are most of the compiler's memory.
struct Token {
  Token *next;      // Next token
  char *loc;        // Token location
  int len : 22;        // Token length
  TokenKind kind : 6;  // Token kind
  bool at_bol : 1;     // True if this token is at beginning of line
  bool has_space : 1;  // True if this token follows a space character
  bool pinned : 1;     // still referred to once its declaration is parsed (pin_token())
  bool marked : 1;     // survives cc_tok_sweep()

  File *file;       // Source location
  int line_no;      // Line number (#line applied when the token leaves the preprocessor)
  Type *ty;         // Used if TK_NUM or TK_STR
  Hideset *hideset; // For macro expansion
  Token *origin;    // If this is expanded from a macro, the original token
  union {
    int64_t val;       // TK_NUM: integer value
    double fval;       // TK_NUM: floating-point value
    char *str;         // TK_STR: contents including terminating '\0'
    char *bad_literal; // TK_PUNCT: unterminated literal, the error to report if it reaches the parser
  };
};

noreturn void error(char *fmt, ...) __attribute__((format(printf, 1, 2)));
noreturn void cc_out_of_memory(const char *what, size_t n);
noreturn void error_at(char *loc, char *fmt, ...) __attribute__((format(printf, 2, 3)));
noreturn void error_tok(Token *tok, char *fmt, ...) __attribute__((format(printf, 2, 3)));
void check_stack(Token *tok);
void warn_tok(Token *tok, char *fmt, ...) __attribute__((format(printf, 2, 3)));
bool equal(Token *tok, char *op);
Token *skip(Token *tok, char *op);
bool consume(Token **rest, Token *tok, char *str);
void convert_pp_tokens(Token *tok);
File **get_input_files(void);
File *new_file(char *name, int file_no, char *contents);
Token *tokenize_string_literal(Token *tok, Type *basety);
Token *tokenize(File *file);
Token *tokenize_line(File *file, char *p, int line);
Token *tokenize_file(char *filename, Token *after);
void tokenize_more(Token *tok);

// Lexes the next chunk of the file if tok is where it stops (TK_MORE). Returns tok.
static inline Token *more(Token *tok) {
  while (tok->kind == TK_MORE)
    tokenize_more(tok);
  return tok;
}
void tokenize_reset(void);

#define unreachable() \
  error("internal error at %s:%d", __FILE__, __LINE__)

//
// preprocess.c
//

char *search_include_paths(char *filename);
void init_macros(void);
void define_macro(char *name, char *buf);
void undef_macro(char *name);
Token *preprocess(Token *tok);
Token *preprocess_more(Token *tok);
void preprocess_release(Token *tok, Token *end, bool keep);
void preprocess_release_init(Token *tok, Token *end);
void pin_token(Token *tok);
void preprocess_reset(void);

//
// parse.c
//

// Variable or function
typedef struct Obj Obj;
struct Obj {
  Obj *next;
  char *name;    // Variable name
  Type *ty;      // Type
  Token *tok;    // representative token
  int align;     // alignment
  bool is_local : 1; // local or global/function

  // Local variable
  bool is_byref : 1;   // struct parameter passed by reference: the slot holds its address
  bool addr_taken : 1;

  // Global variable or function
  bool is_function : 1;
  bool is_definition : 1;
  bool is_static : 1;

  // Global variable
  bool is_tentative : 1;
  bool is_tls : 1;

  // Function
  bool is_inline : 1;

  // Static inline function
  bool is_live : 1;
  bool is_root : 1;

  // Local variable
  int offset;
  int reg;        // code generator: register holding the variable (0 = in memory)
  int uses;       // code generator: uses, weighted by loop nesting

  // Global variable
  char *init_data;
  Relocation *rel;

  // Function
  Obj *params;
  Node *body;
  Obj *locals;
  Obj *va_area;
  Obj *alloca_bottom;
  int stack_size;

  // Static inline function
  StringArray refs;
};

// Global variable can be initialized either by a constant expression
// or a pointer to another global variable. This struct represents the
// latter.
typedef struct Relocation Relocation;
struct Relocation {
  Relocation *next;
  int offset;
  char *label;
  int addend;
};

// AST node
typedef enum {
  ND_NULL_EXPR, // Do nothing
  ND_ADD,       // +
  ND_SUB,       // -
  ND_MUL,       // *
  ND_DIV,       // /
  ND_NEG,       // unary -
  ND_MOD,       // %
  ND_BITAND,    // &
  ND_BITOR,     // |
  ND_BITXOR,    // ^
  ND_SHL,       // <<
  ND_SHR,       // >>
  ND_EQ,        // ==
  ND_NE,        // !=
  ND_LT,        // <
  ND_LE,        // <=
  ND_ASSIGN,    // =
  ND_COND,      // ?:
  ND_COMMA,     // ,
  ND_MEMBER,    // . (struct member access)
  ND_ADDR,      // unary &
  ND_DEREF,     // unary *
  ND_NOT,       // !
  ND_BITNOT,    // ~
  ND_LOGAND,    // &&
  ND_LOGOR,     // ||
  ND_RETURN,    // "return"
  ND_IF,        // "if"
  ND_FOR,       // "for" or "while"
  ND_DO,        // "do"
  ND_SWITCH,    // "switch"
  ND_CASE,      // "case"
  ND_BLOCK,     // { ... }
  ND_GOTO,      // "goto"
  ND_GOTO_EXPR, // "goto" labels-as-values
  ND_LABEL,     // Labeled statement
  ND_LABEL_VAL, // [GNU] Labels-as-values
  ND_FUNCALL,   // Function call
  ND_EXPR_STMT, // Expression statement
  ND_STMT_EXPR, // Statement expression
  ND_VAR,       // Variable
  ND_VLA_PTR,   // VLA designator
  ND_NUM,       // Integer
  ND_CAST,      // Type cast
  ND_MEMZERO,   // Zero-clear a stack variable
  ND_ASM,       // "asm"
  ND_CAS,       // Atomic compare-and-swap
  ND_EXCH,      // Atomic exchange
  ND_PACKED,    // A statement stored compactly (pack_stmt()), expanded by cc_unpack()
} NodeKind;

// AST node type
// Shrunk for the device (56 bytes on RV32, was 160): each kind only uses the fields of one group,
// so the groups share storage. Code that walks every child must switch on the kind (see add_type).
struct Node {
  NodeKind kind; // Node kind
  Node *next;    // Next node
  Type *ty;      // Type, e.g. int or pointer to int
  Token *tok;    // Representative token

  Node *lhs;     // Left-hand side
  Node *rhs;     // Right-hand side

  union {
    // ND_IF, ND_FOR, ND_DO, ND_SWITCH, ND_COND
    struct {
      Node *cond;
      Node *then;
      Node *els;
      union {
        Node *init;          // ND_FOR
        Node *default_case;  // ND_SWITCH
      };
      union {
        Node *inc;    // ND_FOR
        Node *cases;  // ND_SWITCH: its ND_CASEs, linked by case_next
      };
      // "break" and "continue" labels
      char *brk_label;
      char *cont_label;
    };

    // ND_BLOCK, ND_STMT_EXPR
    Node *body;

    // ND_MEMBER
    Member *member;

    // ND_FUNCALL
    struct {
      Type *func_ty;
      Node *args;
      Obj *ret_buffer;
    };

    // ND_GOTO, ND_LABEL, ND_LABEL_VAL, ND_CASE
    struct {
      char *label;
      char *unique_label;
      Node *goto_next;
      Node *case_next;
      int64_t begin;  // case range
      int64_t end;
    };

    // ND_ASM
    char *asm_str;

    // ND_CAS
    struct {
      Node *cas_addr;
      Node *cas_old;
      Node *cas_new;
    };

    // ND_VAR, ND_VLA_PTR, ND_MEMZERO
    Obj *var;

    // ND_PACKED: the encoded statement (an ND_PACKED node is allocated only up to here)
    unsigned char *packed;

    // ND_NUM
    struct {
      int64_t val;
      double fval;
    };
  };
};

// The kinds that use the cond/then/els group.
static inline bool is_ctl_node(Node *node) {
  return node->kind == ND_IF || node->kind == ND_FOR || node->kind == ND_DO ||
         node->kind == ND_SWITCH || node->kind == ND_COND;
}

Node *new_cast(Node *expr, Type *ty);
Node *cc_unpack(Node *node);  // an ND_PACKED's statement, in Nodes until cc_node_release()
Node *implicit_cast(Node *expr, Type *ty);
int64_t const_expr(Token **rest, Token *tok);
Obj *parse(Token *tok);
void parse_reset(void);

//
// type.c
//

typedef enum {
  TY_VOID,
  TY_BOOL,
  TY_CHAR,
  TY_SHORT,
  TY_INT,
  TY_LONG,
  TY_FLOAT,
  TY_DOUBLE,
  TY_LDOUBLE,
  TY_ENUM,
  TY_PTR,
  TY_FUNC,
  TY_ARRAY,
  TY_VLA, // variable-length array
  TY_STRUCT,
  TY_UNION,
} TypeKind;

// Shrunk for the device (40 bytes on RV32, was 68): the flags share a word with the kind and
// the per-kind fields share storage, so read them only after checking the kind.
struct Type {
  TypeKind kind : 8;
  bool is_unsigned : 1;  // unsigned or signed
  bool is_atomic : 1;    // true if _Atomic
  bool is_flexible : 1;  // struct/union: ends in a flexible array member
  bool is_packed : 1;    // struct/union
  bool is_variadic : 1;  // function
  int size;           // sizeof() value
  int align;          // alignment
  Type *origin;       // for type compatibility check

  // Pointer-to or array-of type. We intentionally use the same member
  // to represent pointer/array duality in C.
  //
  // In many contexts in which a pointer is expected, we examine this
  // member instead of "kind" member to determine whether a type is a
  // pointer or not. That means in many contexts "array of T" is
  // naturally handled as if it were "pointer to T", as required by
  // the C spec.
  Type *base;

  // Declaration
  Token *name;
  Token *name_pos;

  union {
    // Array
    int array_len;

    // Variable-length array
    struct {
      Node *vla_len; // # of elements
      Obj *vla_size; // sizeof() value
    };

    // Struct
    Member *members;

    // Function type
    struct {
      Type *return_ty;
      Type **params;  // NULL-terminated; NULL if there are none
    };
  };
};

// Struct member
struct Member {
  Member *next;
  Type *ty;
  Token *tok; // for error message
  Token *name;
  int idx;
  int align;
  int offset;

  // Bitfield
  bool is_bitfield;
  int bit_offset;
  int bit_width;
};

extern Type *ty_void;
extern Type *ty_bool;

extern Type *ty_char;
extern Type *ty_short;
extern Type *ty_int;
extern Type *ty_long;

extern Type *ty_uchar;
extern Type *ty_ushort;
extern Type *ty_uint;
extern Type *ty_ulong;
extern Type *ty_size_t;     // unsigned int
extern Type *ty_ptrdiff_t;  // int

extern Type *ty_float;
extern Type *ty_double;
extern Type *ty_ldouble;

bool is_integer(Type *ty);
bool is_flonum(Type *ty);
bool is_numeric(Type *ty);
bool is_compatible(Type *t1, Type *t2);
Type *copy_type(Type *ty);
Type *keep_type(Type *ty);
Type *pointer_to(Type *base);
void type_reset(void);
Type *func_type(Type *return_ty);
Type *array_of(Type *base, int size);
Type *vla_of(Type *base, Node *expr);
Type *enum_type(void);
Type *struct_type(void);
void add_type(Node *node);

//
// codegen_rv32.c
//

void codegen_begin(CcObject *out);
void codegen_function(Obj *fn);
void codegen_string(Obj *var, char *data);
void codegen_end(Obj *prog);
int align_to(int n, int align);

//
// unicode.c
//

int encode_utf8(char *buf, uint32_t c);
uint32_t decode_utf8(char **new_pos, char *p);
bool is_ident1(uint32_t c);
bool is_ident2(uint32_t c);
int display_width(char *p, int len);

//
// hashmap.c
//

typedef struct {
  char *key;
  int keylen;
  void *val;
} HashEntry;

typedef struct {
  HashEntry *buckets;
  int capacity;
  int used;
  bool in_fn;  // buckets in the function arena (a block scope's)
} HashMap;

void *hashmap_get(HashMap *map, char *key);
void *hashmap_get2(HashMap *map, char *key, int keylen);
void hashmap_put(HashMap *map, char *key, void *val);
void hashmap_put2(HashMap *map, char *key, int keylen, void *val);
void hashmap_delete(HashMap *map, char *key);
void hashmap_delete2(HashMap *map, char *key, int keylen);
void hashmap_test(void);

//
// driver.c
//

bool file_exists(char *path);
char *cc_dirname(char *path);
char *cc_normpath(char *path);

extern StringArray include_paths;
extern bool opt_fcommon;
extern char *base_file;
