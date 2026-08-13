/* Tokens into a tree.
 *
 * Recursive descent, one pass, with every name resolved as it is met.  The
 * grammar is C's, minus the parts nothing in this tree uses: no bitfields, no
 * floating point, no old-style parameter lists, no variable-length arrays.
 * `const`, `volatile`, `inline` and `__attribute__` are read and dropped --
 * they change nothing about the code this compiler generates.
 *
 * Two things happen here that a stricter compiler would separate out. Types
 * are attached to nodes as they are built, because the shape of `a + b`
 * depends on whether `a` is a pointer; and constant expressions are folded
 * where the grammar demands one -- an array length, an enum value, a case
 * label, the initialiser of a global.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wcc.h"

/* ------------------------------------------------------------------ *
 *  Scopes
 * ------------------------------------------------------------------ */

typedef struct scope_entry {
    struct scope_entry *next;
    const char         *name;
    obj_t              *var;        /* a variable or function        */
    type_t             *type_def;   /* a typedef                     */
    type_t             *tag;        /* a struct, union or enum tag   */
    int                 is_enum_constant;
    long                enum_value;
} scope_entry_t;

typedef struct scope {
    struct scope  *parent;
    scope_entry_t *entries;
} scope_t;

static scope_t *scope;

static void enter_scope(void)
{
    scope_t *s = wcc_alloc(sizeof(*s));

    s->parent = scope;
    scope = s;
}

static void leave_scope(void)
{
    scope = scope->parent;
}

static scope_entry_t *scope_add(const char *name)
{
    scope_entry_t *e = wcc_alloc(sizeof(*e));

    e->name = name;
    e->next = scope->entries;
    scope->entries = e;
    return e;
}

static scope_entry_t *scope_find(const char *name)
{
    for (scope_t *s = scope; s; s = s->parent)
        for (scope_entry_t *e = s->entries; e; e = e->next)
            if (e->name && strcmp(e->name, name) == 0)
                return e;

    return NULL;
}

/* Tags live in the same list with a different field, since a struct and a
 * variable may share a name without either hiding the other. */
static scope_entry_t *scope_find_tag(const char *name)
{
    for (scope_t *s = scope; s; s = s->parent)
        for (scope_entry_t *e = s->entries; e; e = e->next)
            if (e->tag && e->name && strcmp(e->name, name) == 0)
                return e;

    return NULL;
}

/* ------------------------------------------------------------------ *
 *  The state of one translation unit
 * ------------------------------------------------------------------ */

static obj_t  *globals;
static obj_t  *global_tail;
static obj_t  *current_function;
static obj_t **local_tail;
static int     next_label_id;
static int     next_anonymous;

static token_t *tok;                /* where the parser is */

static void advance(void)
{
    if (tok->kind != TK_EOF)
        tok = tok->next;
}

static void expect(const char *punct)
{
    if (!token_is(tok, punct))
        error_at(tok->file, tok->line, "expected '%s' before '%.*s'",
                 punct, tok->len, tok->text);
    advance();
}

static int consume(const char *punct)
{
    if (token_is(tok, punct)) {
        advance();
        return 1;
    }
    return 0;
}

static char *token_name(const token_t *t)
{
    if (t->kind != TK_IDENT)
        error_at(t->file, t->line, "expected a name");

    return wcc_strndup(t->text, (size_t)t->len);
}

static char *unique_name(const char *stem)
{
    char *name = wcc_alloc(strlen(stem) + 24);

    sprintf(name, "%s.%d", stem, next_anonymous++);
    return name;
}

static obj_t *new_global(const char *name, type_t *type)
{
    obj_t *g = wcc_alloc(sizeof(*g));

    g->name = name;
    g->type = type;

    if (global_tail)
        global_tail = global_tail->next = g;
    else
        globals = global_tail = g;

    return g;
}

static obj_t *new_local(const char *name, type_t *type)
{
    obj_t *v = wcc_alloc(sizeof(*v));

    v->name     = name;
    v->type     = type;
    v->is_local = 1;

    *local_tail = v;
    local_tail  = &v->next;
    return v;
}

/* ------------------------------------------------------------------ *
 *  Making nodes
 * ------------------------------------------------------------------ */

static void add_type(node_t *node);

static node_t *new_node(node_kind_t kind, const token_t *t)
{
    node_t *n = wcc_alloc(sizeof(*n));

    n->kind  = kind;
    n->token = t;
    return n;
}

static node_t *new_binary(node_kind_t kind, node_t *lhs, node_t *rhs,
                          const token_t *t)
{
    node_t *n = new_node(kind, t);

    n->lhs = lhs;
    n->rhs = rhs;
    return n;
}

static node_t *new_unary(node_kind_t kind, node_t *operand, const token_t *t)
{
    node_t *n = new_node(kind, t);

    n->lhs = operand;
    return n;
}

static node_t *new_number(long value, const token_t *t)
{
    node_t *n = new_node(ND_NUM, t);

    n->value = value;
    n->type  = ty_int;
    return n;
}

static node_t *new_long(long value, const token_t *t)
{
    node_t *n = new_node(ND_NUM, t);

    n->value = value;
    n->type  = ty_long;
    return n;
}

static node_t *new_var_node(obj_t *var, const token_t *t)
{
    node_t *n = new_node(ND_VAR, t);

    n->var  = var;
    n->type = var->type;
    return n;
}

static node_t *new_cast(node_t *operand, type_t *type)
{
    node_t *n = new_node(ND_CAST, operand->token);

    add_type(operand);
    n->lhs  = operand;
    n->type = type;
    return n;
}

/* The usual arithmetic conversions, cut down to the integer half of them:
 * anything narrower than int becomes int, and a pair of different widths
 * meets at the wider one. */
static type_t *common_type(type_t *a, type_t *b)
{
    if (a->kind == TY_ARRAY || a->kind == TY_PTR)
        return pointer_to(a->base);

    if (a->size < 4) a = ty_int;
    if (b->size < 4) b = ty_int;

    if (a->size != b->size)
        return a->size > b->size ? a : b;

    if (b->is_unsigned)
        return b;

    return a;
}

static void convert_operands(node_t **lhs, node_t **rhs)
{
    type_t *type = common_type((*lhs)->type, (*rhs)->type);

    *lhs = new_cast(*lhs, type);
    *rhs = new_cast(*rhs, type);
}

/* Work out what a node's type is, from the types of what is under it. */
static void add_type(node_t *node)
{
    if (!node || node->type)
        return;

    add_type(node->lhs);
    add_type(node->rhs);
    add_type(node->cond);
    add_type(node->then);
    add_type(node->els);
    add_type(node->init);
    add_type(node->step);

    for (node_t *n = node->body; n; n = n->next)
        add_type(n);
    for (node_t *n = node->args; n; n = n->next)
        add_type(n);

    switch (node->kind) {
    case ND_ADD: case ND_SUB: case ND_MUL: case ND_DIV: case ND_MOD:
    case ND_BITAND: case ND_BITOR: case ND_BITXOR:
        convert_operands(&node->lhs, &node->rhs);
        node->type = node->lhs->type;
        return;

    case ND_NEG:
        node->type = node->lhs->type->size < 4 ? ty_int : node->lhs->type;
        return;

    case ND_ASSIGN:
        if (node->lhs->type->kind == TY_ARRAY)
            error_at(node->token->file, node->token->line,
                     "an array is not something that can be assigned to");
        if (node->lhs->type->kind != TY_STRUCT &&
            node->lhs->type->kind != TY_UNION)
            node->rhs = new_cast(node->rhs, node->lhs->type);
        node->type = node->lhs->type;
        return;

    case ND_EQ: case ND_NE: case ND_LT: case ND_LE:
        convert_operands(&node->lhs, &node->rhs);
        node->type = ty_int;
        return;

    case ND_FUNCALL:
        node->type = node->func_type ? node->func_type->return_type : ty_long;
        return;

    case ND_NOT: case ND_OR: case ND_AND:
        node->type = ty_int;
        return;

    case ND_BITNOT: case ND_SHL: case ND_SHR:
        node->type = node->lhs->type->size < 4 ? ty_int : node->lhs->type;
        return;

    case ND_VAR:
        node->type = node->var->type;
        return;

    case ND_COND:
        if (node->then->type->kind == TY_VOID ||
            node->els->type->kind == TY_VOID) {
            node->type = ty_void;
        } else {
            convert_operands(&node->then, &node->els);
            node->type = node->then->type;
        }
        return;

    case ND_COMMA:
        node->type = node->rhs->type;
        return;

    case ND_MEMBER:
        node->type = node->member->type;
        return;

    case ND_ADDR:
        /* &array is a pointer to the array's element type, not to the array:
         * the two have the same address and the difference only shows in the
         * arithmetic, where element-sized steps are what anybody means. */
        if (node->lhs->type->kind == TY_ARRAY)
            node->type = pointer_to(node->lhs->type->base);
        else
            node->type = pointer_to(node->lhs->type);
        return;

    case ND_DEREF:
        if (!node->lhs->type->base)
            error_at(node->token->file, node->token->line,
                     "this is not a pointer, so it cannot be dereferenced");
        if (node->lhs->type->base->kind == TY_VOID)
            error_at(node->token->file, node->token->line,
                     "dereferencing a pointer to void");
        node->type = node->lhs->type->base;
        return;

    case ND_STMT_EXPR:
        if (node->body) {
            node_t *last = node->body;
            while (last->next)
                last = last->next;
            if (last->kind == ND_EXPR_STMT) {
                node->type = last->lhs->type;
                return;
            }
        }
        node->type = ty_void;
        return;

    default:
        node->type = ty_int;
        return;
    }
}

/* ------------------------------------------------------------------ *
 *  Constant expressions
 * ------------------------------------------------------------------ */

static node_t *expression(void);
static node_t *assignment(void);
static node_t *conditional(void);

static long fold(node_t *n, init_reloc_t **reloc, int offset_base);

/* The constant part of an address: `&table[3]` is the symbol `table` and an
 * offset of three elements, and both halves are known here rather than at run
 * time.  Sets `*symbol` to the name and returns the offset, or leaves
 * `*symbol` NULL when this is not an address at all. */
static long fold_address(node_t *n, const char **symbol)
{
    *symbol = NULL;

    if (!n)
        return 0;

    switch (n->kind) {
    case ND_VAR:
        if (n->var->is_local)
            return 0;
        *symbol = n->var->name;
        return 0;

    /* &*x is x, which is what makes `&array[i]` reach this at all: the
     * subscript already turned into a dereference. */
    case ND_DEREF:
        return fold_address(n->lhs, symbol);

    case ND_MEMBER: {
        long base = fold_address(n->lhs, symbol);
        return *symbol ? base + n->member->offset : 0;
    }

    case ND_CAST:
        return fold_address(n->lhs, symbol);

    case ND_ADD: {
        long base = fold_address(n->lhs, symbol);

        if (*symbol)
            return base + fold(n->rhs, NULL, 0);

        base = fold_address(n->rhs, symbol);
        return *symbol ? base + fold(n->lhs, NULL, 0) : 0;
    }

    case ND_SUB: {
        long base = fold_address(n->lhs, symbol);
        return *symbol ? base - fold(n->rhs, NULL, 0) : 0;
    }

    default:
        return 0;
    }
}

/* Fold a tree that the grammar says must be constant.  Anything that is not
 * -- a variable, a call -- is an error rather than a zero, because a silent
 * zero in an array length is a bug that shows up much later. */
static long fold(node_t *n, init_reloc_t **reloc, int offset_base)
{
    switch (n->kind) {
    case ND_NUM:    return n->value;
    case ND_ADD:    return fold(n->lhs, reloc, offset_base) +
                           fold(n->rhs, reloc, offset_base);
    case ND_SUB:    return fold(n->lhs, reloc, offset_base) -
                           fold(n->rhs, reloc, offset_base);
    case ND_MUL:    return fold(n->lhs, reloc, offset_base) *
                           fold(n->rhs, reloc, offset_base);
    case ND_DIV: {
        long d = fold(n->rhs, reloc, offset_base);
        return d ? fold(n->lhs, reloc, offset_base) / d : 0;
    }
    case ND_MOD: {
        long d = fold(n->rhs, reloc, offset_base);
        return d ? fold(n->lhs, reloc, offset_base) % d : 0;
    }
    case ND_BITAND: return fold(n->lhs, reloc, offset_base) &
                           fold(n->rhs, reloc, offset_base);
    case ND_BITOR:  return fold(n->lhs, reloc, offset_base) |
                           fold(n->rhs, reloc, offset_base);
    case ND_BITXOR: return fold(n->lhs, reloc, offset_base) ^
                           fold(n->rhs, reloc, offset_base);
    case ND_SHL:    return fold(n->lhs, reloc, offset_base) <<
                           fold(n->rhs, reloc, offset_base);
    case ND_SHR:    return fold(n->lhs, reloc, offset_base) >>
                           fold(n->rhs, reloc, offset_base);
    case ND_EQ:     return fold(n->lhs, reloc, offset_base) ==
                           fold(n->rhs, reloc, offset_base);
    case ND_NE:     return fold(n->lhs, reloc, offset_base) !=
                           fold(n->rhs, reloc, offset_base);
    case ND_LT:     return fold(n->lhs, reloc, offset_base) <
                           fold(n->rhs, reloc, offset_base);
    case ND_LE:     return fold(n->lhs, reloc, offset_base) <=
                           fold(n->rhs, reloc, offset_base);
    case ND_AND:    return fold(n->lhs, reloc, offset_base) &&
                           fold(n->rhs, reloc, offset_base);
    case ND_OR:     return fold(n->lhs, reloc, offset_base) ||
                           fold(n->rhs, reloc, offset_base);
    case ND_NOT:    return !fold(n->lhs, reloc, offset_base);
    case ND_BITNOT: return ~fold(n->lhs, reloc, offset_base);
    case ND_NEG:    return -fold(n->lhs, reloc, offset_base);
    case ND_COMMA:  return fold(n->rhs, reloc, offset_base);
    case ND_COND:   return fold(n->cond, reloc, offset_base)
                         ? fold(n->then, reloc, offset_base)
                         : fold(n->els, reloc, offset_base);
    case ND_CAST: {
        long value = fold(n->lhs, reloc, offset_base);

        if (is_integer(n->type)) {
            switch (n->type->size) {
            case 1: return n->type->is_unsigned ? (long)(unsigned char)value
                                                : (long)(char)value;
            case 2: return n->type->is_unsigned ? (long)(unsigned short)value
                                                : (long)(short)value;
            case 4: return n->type->is_unsigned ? (long)(unsigned int)value
                                                : (long)(int)value;
            }
        }
        return value;
    }

    /* `&something` and a plain array or function name in an initialiser: not
     * a number at all, but an address the linker will fill in. */
    case ND_ADDR:
        if (reloc) {
            const char *symbol = NULL;
            long        addend = fold_address(n->lhs, &symbol);

            if (symbol) {
                init_reloc_t *r = wcc_alloc(sizeof(*r));
                r->offset = offset_base;
                r->symbol = symbol;
                *reloc = r;
                return addend;
            }
        }
        break;

    case ND_VAR:
        if (reloc && !n->var->is_local &&
            (n->var->type->kind == TY_ARRAY || n->var->type->kind == TY_FUNC)) {
            init_reloc_t *r = wcc_alloc(sizeof(*r));
            r->offset = offset_base;
            r->symbol = n->var->name;
            *reloc = r;
            return 0;
        }
        break;

    default:
        break;
    }

    error_at(n->token->file, n->token->line,
             "this has to be a constant, and it is not");
    return 0;
}

/* Can this be worked out at compile time?  Used where the grammar allows only
 * a constant and a better message than "this is not one" is worth having. */
static int is_constant(node_t *n)
{
    if (!n)
        return 0;

    switch (n->kind) {
    case ND_NUM:
        return 1;
    case ND_NEG: case ND_NOT: case ND_BITNOT: case ND_CAST:
        return is_constant(n->lhs);
    case ND_ADD: case ND_SUB: case ND_MUL: case ND_DIV: case ND_MOD:
    case ND_BITAND: case ND_BITOR: case ND_BITXOR: case ND_SHL: case ND_SHR:
    case ND_EQ: case ND_NE: case ND_LT: case ND_LE: case ND_AND: case ND_OR:
        return is_constant(n->lhs) && is_constant(n->rhs);
    case ND_COND:
        return is_constant(n->cond) && is_constant(n->then) &&
               is_constant(n->els);
    default:
        return 0;
    }
}

static long constant_expression(void)
{
    node_t *n = conditional();

    add_type(n);
    return fold(n, NULL, 0);
}

/* ------------------------------------------------------------------ *
 *  Types in declarations
 * ------------------------------------------------------------------ */

typedef struct {
    int is_typedef;
    int is_static;
    int is_extern;
} storage_t;

static type_t *declspec(storage_t *storage);
static type_t *declarator(type_t *type, char **name);
static type_t *abstract_declarator(type_t *type);
static node_t *compound_statement(void);
static node_t *statement(void);
static void    global_declaration(type_t *base, storage_t *storage);

/* __attribute__((...)) and friends: read the balanced parentheses and drop
 * them.  Nothing this compiler does is changed by any of them, and refusing
 * to parse them would mean refusing to read the headers that use them. */
static void skip_attribute(void)
{
    for (;;) {
        if (token_is(tok, "__attribute__") || token_is(tok, "__attribute") ||
            token_is(tok, "__extension__") || token_is(tok, "__inline__") ||
            token_is(tok, "__inline") || token_is(tok, "inline") ||
            token_is(tok, "__restrict") || token_is(tok, "__restrict__") ||
            token_is(tok, "restrict") || token_is(tok, "volatile") ||
            token_is(tok, "__volatile__") || token_is(tok, "_Noreturn") ||
            token_is(tok, "register") || token_is(tok, "auto")) {
            int has_parens = token_is(tok, "__attribute__") ||
                             token_is(tok, "__attribute");
            advance();

            if (has_parens && token_is(tok, "(")) {
                int depth = 0;
                do {
                    if (token_is(tok, "("))      depth++;
                    else if (token_is(tok, ")")) depth--;
                    advance();
                } while (depth && tok->kind != TK_EOF);
            }
            continue;
        }
        return;
    }
}

static int is_type_name(const token_t *t)
{
    static const char *const keywords[] = {
        "void", "char", "short", "int", "long", "signed", "unsigned",
        "struct", "union", "enum", "_Bool", "const", "volatile", "static",
        "extern", "typedef", "inline", "__inline", "__inline__", "register",
        "auto", "restrict", "__restrict", "__restrict__", "__attribute__",
        "__attribute", "__extension__", "_Noreturn", NULL,
    };

    if (t->kind != TK_IDENT)
        return 0;

    for (int i = 0; keywords[i]; i++)
        if (token_is(t, keywords[i]))
            return 1;

    char          *name = wcc_strndup(t->text, (size_t)t->len);
    scope_entry_t *e    = scope_find(name);

    return e && e->type_def != NULL;
}

/* struct-or-union-specifier, after the keyword. */
static type_t *struct_or_union(int is_union)
{
    char *tag = NULL;

    skip_attribute();

    if (tok->kind == TK_IDENT && !token_is(tok, "{")) {
        tag = token_name(tok);
        advance();
    }

    /* A named type with no body is a reference to one declared elsewhere, and
     * may be a forward declaration of one not yet defined. */
    if (tag && !token_is(tok, "{")) {
        scope_entry_t *e = scope_find_tag(tag);

        if (e)
            return e->tag;

        type_t *t = new_type(is_union ? TY_UNION : TY_STRUCT, 0, 1);
        t->name = tag;
        scope_add(tag)->tag = t;
        return t;
    }

    type_t *type;
    scope_entry_t *existing = tag ? scope_find_tag(tag) : NULL;

    /* A tag declared earlier in this scope is completed rather than shadowed,
     * so that `struct list *next;` inside `struct list` points at this one. */
    if (existing && !existing->tag->is_defined) {
        type = existing->tag;
    } else {
        type = new_type(is_union ? TY_UNION : TY_STRUCT, 0, 1);
        type->name = tag;
        if (tag)
            scope_add(tag)->tag = type;
    }

    expect("{");

    member_t  head;
    member_t *tail = &head;
    int       offset = 0;
    int       align  = 1;

    head.next = NULL;

    while (!consume("}")) {
        storage_t storage;
        memset(&storage, 0, sizeof(storage));

        type_t *base = declspec(&storage);
        int     first = 1;

        while (!consume(";")) {
            if (!first)
                expect(",");
            first = 0;

            char   *name = NULL;
            type_t *member_type = declarator(base, &name);

            skip_attribute();

            member_t *m = wcc_alloc(sizeof(*m));
            m->type = member_type;
            m->name = name;

            if (member_type->align > align)
                align = member_type->align;

            if (is_union) {
                m->offset = 0;
                if (member_type->size > offset)
                    offset = member_type->size;
            } else {
                offset = (offset + member_type->align - 1) /
                         member_type->align * member_type->align;
                m->offset = offset;
                offset += member_type->size;
            }

            tail = tail->next = m;
        }
    }

    type->members    = head.next;
    type->align      = align;
    type->size       = (offset + align - 1) / align * align;
    type->is_defined = 1;

    skip_attribute();
    return type;
}

static type_t *enum_specifier(void)
{
    char *tag = NULL;

    if (tok->kind == TK_IDENT && !token_is(tok, "{")) {
        tag = token_name(tok);
        advance();
    }

    if (tag && !token_is(tok, "{")) {
        scope_entry_t *e = scope_find_tag(tag);
        if (e)
            return e->tag;
        error_at(tok->file, tok->line, "no enum called %s", tag);
    }

    /* An enum is an int with names attached; the names go into the scope as
     * constants and the type itself is int-shaped. */
    type_t *type = new_type(TY_ENUM, 4, 4);
    type->name = tag;

    if (tag)
        scope_add(tag)->tag = type;

    expect("{");

    long value = 0;
    while (!consume("}")) {
        char *name = token_name(tok);
        advance();

        if (consume("="))
            value = constant_expression();

        scope_entry_t *e = scope_add(name);
        e->is_enum_constant = 1;
        e->enum_value       = value++;

        if (!consume(","))
            { expect("}"); break; }
    }

    return type;
}

/* The type at the head of a declaration: `unsigned long`, `struct foo`, a
 * typedef name, and the storage class words mixed in among them. */
static type_t *declspec(storage_t *storage)
{
    /* Counted rather than flagged, so `long long` and `unsigned int` fall out
     * of the same loop that reads `int unsigned long`. */
    int voids = 0, bools = 0, chars = 0, shorts = 0, ints = 0, longs = 0;
    int is_unsigned = 0, is_signed = 0;
    type_t *named = NULL;

    for (;;) {
        skip_attribute();

        if (token_is(tok, "typedef")) {
            if (storage) storage->is_typedef = 1;
            advance();
            continue;
        }
        if (token_is(tok, "static")) {
            if (storage) storage->is_static = 1;
            advance();
            continue;
        }
        if (token_is(tok, "extern")) {
            if (storage) storage->is_extern = 1;
            advance();
            continue;
        }
        if (token_is(tok, "const")) {
            advance();
            continue;
        }

        if (token_is(tok, "struct") || token_is(tok, "union")) {
            int is_union = token_is(tok, "union");
            advance();
            named = struct_or_union(is_union);
            continue;
        }
        if (token_is(tok, "enum")) {
            advance();
            named = enum_specifier();
            continue;
        }

        if (token_is(tok, "void"))     { voids++;  advance(); continue; }
        if (token_is(tok, "_Bool"))    { bools++;  advance(); continue; }
        if (token_is(tok, "char"))     { chars++;  advance(); continue; }
        if (token_is(tok, "short"))    { shorts++; advance(); continue; }
        if (token_is(tok, "int"))      { ints++;   advance(); continue; }
        if (token_is(tok, "long"))     { longs++;  advance(); continue; }
        if (token_is(tok, "unsigned")) { is_unsigned = 1; advance(); continue; }
        if (token_is(tok, "signed"))   { is_signed = 1;   advance(); continue; }

        /* A typedef name ends the type, but only if nothing has named a type
         * yet: in `long x` the `x` is the declarator, not a type. */
        if (tok->kind == TK_IDENT && !named && !voids && !bools && !chars &&
            !shorts && !ints && !longs && !is_unsigned && !is_signed) {
            char          *name = wcc_strndup(tok->text, (size_t)tok->len);
            scope_entry_t *e    = scope_find(name);

            if (e && e->type_def) {
                named = e->type_def;
                advance();
                continue;
            }
        }

        break;
    }

    if (named)
        return named;

    if (voids)  return ty_void;
    if (bools)  return ty_bool;
    if (chars)  return is_unsigned ? ty_uchar : ty_char;
    if (shorts) return is_unsigned ? ty_ushort : ty_short;
    if (longs)  return is_unsigned ? ty_ulong : ty_long;
    if (ints || is_unsigned || is_signed)
        return is_unsigned ? ty_uint : ty_int;

    /* No type words at all: C89 said that meant int, and headers still rely
     * on it for things like `static inline foo()`. */
    return ty_int;
}

static type_t *function_parameters(type_t *return_type)
{
    type_t *type = func_type(return_type);
    obj_t   head;
    obj_t  *tail = &head;

    head.next = NULL;

    if (token_is(tok, "void") && token_is(tok->next, ")")) {
        advance();
        advance();
        return type;
    }

    while (!consume(")")) {
        if (tail != &head)
            expect(",");

        if (consume("...")) {
            type->is_variadic = 1;
            expect(")");
            break;
        }

        storage_t storage;
        memset(&storage, 0, sizeof(storage));

        type_t *base  = declspec(&storage);
        char   *name  = NULL;
        type_t *param = declarator(base, &name);

        /* An array parameter is a pointer, and a function parameter is a
         * pointer to a function: what is passed is an address either way. */
        if (param->kind == TY_ARRAY)
            param = pointer_to(param->base);
        else if (param->kind == TY_FUNC)
            param = pointer_to(param);

        obj_t *p = wcc_alloc(sizeof(*p));
        p->name = name ? name : "";
        p->type = param;
        tail = tail->next = p;
    }

    type->params = head.next;
    return type;
}

/* The part after the type: pointers, then a name, then the array and function
 * suffixes -- which bind tighter than the pointers, hence the recursion. */
static type_t *declarator_suffix(type_t *type)
{
    if (token_is(tok, "(")) {
        advance();
        return function_parameters(type);
    }

    if (token_is(tok, "[")) {
        advance();

        int len = -1;
        if (!token_is(tok, "]")) {
            token_t *size_at = tok;
            node_t  *size    = conditional();

            add_type(size);

            /* A length that is not constant is a variable-length array: the
             * frame would have to grow while the function runs, and every
             * offset in it would stop being known at compile time. */
            if (size->kind != ND_NUM && !is_constant(size))
                error_at(size_at->file, size_at->line,
                         "wcc has no variable-length arrays; this length has "
                         "to be a constant");

            len = (int)fold(size, NULL, 0);
        }

        expect("]");
        type = declarator_suffix(type);
        return array_of(type, len);
    }

    return type;
}

static type_t *declarator(type_t *type, char **name)
{
    while (consume("*")) {
        type = pointer_to(type);
        while (token_is(tok, "const") || token_is(tok, "volatile") ||
               token_is(tok, "restrict") || token_is(tok, "__restrict") ||
               token_is(tok, "__restrict__"))
            advance();
    }

    /* A parenthesised declarator -- `char (*f)(int)` -- is read by parsing
     * the inside against a placeholder and then filling the placeholder in
     * with whatever the suffix turned out to be. */
    if (token_is(tok, "(")) {
        token_t *inner = tok->next;
        type_t   placeholder;

        memset(&placeholder, 0, sizeof(placeholder));

        /* Skip the parenthesised part, read the suffix that follows it, then
         * come back and read the inside against that. */
        int depth = 0;
        do {
            if (token_is(tok, "("))      depth++;
            else if (token_is(tok, ")")) depth--;
            advance();
        } while (depth && tok->kind != TK_EOF);

        type_t *outer = declarator_suffix(type);
        token_t *after = tok;

        tok = inner;
        *(type_t **)&placeholder.base = NULL;
        type_t *result = declarator(outer, name);
        tok = after;
        return result;
    }

    if (tok->kind == TK_IDENT) {
        *name = token_name(tok);
        advance();
    }

    return declarator_suffix(type);
}

/* The same, for a type with no name in it: a cast, or sizeof(type). */
static type_t *abstract_declarator(type_t *type)
{
    while (consume("*"))
        type = pointer_to(type);

    if (token_is(tok, "(") && (token_is(tok->next, "*") ||
                               token_is(tok->next, "("))) {
        char *ignored = NULL;
        return declarator(type, &ignored);
    }

    return declarator_suffix(type);
}

/* ------------------------------------------------------------------ *
 *  Expressions
 * ------------------------------------------------------------------ */

static node_t *unary(void);
static node_t *cast_expression(void);

/* Addition and subtraction know about pointers: `p + n` steps by the size of
 * what p points at.  Declared here because `a[i]` is `*(a + i)` and is parsed
 * before they are defined. */
static node_t *make_add(node_t *lhs, node_t *rhs, const token_t *t);
static node_t *make_sub(node_t *lhs, node_t *rhs, const token_t *t);

/* A string literal becomes an anonymous array in .rodata. */
static obj_t *string_literal(const token_t *t)
{
    obj_t *g = new_global(unique_name(".Lstr"),
                          array_of(ty_char, t->string_len));

    g->is_definition = 1;
    g->is_static     = 1;
    g->init_data     = (unsigned char *)t->string;
    g->init_len      = t->string_len;
    return g;
}

/* A call, by name or through a pointer.  Exactly one of `name` and `target`
 * is given: a name becomes a direct call the linker resolves, and anything
 * else is an address to call through. */
static node_t *function_call(char *name, node_t *target, const token_t *site)
{
    node_t *n = new_node(ND_FUNCALL, site);

    n->funcname = name;
    n->lhs      = target;

    if (target) {
        add_type(target);

        type_t *type = target->type;

        /* `f(x)` where f is a pointer to a function, and `(*f)(x)`, which the
         * unary operator already turned into the same thing. */
        if (type->kind == TY_PTR)
            type = type->base;
        if (type->kind == TY_FUNC)
            n->func_type = type;
    } else {
        scope_entry_t *e = scope_find(name);
        if (e && e->var && e->var->type->kind == TY_FUNC)
            n->func_type = e->var->type;
    }

    node_t  head;
    node_t *tail = &head;
    head.next = NULL;

    /* Which parameter each argument lines up with, so it can be converted to
     * the declared type -- an int passed where a long is expected has to be
     * widened by the caller. */
    obj_t *param = n->func_type ? n->func_type->params : NULL;

    while (!consume(")")) {
        if (tail != &head)
            expect(",");

        node_t *arg = assignment();
        add_type(arg);

        if (param) {
            if (param->type->kind != TY_STRUCT && param->type->kind != TY_UNION)
                arg = new_cast(arg, param->type);
            param = param->next;
        } else if (arg->type->kind == TY_ARRAY) {
            arg = new_cast(arg, pointer_to(arg->type->base));
        } else if (arg->type->size < 4) {
            arg = new_cast(arg, ty_int);
        }

        tail = tail->next = arg;
    }

    n->args = head.next;

    if (!n->func_type) {
        /* Not declared: assume it returns int, and say so once.  A C compiler
         * used to have to accept this, and headers on this machine are
         * complete enough that it is almost always a spelling mistake. */
        n->type = ty_int;
    }

    return n;
}

static node_t *primary(void)
{
    const token_t *t = tok;

    if (consume("(")) {
        /* A statement expression: ({ ... }), which headers here do not use
         * but which costs three lines to support. */
        if (token_is(tok, "{")) {
            node_t *n = new_node(ND_STMT_EXPR, t);
            advance();
            enter_scope();
            node_t  head;
            node_t *tail = &head;
            head.next = NULL;
            while (!consume("}"))
                tail = tail->next = statement();
            leave_scope();
            n->body = head.next;
            expect(")");
            return n;
        }

        /* A cast, or a parenthesised expression. */
        if (is_type_name(tok)) {
            storage_t storage;
            memset(&storage, 0, sizeof(storage));

            type_t *type = abstract_declarator(declspec(&storage));
            expect(")");

            if (token_is(tok, "{"))
                error_at(tok->file, tok->line,
                         "wcc has no compound literals; give this a name and "
                         "initialise it there");

            return new_cast(cast_expression(), type);
        }

        node_t *n = expression();
        expect(")");
        return n;
    }

    if (token_is(tok, "sizeof")) {
        advance();

        if (token_is(tok, "(") && is_type_name(tok->next)) {
            advance();
            storage_t storage;
            memset(&storage, 0, sizeof(storage));
            type_t *type = abstract_declarator(declspec(&storage));
            expect(")");
            return new_long(type->size, t);
        }

        node_t *operand = unary();
        add_type(operand);
        return new_long(operand->type->size, t);
    }

    if (tok->kind == TK_NUMBER) {
        node_t *n = tok->is_long ? new_long(tok->value, tok)
                                 : new_number(tok->value, tok);
        if (tok->is_unsigned)
            n->type = tok->is_long ? ty_ulong : ty_uint;
        advance();
        return n;
    }

    if (tok->kind == TK_CHAR) {
        node_t *n = new_number(tok->value, tok);
        advance();
        return n;
    }

    if (tok->kind == TK_STRING) {
        obj_t *g = string_literal(tok);
        advance();

        /* Adjacent string literals are one string. */
        while (tok->kind == TK_STRING) {
            buffer_t joined;
            buf_init(&joined);
            buf_put(&joined, g->init_data, g->init_len - 1);
            buf_put(&joined, tok->string, tok->string_len);

            g->init_data = joined.data;
            g->init_len  = joined.len;
            g->type      = array_of(ty_char, joined.len);
            advance();
        }

        return new_var_node(g, t);
    }

    if (tok->kind == TK_IDENT) {
        char *name = token_name(tok);

        /* The three things this compiler does not have, each said by name
         * rather than as a parse error somewhere further on. */
        if (strcmp(name, "__asm__") == 0 || strcmp(name, "asm") == 0 ||
            strcmp(name, "__asm") == 0)
            error_at(t->file, t->line,
                     "wcc has no inline assembly; a program that needs it has "
                     "to be built on the host");

        advance();

        if (consume("("))
            return function_call(name, NULL, t);

        scope_entry_t *e = scope_find(name);

        if (e && e->is_enum_constant)
            return new_number(e->enum_value, t);

        if (!e || !e->var)
            error_at(t->file, t->line, "'%s' is not declared here", name);

        return new_var_node(e->var, t);
    }

    error_at(tok->file, tok->line, "expected an expression before '%.*s'",
             tok->len, tok->text);
    return NULL;
}

static member_t *find_member(type_t *type, const char *name,
                             const token_t *site)
{
    if (type->kind != TY_STRUCT && type->kind != TY_UNION)
        error_at(site->file, site->line,
                 "this is not a structure, so it has no member '%s'", name);

    for (member_t *m = type->members; m; m = m->next)
        if (strcmp(m->name, name) == 0)
            return m;

    error_at(site->file, site->line, "no member called '%s'", name);
    return NULL;
}

static node_t *postfix(void)
{
    node_t *n = primary();

    for (;;) {
        const token_t *t = tok;

        if (consume("(")) {
            /* Calling what an expression produced rather than a name:
             * `table[i].run(x)`, `(*f)(x)`, `handler(x)` where handler is a
             * variable.  The address is computed and called through. */
            n = function_call(NULL, n, t);
            continue;
        }

        if (consume("[")) {
            /* a[i] is *(a + i), with the addition doing the scaling. */
            node_t *index = expression();
            expect("]");

            add_type(n);
            add_type(index);

            node_t *sum = make_add(n, index, t);
            add_type(sum);
            n = new_unary(ND_DEREF, sum, t);
            add_type(n);
            continue;
        }

        if (consume(".")) {
            add_type(n);
            char *name = token_name(tok);
            advance();

            node_t *m = new_unary(ND_MEMBER, n, t);
            m->member = find_member(n->type, name, t);
            add_type(m);
            n = m;
            continue;
        }

        if (consume("->")) {
            add_type(n);
            char *name = token_name(tok);
            advance();

            node_t *deref = new_unary(ND_DEREF, n, t);
            add_type(deref);

            node_t *m = new_unary(ND_MEMBER, deref, t);
            m->member = find_member(deref->type, name, t);
            add_type(m);
            n = m;
            continue;
        }

        if (consume("++") || (t == tok && 0)) {
            /* x++ is the value of x, with x increased afterwards.  Written as
             * `(x += 1) - 1` on the type of x, which is right for pointers
             * too because the arithmetic scales. */
            add_type(n);
            node_t *inc = new_node(ND_ASSIGN, t);
            inc->lhs = n;
            inc->rhs = new_binary(ND_ADD, n, new_number(1, t), t);
            add_type(inc);

            node_t *back = new_binary(ND_SUB, inc, new_number(1, t), t);
            add_type(back);
            n = new_cast(back, n->type);
            continue;
        }

        if (token_is(tok, "--")) {
            advance();
            add_type(n);
            node_t *dec = new_node(ND_ASSIGN, t);
            dec->lhs = n;
            dec->rhs = new_binary(ND_SUB, n, new_number(1, t), t);
            add_type(dec);

            node_t *back = new_binary(ND_ADD, dec, new_number(1, t), t);
            add_type(back);
            n = new_cast(back, n->type);
            continue;
        }

        return n;
    }
}

static node_t *unary(void)
{
    const token_t *t = tok;

    if (consume("+"))
        return cast_expression();

    if (consume("-"))
        return new_unary(ND_NEG, cast_expression(), t);

    if (consume("!"))
        return new_unary(ND_NOT, cast_expression(), t);

    if (consume("~"))
        return new_unary(ND_BITNOT, cast_expression(), t);

    if (consume("&")) {
        node_t *operand = cast_expression();
        add_type(operand);
        return new_unary(ND_ADDR, operand, t);
    }

    if (consume("*")) {
        node_t *operand = cast_expression();
        add_type(operand);

        /* Dereferencing a function pointer gives the function back, which is
         * how `(*f)(x)` and `f(x)` come to mean the same thing. */
        if (operand->type->kind == TY_PTR &&
            operand->type->base->kind == TY_FUNC)
            return operand;

        return new_unary(ND_DEREF, operand, t);
    }

    if (consume("++")) {
        /* ++x is x += 1. */
        node_t *operand = unary();
        add_type(operand);

        node_t *n = new_node(ND_ASSIGN, t);
        n->lhs = operand;
        n->rhs = new_binary(ND_ADD, operand, new_number(1, t), t);
        return n;
    }

    if (consume("--")) {
        node_t *operand = unary();
        add_type(operand);

        node_t *n = new_node(ND_ASSIGN, t);
        n->lhs = operand;
        n->rhs = new_binary(ND_SUB, operand, new_number(1, t), t);
        return n;
    }

    return postfix();
}

static node_t *cast_expression(void)
{
    if (token_is(tok, "(") && is_type_name(tok->next)) {
        const token_t *t = tok;

        advance();
        storage_t storage;
        memset(&storage, 0, sizeof(storage));

        type_t *type = abstract_declarator(declspec(&storage));
        expect(")");

        if (token_is(tok, "{"))
            error_at(tok->file, tok->line,
                     "wcc has no compound literals; give this a name and "
                     "initialise it there");

        (void)t;
        return new_cast(cast_expression(), type);
    }

    return unary();
}

/* Pointer arithmetic: `p + n` steps by the size of what p points at, and
 * `p - q` counts elements rather than bytes. */
static node_t *make_add(node_t *lhs, node_t *rhs, const token_t *t)
{
    add_type(lhs);
    add_type(rhs);

    if (is_integer(lhs->type) && is_integer(rhs->type))
        return new_binary(ND_ADD, lhs, rhs, t);

    if (is_pointer_like(lhs->type) && is_pointer_like(rhs->type))
        error_at(t->file, t->line, "two pointers cannot be added");

    /* n + p is p + n. */
    if (is_integer(lhs->type)) {
        node_t *swap = lhs;
        lhs = rhs;
        rhs = swap;
    }

    int step = lhs->type->base->size;
    rhs = new_binary(ND_MUL, new_cast(rhs, ty_long), new_long(step, t), t);
    return new_binary(ND_ADD, lhs, rhs, t);
}

static node_t *make_sub(node_t *lhs, node_t *rhs, const token_t *t)
{
    add_type(lhs);
    add_type(rhs);

    if (is_integer(lhs->type) && is_integer(rhs->type))
        return new_binary(ND_SUB, lhs, rhs, t);

    if (is_pointer_like(lhs->type) && is_integer(rhs->type)) {
        int step = lhs->type->base->size;
        rhs = new_binary(ND_MUL, new_cast(rhs, ty_long), new_long(step, t), t);
        add_type(rhs);
        node_t *n = new_binary(ND_SUB, lhs, rhs, t);
        n->type = lhs->type;
        return n;
    }

    if (is_pointer_like(lhs->type) && is_pointer_like(rhs->type)) {
        int step = lhs->type->base->size;
        node_t *n = new_binary(ND_SUB, lhs, rhs, t);
        n->type = ty_long;
        return new_binary(ND_DIV, n, new_long(step, t), t);
    }

    error_at(t->file, t->line, "these two cannot be subtracted");
    return NULL;
}

static node_t *mul_expression(void)
{
    node_t *n = cast_expression();

    for (;;) {
        const token_t *t = tok;

        if (consume("*"))      n = new_binary(ND_MUL, n, cast_expression(), t);
        else if (consume("/")) n = new_binary(ND_DIV, n, cast_expression(), t);
        else if (consume("%")) n = new_binary(ND_MOD, n, cast_expression(), t);
        else return n;
    }
}

static node_t *add_expression(void)
{
    node_t *n = mul_expression();

    for (;;) {
        const token_t *t = tok;

        if (consume("+"))      n = make_add(n, mul_expression(), t);
        else if (consume("-")) n = make_sub(n, mul_expression(), t);
        else return n;
    }
}

static node_t *shift_expression(void)
{
    node_t *n = add_expression();

    for (;;) {
        const token_t *t = tok;

        if (consume("<<"))      n = new_binary(ND_SHL, n, add_expression(), t);
        else if (consume(">>")) n = new_binary(ND_SHR, n, add_expression(), t);
        else return n;
    }
}

static node_t *relational(void)
{
    node_t *n = shift_expression();

    for (;;) {
        const token_t *t = tok;

        if (consume("<"))
            n = new_binary(ND_LT, n, shift_expression(), t);
        else if (consume("<="))
            n = new_binary(ND_LE, n, shift_expression(), t);
        else if (consume(">"))
            n = new_binary(ND_LT, shift_expression(), n, t);
        else if (consume(">="))
            n = new_binary(ND_LE, shift_expression(), n, t);
        else
            return n;
    }
}

static node_t *equality(void)
{
    node_t *n = relational();

    for (;;) {
        const token_t *t = tok;

        if (consume("=="))      n = new_binary(ND_EQ, n, relational(), t);
        else if (consume("!=")) n = new_binary(ND_NE, n, relational(), t);
        else return n;
    }
}

static node_t *bitand_expression(void)
{
    node_t *n = equality();

    while (token_is(tok, "&") && tok->kind == TK_PUNCT) {
        const token_t *t = tok;
        advance();
        n = new_binary(ND_BITAND, n, equality(), t);
    }
    return n;
}

static node_t *bitxor_expression(void)
{
    node_t *n = bitand_expression();

    while (token_is(tok, "^")) {
        const token_t *t = tok;
        advance();
        n = new_binary(ND_BITXOR, n, bitand_expression(), t);
    }
    return n;
}

static node_t *bitor_expression(void)
{
    node_t *n = bitxor_expression();

    while (token_is(tok, "|")) {
        const token_t *t = tok;
        advance();
        n = new_binary(ND_BITOR, n, bitxor_expression(), t);
    }
    return n;
}

static node_t *logical_and(void)
{
    node_t *n = bitor_expression();

    while (token_is(tok, "&&")) {
        const token_t *t = tok;
        advance();
        n = new_binary(ND_AND, n, bitor_expression(), t);
    }
    return n;
}

static node_t *logical_or(void)
{
    node_t *n = logical_and();

    while (token_is(tok, "||")) {
        const token_t *t = tok;
        advance();
        n = new_binary(ND_OR, n, logical_and(), t);
    }
    return n;
}

static node_t *conditional(void)
{
    node_t *cond = logical_or();

    if (!token_is(tok, "?"))
        return cond;

    const token_t *t = tok;
    advance();

    node_t *n = new_node(ND_COND, t);
    n->cond = cond;
    n->then = expression();
    expect(":");
    n->els  = conditional();
    return n;
}

/* `a op= b` becomes `a = a op b`.  The left side is evaluated twice in the
 * tree, which is wrong for `*p++ += 1` and right for everything this compiler
 * is asked to build; saying so beats pretending otherwise. */
static node_t *compound_assign(node_t *lhs, node_kind_t op, const token_t *t)
{
    node_t *rhs = assignment();
    node_t *combined;

    add_type(lhs);
    add_type(rhs);

    if (op == ND_ADD)
        combined = make_add(lhs, rhs, t);
    else if (op == ND_SUB)
        combined = make_sub(lhs, rhs, t);
    else
        combined = new_binary(op, lhs, rhs, t);

    node_t *n = new_node(ND_ASSIGN, t);
    n->lhs = lhs;
    n->rhs = combined;
    return n;
}

static node_t *assignment(void)
{
    node_t        *n = conditional();
    const token_t *t = tok;

    if (consume("="))
        return new_binary(ND_ASSIGN, n, assignment(), t);

    if (consume("+="))  return compound_assign(n, ND_ADD, t);
    if (consume("-="))  return compound_assign(n, ND_SUB, t);
    if (consume("*="))  return compound_assign(n, ND_MUL, t);
    if (consume("/="))  return compound_assign(n, ND_DIV, t);
    if (consume("%="))  return compound_assign(n, ND_MOD, t);
    if (consume("&="))  return compound_assign(n, ND_BITAND, t);
    if (consume("|="))  return compound_assign(n, ND_BITOR, t);
    if (consume("^="))  return compound_assign(n, ND_BITXOR, t);
    if (consume("<<=")) return compound_assign(n, ND_SHL, t);
    if (consume(">>=")) return compound_assign(n, ND_SHR, t);

    return n;
}

static node_t *expression(void)
{
    node_t        *n = assignment();
    const token_t *t = tok;

    while (consume(","))
        n = new_binary(ND_COMMA, n, assignment(), t);

    return n;
}

/* ------------------------------------------------------------------ *
 *  Initialisers
 * ------------------------------------------------------------------ */

/* A local's initialiser becomes assignments; this builds them. */
static node_t *local_initializer(obj_t *var, const token_t *t);

/* Write a constant into a global's initial contents at `offset`. */
static void write_constant(obj_t *g, int offset, type_t *type, node_t *value)
{
    init_reloc_t *reloc = NULL;

    add_type(value);
    long number = fold(value, &reloc, offset);

    if (reloc) {
        reloc->addend = number;
        reloc->next   = g->init_relocs;
        g->init_relocs = reloc;

        /* The address itself is filled in by the linker; eight bytes of room
         * is what a pointer needs. */
        return;
    }

    for (int i = 0; i < type->size && offset + i < g->init_len; i++)
        g->init_data[offset + i] = (unsigned char)((unsigned long)number >>
                                                   (i * 8));
}

static void global_initializer(obj_t *g, type_t *type, int offset);

/* { a, b, c } for an array or a structure, recursively. */
static void brace_initializer(obj_t *g, type_t *type, int offset)
{
    expect("{");

    if (type->kind == TY_ARRAY) {
        int i = 0;

        for (; !token_is(tok, "}"); i++) {
            if (i)
                expect(",");
            if (token_is(tok, "}"))
                break;              /* a trailing comma, which is allowed */
            global_initializer(g, type->base, offset + i * type->base->size);
        }
        expect("}");
        return;
    }

    if (type->kind == TY_STRUCT || type->kind == TY_UNION) {
        member_t *m = type->members;

        for (; m; m = m ? m->next : NULL) {
            if (m != type->members && !consume(","))
                break;
            if (token_is(tok, "}"))
                break;              /* a trailing comma again */

            /* `.name = value` says which member, so the ones before it are
             * left as they are -- which is zero, since the whole initial
             * image starts that way. */
            if (token_is(tok, ".")) {
                advance();
                char *field = token_name(tok);
                advance();
                expect("=");

                m = find_member(type, field, tok);
            }

            global_initializer(g, m->type, offset + m->offset);

            if (type->kind == TY_UNION)
                break;              /* only the first member is initialised */
        }

        /* Fewer initialisers than members is normal and means the rest are
         * zero, which they already are. */
        consume(",");
        expect("}");
        return;
    }

    /* `int x = { 1 };` is legal and means what it looks like. */
    global_initializer(g, type, offset);
    expect("}");
}

static void global_initializer(obj_t *g, type_t *type, int offset)
{
    /* A string initialising a char array is copied in, not pointed at. */
    if (type->kind == TY_ARRAY && type->base->size == 1 &&
        tok->kind == TK_STRING) {
        int len = tok->string_len;

        if (type->array_len >= 0 && len > type->array_len)
            len = type->array_len;

        for (int i = 0; i < len && offset + i < g->init_len; i++)
            g->init_data[offset + i] = (unsigned char)tok->string[i];

        advance();
        return;
    }

    if (token_is(tok, "{")) {
        brace_initializer(g, type, offset);
        return;
    }

    write_constant(g, offset, type, assignment());
}

/* An array whose length was left out takes it from the initialiser. */
static void size_array_from_initializer(type_t *type)
{
    token_t *start = tok;
    int      count = 0;

    if (tok->kind == TK_STRING) {
        type->array_len = tok->string_len;
        type->size      = type->base->size * type->array_len;
        return;
    }

    expect("{");
    int depth = 0;

    for (; tok->kind != TK_EOF; advance()) {
        if (depth == 0 && token_is(tok, "}"))
            break;
        if (token_is(tok, "{"))
            depth++;
        else if (token_is(tok, "}"))
            depth--;
        else if (depth == 0 && token_is(tok, ","))
            count++;
        else if (depth == 0 && count == 0)
            count = 1;
    }

    /* A trailing comma does not add an element. */
    tok = start;
    type->array_len = count;
    type->size      = type->base->size * count;
}

/* ------------------------------------------------------------------ *
 *  Statements
 * ------------------------------------------------------------------ */

static int current_break_label;
static int current_continue_label;
static node_t *current_switch;

static node_t *expression_statement(void)
{
    const token_t *t = tok;

    if (consume(";"))
        return new_node(ND_NULL, t);

    node_t *n = new_unary(ND_EXPR_STMT, expression(), t);
    expect(";");
    add_type(n->lhs);
    return n;
}

/* One declaration inside a function: `int a = 1, *b;` */
static node_t *local_declaration(type_t *base, storage_t *storage)
{
    node_t  head;
    node_t *tail = &head;
    int     first = 1;

    head.next = NULL;

    while (!consume(";")) {
        if (!first)
            expect(",");
        first = 0;

        char   *name = NULL;
        type_t *type = declarator(base, &name);

        skip_attribute();

        if (!name)
            error_at(tok->file, tok->line, "this declaration has no name");

        if (storage->is_typedef) {
            scope_add(name)->type_def = type;
            continue;
        }

        /* A static local is a global with a name nobody else can say. */
        if (storage->is_static) {
            obj_t *g = new_global(unique_name(name), type);

            g->is_definition = 1;
            g->is_static     = 1;

            if (consume("=")) {
                if (type->kind == TY_ARRAY && type->array_len < 0)
                    size_array_from_initializer(type);
                g->init_len  = type->size;
                g->init_data = wcc_alloc((size_t)type->size);
                global_initializer(g, type, 0);
            }

            scope_add(name)->var = g;
            continue;
        }

        if (type->kind == TY_VOID)
            error_at(tok->file, tok->line, "a variable cannot be void");

        if (consume("=")) {
            /* `char s[] = "..."` has to be measured before the variable is
             * made, because its size decides where in the frame it goes.
             * Measuring puts the tokens back for the real read. */
            if (type->kind == TY_ARRAY && type->array_len < 0)
                size_array_from_initializer(type);

            obj_t *var = new_local(name, type);
            scope_add(name)->var = var;

            node_t *init = local_initializer(var, tok);
            if (init)
                tail = tail->next = init;
            continue;
        }

        obj_t *var = new_local(name, type);
        scope_add(name)->var = var;
    }

    node_t *block = new_node(ND_BLOCK, tok);
    block->body = head.next;
    return block;
}

/* Assignments that put an initialiser into a local. */
/* An initialiser for something inside a local: `target` is an expression that
 * names where the value goes -- a variable, an element, a member -- and this
 * appends the assignments that fill it in.  Recursive, because
 * `{ {1, 2}, {3, 4} }` is initialisers inside an initialiser.
 */
static void init_into(node_t *target, type_t *type, const token_t *t,
                      node_t **tail)
{
    /* A string filling a char array is copied a byte at a time.  There is no
     * memcpy this compiler can call that is guaranteed to be linked. */
    if (type->kind == TY_ARRAY && type->base->size == 1 &&
        tok->kind == TK_STRING) {
        int len = tok->string_len;

        if (type->array_len < 0) {
            type->array_len = len;
            type->size      = len;
        }
        if (len > type->size)
            len = type->size;

        for (int i = 0; i < len; i++) {
            node_t *base = target;
            add_type(base);

            node_t *byte_at = new_unary(ND_DEREF,
                new_binary(ND_ADD, new_cast(base, pointer_to(ty_char)),
                           new_long(i, t), t), t);

            node_t *assign = new_binary(ND_ASSIGN, byte_at,
                new_number((unsigned char)tok->string[i], t), t);
            add_type(assign);

            *tail = (*tail)->next = new_unary(ND_EXPR_STMT, assign, t);
        }

        advance();
        return;
    }

    if (token_is(tok, "{")) {
        expect("{");

        if (type->kind == TY_ARRAY) {
            int i = 0;

            while (!consume("}")) {
                if (i && !consume(","))
                    { expect("}"); break; }
                if (consume("}"))
                    break;                      /* a trailing comma */

                node_t *base = target;
                add_type(base);

                node_t *element = new_unary(ND_DEREF,
                    make_add(base, new_number(i, t), t), t);
                add_type(element);

                init_into(element, type->base, t, tail);
                i++;
            }

            if (type->array_len < 0) {
                type->array_len = i;
                type->size      = i * type->base->size;
            }
            return;
        }

        if (type->kind == TY_STRUCT || type->kind == TY_UNION) {
            member_t *m = type->members;
            int       first = 1;

            while (m && !token_is(tok, "}")) {
                if (!first && !consume(","))
                    break;
                if (token_is(tok, "}"))
                    break;
                first = 0;

                if (token_is(tok, ".")) {
                    advance();
                    char *field = token_name(tok);
                    advance();
                    expect("=");

                    m = find_member(type, field, tok);
                }

                node_t *base = target;
                add_type(base);

                node_t *field = new_unary(ND_MEMBER, base, t);
                field->member = m;
                add_type(field);

                init_into(field, m->type, t, tail);

                if (type->kind == TY_UNION)
                    break;
                m = m->next;
            }

            consume(",");
            expect("}");
            return;
        }

        /* `int x = { 1 };` -- legal, and means what it looks like. */
        init_into(target, type, t, tail);
        consume(",");
        expect("}");
        return;
    }

    node_t *value  = assignment();
    node_t *assign = new_binary(ND_ASSIGN, target, value, t);

    add_type(assign);
    *tail = (*tail)->next = new_unary(ND_EXPR_STMT, assign, t);
}

/* Everything a local's initialiser turns into: a block of assignments.
 *
 * Anything not mentioned is left alone rather than zeroed, which is where this
 * differs from the standard.  Zeroing the rest would mean emitting a store per
 * byte of the gap, and the programs here initialise either everything or
 * nothing.
 */
static node_t *local_initializer(obj_t *var, const token_t *t)
{
    node_t  head;
    node_t *tail = &head;

    head.next = NULL;

    node_t *target = new_var_node(var, t);
    add_type(target);

    init_into(target, var->type, t, &tail);

    node_t *block = new_node(ND_BLOCK, t);
    block->body = head.next;
    return block;
}

static node_t *statement(void)
{
    const token_t *t = tok;

    if (token_is(tok, "{"))
        return compound_statement();

    if (consume("return")) {
        node_t *n = new_node(ND_RETURN, t);

        if (!consume(";")) {
            node_t *value = expression();
            add_type(value);
            n->lhs = new_cast(value, current_function->type->return_type);
            expect(";");
        }
        return n;
    }

    if (consume("if")) {
        node_t *n = new_node(ND_IF, t);

        expect("(");
        n->cond = expression();
        expect(")");
        n->then = statement();

        if (consume("else"))
            n->els = statement();

        return n;
    }

    if (consume("while")) {
        node_t *n = new_node(ND_FOR, t);
        int saved_break    = current_break_label;
        int saved_continue = current_continue_label;

        n->break_label    = current_break_label    = ++next_label_id;
        n->continue_label = current_continue_label = ++next_label_id;

        expect("(");
        n->cond = expression();
        expect(")");
        n->then = statement();

        current_break_label    = saved_break;
        current_continue_label = saved_continue;
        return n;
    }

    if (consume("do")) {
        node_t *n = new_node(ND_DO, t);
        int saved_break    = current_break_label;
        int saved_continue = current_continue_label;

        n->break_label    = current_break_label    = ++next_label_id;
        n->continue_label = current_continue_label = ++next_label_id;

        n->then = statement();

        if (!consume("while"))
            error_at(tok->file, tok->line, "a do needs its while");
        expect("(");
        n->cond = expression();
        expect(")");
        expect(";");

        current_break_label    = saved_break;
        current_continue_label = saved_continue;
        return n;
    }

    if (consume("for")) {
        node_t *n = new_node(ND_FOR, t);
        int saved_break    = current_break_label;
        int saved_continue = current_continue_label;

        n->break_label    = current_break_label    = ++next_label_id;
        n->continue_label = current_continue_label = ++next_label_id;

        expect("(");
        enter_scope();

        if (is_type_name(tok)) {
            storage_t storage;
            memset(&storage, 0, sizeof(storage));
            type_t *base = declspec(&storage);
            n->init = local_declaration(base, &storage);
        } else {
            n->init = expression_statement();
        }

        if (!token_is(tok, ";"))
            n->cond = expression();
        expect(";");

        if (!token_is(tok, ")"))
            n->step = expression();
        expect(")");

        n->then = statement();
        leave_scope();

        current_break_label    = saved_break;
        current_continue_label = saved_continue;
        return n;
    }

    if (consume("switch")) {
        node_t *n = new_node(ND_SWITCH, t);
        node_t *saved_switch = current_switch;
        int     saved_break  = current_break_label;

        expect("(");
        n->cond = expression();
        add_type(n->cond);
        expect(")");

        n->break_label = current_break_label = ++next_label_id;
        current_switch = n;

        n->then = statement();

        current_switch      = saved_switch;
        current_break_label = saved_break;
        return n;
    }

    if (consume("case")) {
        if (!current_switch)
            error_at(t->file, t->line, "case outside a switch");

        node_t *n = new_node(ND_CASE, t);
        n->value    = constant_expression();
        n->label_id = ++next_label_id;
        expect(":");
        n->lhs = statement();

        /* The switch needs the list to build its comparisons from, on a link
         * of its own: this node is also a statement in the block it was
         * written in, and that list uses `next`. */
        n->case_next = current_switch->cases;
        current_switch->cases = n;
        return n;
    }

    if (consume("default")) {
        if (!current_switch)
            error_at(t->file, t->line, "default outside a switch");

        node_t *n = new_node(ND_CASE, t);
        n->label_id = ++next_label_id;
        expect(":");
        n->lhs = statement();

        current_switch->default_case = n;
        return n;
    }

    if (consume("break")) {
        if (!current_break_label)
            error_at(t->file, t->line, "break outside a loop or switch");

        node_t *n = new_node(ND_BREAK, t);
        n->label_id = current_break_label;
        expect(";");
        return n;
    }

    if (consume("continue")) {
        if (!current_continue_label)
            error_at(t->file, t->line, "continue outside a loop");

        node_t *n = new_node(ND_CONTINUE, t);
        n->label_id = current_continue_label;
        expect(";");
        return n;
    }

    if (consume("goto")) {
        node_t *n = new_node(ND_GOTO, t);
        n->label = token_name(tok);
        advance();
        expect(";");
        return n;
    }

    /* `name:` is a label, and only a label -- everything else beginning with
     * an identifier is an expression. */
    if (tok->kind == TK_IDENT && token_is(tok->next, ":")) {
        node_t *n = new_node(ND_LABEL, t);
        n->label = token_name(tok);
        advance();
        advance();
        n->lhs = statement();
        return n;
    }

    return expression_statement();
}

static node_t *compound_statement(void)
{
    const token_t *t = tok;
    node_t  head;
    node_t *tail = &head;

    head.next = NULL;
    expect("{");
    enter_scope();

    while (!consume("}")) {
        if (tok->kind == TK_EOF)
            error_at(t->file, t->line, "this block is never closed");

        if (is_type_name(tok) && !token_is(tok->next, ":")) {
            storage_t storage;
            memset(&storage, 0, sizeof(storage));

            type_t *base = declspec(&storage);

            /* `struct foo { ... };` inside a function declares a type and
             * nothing else. */
            if (consume(";"))
                continue;

            tail = tail->next = local_declaration(base, &storage);
            continue;
        }

        tail = tail->next = statement();
        add_type(tail);
    }

    leave_scope();

    node_t *n = new_node(ND_BLOCK, t);
    n->body = head.next;
    return n;
}

/* ------------------------------------------------------------------ *
 *  The top level
 * ------------------------------------------------------------------ */

/* Give every local a place in the frame.  Offsets are negative from rbp, and
 * each variable is aligned to its own requirement. */
static void assign_local_offsets(obj_t *function)
{
    int offset = 0;

    for (obj_t *v = function->locals; v; v = v->next) {
        offset += v->type->size;
        offset = (offset + v->type->align - 1) / v->type->align *
                 v->type->align;
        v->offset = -offset;
    }

    /* A variadic function needs somewhere to put the six argument registers,
     * because va_arg walks them as if they had been on the stack all along.
     * Six eight-byte slots, at the bottom of the frame. */
    if (function->type->is_variadic) {
        offset = (offset + 7) / 8 * 8 + 48;
        function->va_offset = -offset;
    }

    /* The System V ABI wants the stack 16-byte aligned at a call. */
    function->stack_size = (offset + 15) / 16 * 16;
}

static void function_definition(type_t *type, char *name, storage_t *storage)
{
    obj_t *fn = NULL;

    /* A function declared before and defined now is the same object. */
    scope_entry_t *e = scope_find(name);
    if (e && e->var && e->var->is_function)
        fn = e->var;

    if (!fn) {
        fn = new_global(name, type);
        fn->is_function = 1;
        scope_add(name)->var = fn;
    }

    fn->type      = type;
    fn->is_static = storage->is_static;

    if (consume(";"))
        return;                        /* only a declaration after all */

    fn->is_definition = 1;

    current_function = fn;
    fn->locals = NULL;
    local_tail = &fn->locals;

    enter_scope();

    /* The parameters are locals that arrive already filled in. */
    obj_t head;
    obj_t *tail = &head;
    head.next = NULL;

    for (obj_t *p = type->params; p; p = p->next) {
        obj_t *var = new_local(p->name, p->type);
        scope_add(p->name)->var = var;

        obj_t *copy = wcc_alloc(sizeof(*copy));
        *copy = *var;
        copy->next = NULL;
        tail = tail->next = copy;
    }
    fn->params = head.next;

    /* The parameter objects the body refers to are the locals, so the list
     * the code generator walks has to be the same ones. */
    {
        obj_t *p = fn->params;
        obj_t *v = fn->locals;
        while (p && v) {
            p->offset = 0;
            p = p->next;
            v = v->next;
        }
    }

    fn->body = compound_statement();
    leave_scope();

    assign_local_offsets(fn);

    /* Now that the locals have their offsets, the parameter list can point at
     * the same places: the prologue writes the incoming registers there. */
    {
        obj_t *p = fn->params;
        obj_t *v = fn->locals;
        while (p && v) {
            p->offset = v->offset;
            p = p->next;
            v = v->next;
        }
    }

    current_function = NULL;
}

static void global_declaration(type_t *base, storage_t *storage)
{
    int first = 1;

    while (!consume(";")) {
        if (!first)
            expect(",");
        first = 0;

        char   *name = NULL;
        type_t *type = declarator(base, &name);

        skip_attribute();

        if (!name)
            error_at(tok->file, tok->line, "this declaration has no name");

        if (storage->is_typedef) {
            scope_add(name)->type_def = type;
            continue;
        }

        if (type->kind == TY_FUNC) {
            if (token_is(tok, "{")) {
                function_definition(type, name, storage);
                return;
            }

            obj_t *fn = NULL;
            scope_entry_t *e = scope_find(name);

            if (e && e->var)
                fn = e->var;
            else {
                fn = new_global(name, type);
                fn->is_function = 1;
                fn->is_static   = storage->is_static;
                scope_add(name)->var = fn;
            }
            continue;
        }

        obj_t *g = NULL;
        scope_entry_t *e = scope_find(name);

        if (e && e->var && !e->var->is_local) {
            g = e->var;
            g->type = type;
        } else {
            g = new_global(name, type);
            scope_add(name)->var = g;
        }

        g->is_static = storage->is_static;
        g->is_extern = storage->is_extern;

        if (consume("=")) {
            if (type->kind == TY_ARRAY && type->array_len < 0)
                size_array_from_initializer(type);

            g->is_definition = 1;
            g->init_len      = type->size;
            g->init_data     = wcc_alloc((size_t)(type->size ? type->size : 1));
            global_initializer(g, type, 0);
            g->type = type;
        } else if (!storage->is_extern) {
            /* A definition with no initialiser: zeroes, in .bss. */
            g->is_definition = 1;
        }
    }
}

/* `__builtin_va_list` is the compiler's own type, not a library's: it has to
 * exist before any header is read, because <wkernel.h> declares wvsnprintf()
 * in terms of it without including <stdarg.h>.  The shape is the one the
 * x86-64 ABI describes, and <stdarg.h> is a typedef and three macros on top of
 * it. */
static void install_builtin_types(void)
{
    type_t *record = new_type(TY_STRUCT, 24, 8);
    member_t *members = NULL;
    member_t *tail = NULL;

    struct { const char *name; type_t *type; int offset; } fields[] = {
        { "gp_offset",         ty_uint,             0 },
        { "fp_offset",         ty_uint,             4 },
        { "overflow_arg_area", pointer_to(ty_void), 8 },
        { "reg_save_area",     pointer_to(ty_void), 16 },
    };

    for (unsigned i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
        member_t *m = wcc_alloc(sizeof(*m));

        m->name   = fields[i].name;
        m->type   = fields[i].type;
        m->offset = fields[i].offset;

        if (tail)
            tail = tail->next = m;
        else
            members = tail = m;
    }

    record->members    = members;
    record->is_defined = 1;

    /* An array of one, so that passing a va_list passes its address -- which
     * is what makes va_list work as a parameter without the & every caller
     * would otherwise have to write. */
    scope_add("__builtin_va_list")->type_def = array_of(record, 1);
}

obj_t *parse(token_t *tokens)
{
    tok = tokens;
    globals = global_tail = NULL;

    enter_scope();
    install_builtin_types();

    while (tok->kind != TK_EOF) {
        skip_attribute();

        if (consume(";"))
            continue;

        storage_t storage;
        memset(&storage, 0, sizeof(storage));

        type_t *base = declspec(&storage);

        /* `struct foo { ... };` with no declarator names a type only. */
        if (consume(";"))
            continue;

        global_declaration(base, &storage);
    }

    leave_scope();
    return globals;
}
