/* wcc -- a C compiler for WOS.
 *
 * It reads C, writes ELF64 objects, and links them into the executables this
 * machine runs.  There is no assembler in between and no assembly text: the
 * code generator writes machine code into a buffer, and the linker resolves
 * the symbols and applies the relocations itself.  That is the same choice TCC
 * makes, for the same reason -- an assembler and a linker are two more programs
 * that would have to be ported before the first one was useful.
 *
 * The pieces, in the order a file goes through them:
 *
 *   lex.c    text into tokens
 *   pp.c     #include, #define, #if -- a token stream in, a token stream out
 *   type.c   what a type is, and what may be done with it
 *   parse.c  tokens into a tree, with every declaration resolved
 *   gen.c    the tree into x86-64 machine code
 *   elf.c    the machine code into an ELF64 relocatable object
 *   link.c   objects and archives into an executable
 *
 * It is written against the hosted C library -- <stdio.h> and its neighbours,
 * nothing from <wkernel.h> -- so it compiles for this machine and for the host
 * that builds this machine.  The second is what makes it testable: a compiler
 * that can only be run by booting an operating system is a compiler nobody
 * will run often enough.
 */
#ifndef WCC_H
#define WCC_H

#include <stddef.h>

/* ------------------------------------------------------------------ *
 *  Somewhere to put things
 *
 *  Nothing is ever freed.  A compiler runs once over one program and then
 *  exits, so the memory it uses is bounded by the program it read -- and an
 *  arena that never frees is a whole class of lifetime bug that cannot
 *  happen.  What it costs is measured in docs/self-hosting.md: this machine
 *  has 256 MB and the largest source file in the tree is 43 KiB.
 * ------------------------------------------------------------------ */

void *wcc_alloc(size_t bytes);          /* zeroed, and never freed   */
char *wcc_strdup(const char *s);
char *wcc_strndup(const char *s, size_t n);

/* Report and stop.  Everything a compiler has to say about a program is a
 * position and a sentence, so there is one function for it. */
void error_at(const char *file, int line, const char *fmt, ...)
    __attribute__((noreturn, format(printf, 3, 4)));
void fatal(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));

/* A growable byte buffer: the code being generated, the data being laid out,
 * a string table being built. */
typedef struct {
    unsigned char *data;
    int            len;
    int            capacity;
} buffer_t;

void  buf_init(buffer_t *b);
void  buf_put(buffer_t *b, const void *bytes, int len);
void  buf_byte(buffer_t *b, int byte);
void  buf_u16(buffer_t *b, unsigned value);
void  buf_u32(buffer_t *b, unsigned value);
void  buf_u64(buffer_t *b, unsigned long value);
void  buf_zero(buffer_t *b, int count);
void  buf_align(buffer_t *b, int alignment);
int   buf_string(buffer_t *b, const char *s);   /* offset of the copy */

/* ------------------------------------------------------------------ *
 *  Tokens
 * ------------------------------------------------------------------ */

typedef enum {
    TK_EOF = 0,
    TK_IDENT,      /* a name, or a keyword -- parse.c tells them apart */
    TK_NUMBER,     /* an integer constant                             */
    TK_STRING,     /* a string literal, already unescaped             */
    TK_CHAR,       /* a character constant, as its value              */
    TK_PUNCT,      /* an operator or a bracket                        */
    TK_NEWLINE,    /* only the preprocessor sees these                */
} token_kind_t;

typedef struct token {
    token_kind_t   kind;
    struct token  *next;

    const char    *text;      /* the spelling, always NUL-terminated  */
    int            len;

    long           value;     /* TK_NUMBER, TK_CHAR                   */
    int            is_unsigned;
    int            is_long;

    char          *string;    /* TK_STRING: the bytes, unescaped      */
    int            string_len;/* including the terminating NUL        */

    const char    *file;      /* where it came from, for messages     */
    int            line;

    /* Preprocessor bookkeeping: a token at the start of a line may begin a
     * directive, and a macro parameter has to know whether a space stood
     * before it when it is stringified. */
    int            at_line_start;
    int            has_space;

    /* A macro does not expand inside its own expansion; this marks the
     * tokens that came out of one so it cannot. */
    struct macro_set *hidden;
} token_t;

/* Tokenise a whole source file. `text` is NUL-terminated and is kept, so the
 * token spellings can point into it. */
token_t *lex(const char *file, char *text);

/* True when this token is exactly `punct` / this identifier. */
int  token_is(const token_t *t, const char *text);

/* ------------------------------------------------------------------ *
 *  The preprocessor
 * ------------------------------------------------------------------ */

/* Where to look for <angled> includes, in order.  -I adds to this. */
void pp_add_include_path(const char *path);

/* -D on the command line, as `NAME` or `NAME=value`. */
void pp_define_from_argument(const char *text);

/* Run the preprocessor over a tokenised file and return the result: no
 * directives, no macros, and no TK_NEWLINE tokens. */
token_t *preprocess(token_t *tokens);

/* Read a whole file into memory, NUL-terminated. NULL if it cannot be read. */
char *read_file(const char *path);

/* ------------------------------------------------------------------ *
 *  Types
 * ------------------------------------------------------------------ */

typedef enum {
    TY_VOID, TY_BOOL, TY_CHAR, TY_SHORT, TY_INT, TY_LONG,
    TY_PTR, TY_ARRAY, TY_STRUCT, TY_UNION, TY_ENUM, TY_FUNC,
} type_kind_t;

typedef struct type   type_t;
typedef struct member member_t;
typedef struct node   node_t;
typedef struct obj    obj_t;

struct type {
    type_kind_t kind;
    int         size;          /* what sizeof gives                    */
    int         align;
    int         is_unsigned;

    type_t     *base;          /* pointer target, array element        */
    int         array_len;     /* -1 for an array of unknown length    */

    const char *name;          /* struct/union/enum tag, or a function */
    member_t   *members;

    type_t     *return_type;   /* TY_FUNC                              */
    obj_t      *params;
    int         is_variadic;
    int         is_defined;    /* a struct that has seen its body      */
};

struct member {
    member_t   *next;
    type_t     *type;
    const char *name;
    int         offset;
};

extern type_t *ty_void, *ty_bool, *ty_char, *ty_short, *ty_int, *ty_long;
extern type_t *ty_uchar, *ty_ushort, *ty_uint, *ty_ulong;

type_t *pointer_to(type_t *base);
type_t *array_of(type_t *base, int len);
type_t *func_type(type_t *return_type);
type_t *new_type(type_kind_t kind, int size, int align);

int is_integer(const type_t *t);
int is_pointer_like(const type_t *t);   /* a pointer or an array */

/* ------------------------------------------------------------------ *
 *  Objects: everything with a name and a place
 * ------------------------------------------------------------------ */

struct obj {
    obj_t      *next;
    const char *name;
    type_t     *type;

    int         is_local;
    int         offset;        /* local: from rbp. member: from the base */

    /* Globals and functions. */
    int         is_function;
    int         is_definition; /* a body, or an initialised variable     */
    int         is_static;
    int         is_extern;

    /* A function's body and what it needs. */
    node_t     *body;
    obj_t      *params;
    obj_t      *locals;
    int         stack_size;

    /* A variable's initial contents, and the relocations in them: an
     * initialiser may name another object's address. */
    unsigned char *init_data;
    int            init_len;
    struct init_reloc *init_relocs;

    /* A variadic function spills its six argument registers into a save area
     * in its own frame, so that va_arg can walk them; this is where. */
    int         va_offset;

    /* Where the code generator put it. */
    int         code_offset;
    int         emitted;
};

/* `&other + addend`, stored at `offset` in an initialiser. */
typedef struct init_reloc {
    struct init_reloc *next;
    int                offset;
    const char        *symbol;
    long               addend;
} init_reloc_t;

/* ------------------------------------------------------------------ *
 *  The tree
 * ------------------------------------------------------------------ */

typedef enum {
    ND_NUM, ND_VAR, ND_MEMBER, ND_DEREF, ND_ADDR,
    ND_ADD, ND_SUB, ND_MUL, ND_DIV, ND_MOD,
    ND_BITAND, ND_BITOR, ND_BITXOR, ND_SHL, ND_SHR,
    ND_EQ, ND_NE, ND_LT, ND_LE,
    ND_AND, ND_OR, ND_NOT, ND_BITNOT, ND_NEG,
    ND_ASSIGN, ND_COND, ND_COMMA, ND_CAST, ND_FUNCALL,
    ND_RETURN, ND_IF, ND_FOR, ND_DO, ND_SWITCH, ND_CASE,
    ND_BLOCK, ND_EXPR_STMT, ND_BREAK, ND_CONTINUE, ND_GOTO, ND_LABEL,
    ND_STMT_EXPR, ND_NULL,
} node_kind_t;

struct node {
    node_kind_t  kind;
    node_t      *next;         /* next statement in a block             */
    type_t      *type;
    const token_t *token;      /* for error messages                    */

    node_t      *lhs;
    node_t      *rhs;

    /* Control flow. `init` and `step` are the for-loop's, `then` and `els`
     * belong to if and to the conditional operator. */
    node_t      *cond;
    node_t      *then;
    node_t      *els;
    node_t      *init;
    node_t      *step;
    node_t      *body;         /* a block's statements                  */

    obj_t       *var;          /* ND_VAR                                */
    long         value;        /* ND_NUM, ND_CASE                       */

    member_t    *member;       /* ND_MEMBER                             */

    /* A call: the function's name, its arguments, and what it returns. */
    const char  *funcname;
    node_t      *args;
    type_t      *func_type;

    const char  *label;        /* ND_GOTO, ND_LABEL                     */
    int          label_id;     /* switch/case/loop labels, numbered     */

    /* A case belongs to two lists at once: the statements of the block it is
     * written in, and the cases of the switch it belongs to.  They need
     * separate links, or threading it onto the second would cut the first. */
    node_t      *cases;        /* ND_SWITCH: the ND_CASE list           */
    node_t      *case_next;    /* ND_CASE: the next case of that switch */
    node_t      *default_case;
    int          break_label;
    int          continue_label;
};

/* Parse a whole translation unit into a list of globals and functions. */
obj_t *parse(token_t *tokens);

/* ------------------------------------------------------------------ *
 *  Code generation and objects
 * ------------------------------------------------------------------ */

/* What a generated object holds: the four sections, the symbols and the
 * relocations against them. */
typedef enum { SEC_TEXT, SEC_RODATA, SEC_DATA, SEC_BSS, SEC_COUNT } section_id_t;

typedef struct reloc {
    struct reloc *next;
    section_id_t  section;     /* where the fixup goes        */
    int           offset;
    const char   *symbol;
    int           type;        /* R_X86_64_*                  */
    long          addend;
} reloc_t;

typedef struct symbol {
    struct symbol *next;
    const char    *name;
    section_id_t   section;
    long           value;      /* offset within the section   */
    long           size;
    int            is_defined;
    int            is_global;
    int            is_function;
    int            elf_index;  /* filled in while writing     */
} symbol_t;

typedef struct {
    buffer_t   section[SEC_COUNT];
    int        bss_size;
    reloc_t   *relocs;
    reloc_t   *last_reloc;
    symbol_t  *symbols;
    symbol_t  *last_symbol;
} unit_t;

/* Generate code for everything `parse` produced. */
void generate(unit_t *unit, obj_t *program);

symbol_t *unit_symbol(unit_t *unit, const char *name);
void      unit_reloc(unit_t *unit, section_id_t section, int offset,
                     const char *symbol, int type, long addend);

/* Write the unit as an ELF64 relocatable object. */
int write_object(const unit_t *unit, const char *path);

/* Link objects and archives into an executable at `path`. */
int link_executable(char **inputs, int input_count, const char *path);

/* The relocation types this compiler produces and its linker understands. */
#define R_X86_64_64    1
#define R_X86_64_PC32  2
#define R_X86_64_PLT32 4
#define R_X86_64_32    10
#define R_X86_64_32S   11

#endif /* WCC_H */
