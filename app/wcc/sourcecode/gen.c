/* The tree into x86-64 machine code.
 *
 * There is no assembler here and no assembly text: the bytes go straight into
 * a buffer.  That is a decision about what has to be ported, not about speed
 * -- an assembler is another program that would have to run on this machine
 * before the compiler was any use.
 *
 * The strategy is the simplest one that is correct: every expression leaves
 * its value in rax, and a binary operator evaluates its right side, pushes it,
 * evaluates its left side, and pops.  No register allocation, no attempt to
 * keep anything in a register between statements.  The code is bigger and
 * slower than gcc's by a wide margin, and it is code this machine can produce
 * for itself, which is the whole point.
 *
 * Addresses are 32-bit absolute.  A WOS program lives between 0x40000000 and
 * 0xBFFE0000, so `mov $symbol, %eax` reaches anything -- which is why there is
 * no position-independent code here and no GOT.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wcc.h"

static unit_t *unit;
static obj_t  *function;        /* the one being generated */
static int     depth;           /* 8-byte pushes since the frame was set up */

static buffer_t *text(void)
{
    return &unit->section[SEC_TEXT];
}

static int here(void)
{
    return text()->len;
}

/* ------------------------------------------------------------------ *
 *  Emitting bytes
 * ------------------------------------------------------------------ */

static void byte(int b)               { buf_byte(text(), b); }
static void u32(unsigned v)           { buf_u32(text(), v); }
static void u64(unsigned long v)      { buf_u64(text(), v); }

static void bytes2(int a, int b)      { byte(a); byte(b); }
static void bytes3(int a, int b, int c) { byte(a); byte(b); byte(c); }
static void bytes4(int a, int b, int c, int d)
{
    byte(a); byte(b); byte(c); byte(d);
}

/* push rax / pop into a register, with the depth kept so a call can align the
 * stack the way the ABI insists on. */
static void push(void)
{
    byte(0x50);                     /* push %rax */
    depth++;
}

static void pop_into(int reg)
{
    /* 58+r for rax..rdi, with a REX prefix for r8 and r9. */
    if (reg >= 8) {
        byte(0x41);
        byte(0x58 + (reg - 8));
    } else {
        byte(0x58 + reg);
    }
    depth--;
}

enum { REG_AX = 0, REG_CX = 1, REG_DX = 2, REG_BX = 3,
       REG_SP = 4, REG_BP = 5, REG_SI = 6, REG_DI = 7,
       REG_R8 = 8, REG_R9 = 9 };

/* The registers arguments arrive in, in order. */
static const int arg_regs[6] = { REG_DI, REG_SI, REG_DX, REG_CX, REG_R8, REG_R9 };

/* mov $value, %rax -- 32 bits when it fits, which is most of the time. */
static void load_immediate(long value)
{
    if (value >= 0 && value <= 0xFFFFFFFFL) {
        byte(0xB8);                 /* mov $imm32, %eax (zero-extends) */
        u32((unsigned)value);
    } else if (value >= -2147483648L && value < 0) {
        bytes3(0x48, 0xC7, 0xC0);   /* mov $imm32, %rax (sign-extends) */
        u32((unsigned)(long)value);
    } else {
        bytes2(0x48, 0xB8);         /* movabs $imm64, %rax */
        u64((unsigned long)value);
    }
}

/* mov $symbol, %eax, with the address left for the linker. */
static void load_symbol_address(const char *name, long addend)
{
    byte(0xB8);
    unit_reloc(unit, SEC_TEXT, here(), name, R_X86_64_32, addend);
    u32(0);
}

/* lea offset(%rbp), %rax */
static void load_frame_address(int offset)
{
    bytes3(0x48, 0x8D, 0x85);
    u32((unsigned)offset);
}

/* ------------------------------------------------------------------ *
 *  Labels
 *
 *  A jump is emitted with a placeholder and remembered; when the label is
 *  placed, every jump waiting for it is filled in.  One pass, no fixed-point
 *  iteration, because every jump here is the 32-bit form.
 * ------------------------------------------------------------------ */

typedef struct patch {
    struct patch *next;
    int           id;
    int           at;          /* where the rel32 field is */
} patch_t;

typedef struct label_pos {
    struct label_pos *next;
    int               id;
    int               at;
} label_pos_t;

static patch_t     *patches;
static label_pos_t *labels;

static void place_label(int id)
{
    label_pos_t *l = wcc_alloc(sizeof(*l));

    l->id   = id;
    l->at   = here();
    l->next = labels;
    labels  = l;
}

static void jump_to(int id, int condition)
{
    if (condition < 0) {
        byte(0xE9);                       /* jmp rel32 */
    } else {
        bytes2(0x0F, 0x80 + condition);   /* jcc rel32 */
    }

    patch_t *p = wcc_alloc(sizeof(*p));
    p->id   = id;
    p->at   = here();
    p->next = patches;
    patches = p;

    u32(0);
}

#define COND_E   0x04
#define COND_NE  0x05

static void resolve_labels(void)
{
    for (patch_t *p = patches; p; p = p->next) {
        int target = -1;

        for (label_pos_t *l = labels; l; l = l->next)
            if (l->id == p->id) {
                target = l->at;
                break;
            }

        if (target < 0)
            fatal("internal: label %d was never placed", p->id);

        /* rel32 counts from the end of the instruction, which is where the
         * field itself ends. */
        int rel = target - (p->at + 4);
        unsigned char *at = text()->data + p->at;

        for (int i = 0; i < 4; i++)
            at[i] = (unsigned char)((unsigned)rel >> (i * 8));
    }

    patches = NULL;
    labels  = NULL;
}

/* Labels a program wrote itself, which are matched by name. */
typedef struct named_label {
    struct named_label *next;
    const char         *name;
    int                 id;
} named_label_t;

static named_label_t *named_labels;

static int label_for_name(const char *name)
{
    for (named_label_t *l = named_labels; l; l = l->next)
        if (strcmp(l->name, name) == 0)
            return l->id;

    static int next_named_id = 100000;
    named_label_t *l = wcc_alloc(sizeof(*l));

    l->name = name;
    l->id   = next_named_id++;
    l->next = named_labels;
    named_labels = l;
    return l->id;
}

/* ------------------------------------------------------------------ *
 *  Loading and storing through rax
 * ------------------------------------------------------------------ */

/* Replace the address in rax with what is at that address. */
static void load_from_rax(type_t *type)
{
    if (type->kind == TY_ARRAY || type->kind == TY_STRUCT ||
        type->kind == TY_UNION || type->kind == TY_FUNC) {
        /* An array or a structure is used by its address, not by its value:
         * there is nothing to load. */
        return;
    }

    switch (type->size) {
    case 1:
        if (type->is_unsigned)
            bytes4(0x48, 0x0F, 0xB6, 0x00);   /* movzbq (%rax), %rax */
        else
            bytes4(0x48, 0x0F, 0xBE, 0x00);   /* movsbq (%rax), %rax */
        break;
    case 2:
        if (type->is_unsigned)
            bytes4(0x48, 0x0F, 0xB7, 0x00);   /* movzwq */
        else
            bytes4(0x48, 0x0F, 0xBF, 0x00);   /* movswq */
        break;
    case 4:
        if (type->is_unsigned)
            bytes2(0x8B, 0x00);               /* movl (%rax), %eax */
        else
            bytes3(0x48, 0x63, 0x00);         /* movslq (%rax), %rax */
        break;
    default:
        bytes3(0x48, 0x8B, 0x00);             /* movq (%rax), %rax */
        break;
    }
}

/* Store the value on the top of the stack at the address in rax, and leave
 * the value in rax afterwards -- which is what an assignment produces.
 *
 * This way round, rather than the address being the pushed one, because of
 * setjmp.  `x = setjmp(env)` has to work when longjmp comes back to it, and
 * execution resumes just after the call with everything pushed before it long
 * since overwritten.  Evaluating the value first means nothing has to survive
 * the call on the stack. */
static void store_from_stack(type_t *type)
{
    pop_into(REG_DI);                         /* the value        */

    if (type->kind == TY_STRUCT || type->kind == TY_UNION) {
        /* rdi is the source address and rax the destination; `rep movsb`
         * wants them the other way about, and wants the destination kept
         * because it moves rdi as it goes. */
        bytes3(0x48, 0x89, 0xFE);             /* mov %rdi, %rsi  */
        bytes3(0x49, 0x89, 0xC2);             /* mov %rax, %r10  */
        bytes3(0x48, 0x89, 0xC7);             /* mov %rax, %rdi  */
        byte(0xB9);                           /* mov $size, %ecx */
        u32((unsigned)type->size);
        bytes2(0xF3, 0xA4);                   /* rep movsb       */
        bytes3(0x4C, 0x89, 0xD0);             /* mov %r10, %rax  */
        return;
    }

    switch (type->size) {
    /* The byte case needs a REX prefix even though nothing about it is
     * 64-bit: without one, the register field 7 means %bh rather than %dil.
     * That is the oldest trap in x86-64 encoding and it assembles quietly. */
    case 1: bytes3(0x40, 0x88, 0x38); break;        /* mov %dil, (%rax) */
    case 2: bytes3(0x66, 0x89, 0x38); break;        /* mov %di, (%rax)  */
    case 4: bytes2(0x89, 0x38); break;              /* mov %edi, (%rax) */
    default: bytes3(0x48, 0x89, 0x38); break;       /* mov %rdi, (%rax) */
    }

    bytes3(0x48, 0x89, 0xF8);                       /* mov %rdi, %rax  */
}

/* Narrow or widen rax to a type, which is what a cast between integers is. */
static void cast_rax(type_t *from, type_t *to)
{
    if (to->kind == TY_VOID)
        return;

    if (to->kind == TY_BOOL) {
        /* Anything non-zero becomes 1. */
        bytes3(0x48, 0x85, 0xC0);             /* test %rax, %rax */
        bytes3(0x0F, 0x95, 0xC0);             /* setne %al       */
        bytes3(0x0F, 0xB6, 0xC0);             /* movzbl %al, %eax */
        return;
    }

    if (!is_integer(to) || !from)
        return;

    switch (to->size) {
    case 1:
        if (to->is_unsigned)
            bytes4(0x48, 0x0F, 0xB6, 0xC0);   /* movzbq %al, %rax */
        else
            bytes4(0x48, 0x0F, 0xBE, 0xC0);   /* movsbq %al, %rax */
        break;
    case 2:
        if (to->is_unsigned)
            bytes4(0x48, 0x0F, 0xB7, 0xC0);
        else
            bytes4(0x48, 0x0F, 0xBF, 0xC0);
        break;
    case 4:
        if (to->is_unsigned)
            bytes2(0x89, 0xC0);               /* mov %eax, %eax (clears top) */
        else
            bytes3(0x48, 0x63, 0xC0);         /* movslq %eax, %rax */
        break;
    default:
        /* Widening to 64 bits: an unsigned source is already zero-extended
         * by whatever produced it, a signed 32-bit one needs the sign. */
        if (from->size == 4 && !from->is_unsigned)
            bytes3(0x48, 0x63, 0xC0);
        break;
    }
}

/* ------------------------------------------------------------------ *
 *  Expressions
 * ------------------------------------------------------------------ */

static void gen_expr(node_t *node);
static void gen_stmt(node_t *node);

/* Leave the address of something addressable in rax. */
static void gen_address(node_t *node)
{
    switch (node->kind) {
    case ND_VAR:
        if (node->var->is_local)
            load_frame_address(node->var->offset);
        else
            load_symbol_address(node->var->name, 0);
        return;

    case ND_DEREF:
        gen_expr(node->lhs);
        return;

    case ND_MEMBER:
        gen_address(node->lhs);
        if (node->member->offset) {
            bytes2(0x48, 0x05);                /* add $imm32, %rax */
            u32((unsigned)node->member->offset);
        }
        return;

    case ND_COMMA:
        gen_expr(node->lhs);
        gen_address(node->rhs);
        return;

    default:
        error_at(node->token->file, node->token->line,
                 "this does not have an address");
    }
}

/* The comparison instructions, once the two values are in rax and rdi. */
static void compare_and_set(node_kind_t kind, type_t *operand_type)
{
    bytes3(0x48, 0x39, 0xF8);                 /* cmp %rdi, %rax */

    int condition;
    int is_unsigned = operand_type && operand_type->is_unsigned;

    switch (kind) {
    case ND_EQ: condition = 0x94; break;      /* sete  */
    case ND_NE: condition = 0x95; break;      /* setne */
    case ND_LT: condition = is_unsigned ? 0x92 : 0x9C; break;  /* setb/setl  */
    case ND_LE: condition = is_unsigned ? 0x96 : 0x9E; break;  /* setbe/setle */
    default:    condition = 0x94; break;
    }

    bytes3(0x0F, condition, 0xC0);            /* setcc %al        */
    bytes3(0x0F, 0xB6, 0xC0);                 /* movzbl %al, %eax */
}

static int label_counter = 200000;

/* va_start is the one thing in <stdarg.h> the compiler has to do itself: only
 * it knows where the six argument registers were spilled and where the stack
 * arguments begin.  Everything after that -- stepping through them -- is
 * ordinary C, and lives in the library as __va_start and __va_arg.
 */
static void gen_va_start(node_t *node)
{
    if (!function || !function->type->is_variadic)
        error_at(node->token->file, node->token->line,
                 "va_start outside a function with '...'");

    if (!node->args)
        error_at(node->token->file, node->token->line,
                 "va_start needs the va_list");

    int named = 0;
    for (obj_t *p = function->params; p; p = p->next)
        named++;

    gen_expr(node->args);                     /* the va_list's address */
    bytes3(0x48, 0x89, 0xC7);                 /* mov %rax, %rdi        */

    bytes3(0x48, 0x8D, 0xB5);                 /* lea save(%rbp), %rsi  */
    u32((unsigned)function->va_offset);

    bytes4(0x48, 0x8D, 0x55, 0x10);           /* lea 16(%rbp), %rdx    */

    byte(0xB9);                               /* mov $named*8, %ecx    */
    u32((unsigned)(named * 8));

    bytes2(0xB0, 0x00);                       /* mov $0, %al           */

    /* The stack is aligned here: nothing has been pushed since the prologue
     * that has not been popped, which is what `depth` is for. */
    int pad = depth % 2;
    if (pad)
        bytes4(0x48, 0x83, 0xEC, 0x08);

    byte(0xE8);
    unit_reloc(unit, SEC_TEXT, here(), "__va_start", R_X86_64_PLT32, -4);
    u32(0);

    if (pad)
        bytes4(0x48, 0x83, 0xC4, 0x08);
}

static void gen_funcall(node_t *node)
{
    int count = 0;

    if (node->funcname && strcmp(node->funcname, "__builtin_va_start") == 0) {
        gen_va_start(node);
        return;
    }

    for (node_t *arg = node->args; arg; arg = arg->next)
        count++;

    /* The first six arguments go in registers and the rest on the stack, with
     * the seventh at the lowest address.  Everything is pushed and then the
     * first six are popped off again, which puts the stack ones exactly where
     * the ABI says and needs no arithmetic to work out where that is.
     *
     * That means evaluating right to left.  C does not say which order
     * arguments are evaluated in, and this is the order that makes the layout
     * fall out of the pushes. */
    int on_stack = count > 6 ? count - 6 : 0;

    /* rsp must be a multiple of 16 at the call.  `depth` is what has been
     * pushed since the prologue, which left it aligned; the stack arguments
     * are still to come, so both are counted before deciding. */
    int pad = (depth + on_stack) % 2;

    if (pad) {
        bytes4(0x48, 0x83, 0xEC, 0x08);       /* sub $8, %rsp */
        depth++;
    }

    for (int i = count - 1; i >= 0; i--) {
        node_t *arg = node->args;

        for (int k = 0; k < i; k++)
            arg = arg->next;

        gen_expr(arg);
        push();
    }

    /* Calling through a pointer: the address is worked out before the
     * argument registers are loaded, and parked in r10 -- which is neither an
     * argument register nor one the callee has to preserve. */
    if (!node->funcname) {
        gen_expr(node->lhs);
        bytes3(0x49, 0x89, 0xC2);             /* mov %rax, %r10 */
    }

    for (int i = 0; i < count && i < 6; i++)
        pop_into(arg_regs[i]);

    /* al holds how many vector registers carry arguments.  Nothing here uses
     * any, and a variadic callee reads it, so it has to be zero. */
    bytes2(0xB0, 0x00);                       /* mov $0, %al */

    if (node->funcname) {
        /* A name called without being declared still needs a symbol, or the
         * relocation would have nothing to point at.  Undefined here means
         * the linker looks elsewhere, which is exactly right. */
        symbol_t *sym = unit_symbol(unit, node->funcname);
        if (!sym->is_defined) {
            sym->is_global   = 1;
            sym->is_function = 1;
        }

        byte(0xE8);                           /* call rel32 */
        unit_reloc(unit, SEC_TEXT, here(), node->funcname, R_X86_64_PLT32, -4);
        u32(0);
    } else {
        bytes3(0x41, 0xFF, 0xD2);             /* call *%r10 */
    }

    int to_drop = on_stack + pad;
    if (to_drop) {
        bytes3(0x48, 0x81, 0xC4);             /* add $imm32, %rsp */
        u32((unsigned)(to_drop * 8));
        depth -= to_drop;
    }

    /* A function returning something narrower than 64 bits leaves the top
     * bits undefined, so the caller has to make them mean something. */
    if (node->type && is_integer(node->type) && node->type->size < 8)
        cast_rax(ty_int, node->type);
}

static void gen_expr(node_t *node)
{
    switch (node->kind) {
    case ND_NUM:
        load_immediate(node->value);
        return;

    case ND_VAR:
    case ND_MEMBER:
        gen_address(node);
        load_from_rax(node->type);
        return;

    case ND_DEREF:
        gen_expr(node->lhs);
        load_from_rax(node->type);
        return;

    case ND_ADDR:
        gen_address(node->lhs);
        return;

    case ND_ASSIGN:
        /* The value first, then where it goes: see store_from_stack(). */
        gen_expr(node->rhs);
        push();
        gen_address(node->lhs);
        store_from_stack(node->lhs->type);
        return;

    case ND_CAST:
        gen_expr(node->lhs);
        cast_rax(node->lhs->type, node->type);
        return;

    case ND_COMMA:
        gen_expr(node->lhs);
        gen_expr(node->rhs);
        return;

    case ND_NEG:
        gen_expr(node->lhs);
        bytes3(0x48, 0xF7, 0xD8);             /* neg %rax */
        return;

    case ND_BITNOT:
        gen_expr(node->lhs);
        bytes3(0x48, 0xF7, 0xD0);             /* not %rax */
        return;

    case ND_NOT:
        gen_expr(node->lhs);
        bytes3(0x48, 0x85, 0xC0);             /* test %rax, %rax  */
        bytes3(0x0F, 0x94, 0xC0);             /* sete %al         */
        bytes3(0x0F, 0xB6, 0xC0);             /* movzbl %al, %eax */
        return;

    case ND_AND: {
        int false_label = ++label_counter;
        int end_label   = ++label_counter;

        gen_expr(node->lhs);
        bytes3(0x48, 0x85, 0xC0);
        jump_to(false_label, COND_E);
        gen_expr(node->rhs);
        bytes3(0x48, 0x85, 0xC0);
        jump_to(false_label, COND_E);
        load_immediate(1);
        jump_to(end_label, -1);
        place_label(false_label);
        load_immediate(0);
        place_label(end_label);
        return;
    }

    case ND_OR: {
        int true_label = ++label_counter;
        int end_label  = ++label_counter;

        gen_expr(node->lhs);
        bytes3(0x48, 0x85, 0xC0);
        jump_to(true_label, COND_NE);
        gen_expr(node->rhs);
        bytes3(0x48, 0x85, 0xC0);
        jump_to(true_label, COND_NE);
        load_immediate(0);
        jump_to(end_label, -1);
        place_label(true_label);
        load_immediate(1);
        place_label(end_label);
        return;
    }

    case ND_COND: {
        int else_label = ++label_counter;
        int end_label  = ++label_counter;

        gen_expr(node->cond);
        bytes3(0x48, 0x85, 0xC0);
        jump_to(else_label, COND_E);
        gen_expr(node->then);
        jump_to(end_label, -1);
        place_label(else_label);
        gen_expr(node->els);
        place_label(end_label);
        return;
    }

    case ND_FUNCALL:
        gen_funcall(node);
        return;

    case ND_STMT_EXPR:
        for (node_t *n = node->body; n; n = n->next)
            gen_stmt(n);
        return;

    default:
        break;
    }

    /* Everything left is a binary operator: right side first, so that the
     * left one ends up in rax where the instructions want it. */
    gen_expr(node->rhs);
    push();
    gen_expr(node->lhs);
    pop_into(REG_DI);

    int is_long = node->lhs->type && node->lhs->type->size == 8;
    int rex = is_long ? 0x48 : 0x40;

    switch (node->kind) {
    case ND_ADD:
        if (is_long) bytes3(0x48, 0x01, 0xF8);      /* add %rdi, %rax */
        else         bytes2(0x01, 0xF8);            /* add %edi, %eax */
        return;

    case ND_SUB:
        if (is_long) bytes3(0x48, 0x29, 0xF8);
        else         bytes2(0x29, 0xF8);
        return;

    case ND_MUL:
        if (is_long) bytes4(0x48, 0x0F, 0xAF, 0xC7);  /* imul %rdi, %rax */
        else         bytes3(0x0F, 0xAF, 0xC7);
        return;

    case ND_DIV:
    case ND_MOD:
        if (node->type->is_unsigned) {
            bytes2(0x31, 0xD2);                       /* xor %edx, %edx  */
            if (is_long) bytes3(0x48, 0xF7, 0xF7);    /* div %rdi        */
            else         bytes2(0xF7, 0xF7);
        } else {
            if (is_long) { bytes2(0x48, 0x99);        /* cqo             */
                           bytes3(0x48, 0xF7, 0xFF); }/* idiv %rdi       */
            else         { byte(0x99);                /* cdq             */
                           bytes2(0xF7, 0xFF); }
        }

        /* The remainder comes back in rdx, so a modulo moves it over. */
        if (node->kind == ND_MOD)
            bytes3(0x48, 0x89, 0xD0);                 /* mov %rdx, %rax  */
        return;

    case ND_BITAND:
        if (is_long) bytes3(0x48, 0x21, 0xF8);
        else         bytes2(0x21, 0xF8);
        return;

    case ND_BITOR:
        if (is_long) bytes3(0x48, 0x09, 0xF8);
        else         bytes2(0x09, 0xF8);
        return;

    case ND_BITXOR:
        if (is_long) bytes3(0x48, 0x31, 0xF8);
        else         bytes2(0x31, 0xF8);
        return;

    case ND_SHL:
        bytes3(0x48, 0x89, 0xF9);                     /* mov %rdi, %rcx  */
        if (is_long) bytes3(0x48, 0xD3, 0xE0);        /* shl %cl, %rax   */
        else         bytes2(0xD3, 0xE0);
        return;

    case ND_SHR:
        bytes3(0x48, 0x89, 0xF9);
        if (node->type->is_unsigned) {
            if (is_long) bytes3(0x48, 0xD3, 0xE8);    /* shr %cl, %rax   */
            else         bytes2(0xD3, 0xE8);
        } else {
            if (is_long) bytes3(0x48, 0xD3, 0xF8);    /* sar %cl, %rax   */
            else         bytes2(0xD3, 0xF8);
        }
        return;

    case ND_EQ: case ND_NE: case ND_LT: case ND_LE:
        compare_and_set(node->kind, node->lhs->type);
        return;

    default:
        (void)rex;
        error_at(node->token->file, node->token->line,
                 "internal: no code for this expression");
    }
}

/* ------------------------------------------------------------------ *
 *  Statements
 * ------------------------------------------------------------------ */

static int return_label;

static void gen_stmt(node_t *node)
{
    if (!node)
        return;

    switch (node->kind) {
    case ND_NULL:
        return;

    case ND_EXPR_STMT:
        gen_expr(node->lhs);
        return;

    case ND_BLOCK:
        for (node_t *n = node->body; n; n = n->next)
            gen_stmt(n);
        return;

    case ND_RETURN:
        if (node->lhs)
            gen_expr(node->lhs);
        jump_to(return_label, -1);
        return;

    case ND_IF: {
        int else_label = ++label_counter;
        int end_label  = ++label_counter;

        gen_expr(node->cond);
        bytes3(0x48, 0x85, 0xC0);
        jump_to(else_label, COND_E);
        gen_stmt(node->then);
        jump_to(end_label, -1);
        place_label(else_label);
        gen_stmt(node->els);
        place_label(end_label);
        return;
    }

    case ND_FOR: {
        int top = ++label_counter;

        gen_stmt(node->init);
        place_label(top);

        if (node->cond) {
            gen_expr(node->cond);
            bytes3(0x48, 0x85, 0xC0);
            jump_to(node->break_label, COND_E);
        }

        gen_stmt(node->then);
        place_label(node->continue_label);

        if (node->step)
            gen_expr(node->step);

        jump_to(top, -1);
        place_label(node->break_label);
        return;
    }

    case ND_DO: {
        int top = ++label_counter;

        place_label(top);
        gen_stmt(node->then);
        place_label(node->continue_label);
        gen_expr(node->cond);
        bytes3(0x48, 0x85, 0xC0);
        jump_to(top, COND_NE);
        place_label(node->break_label);
        return;
    }

    case ND_SWITCH: {
        gen_expr(node->cond);

        /* One comparison per case.  A jump table would be faster and would
         * also need the cases to be dense, which is a property this compiler
         * would have to check for; the comparisons are always right. */
        for (node_t *c = node->cases; c; c = c->case_next) {
            push();
            load_immediate(c->value);
            bytes3(0x48, 0x89, 0xC7);           /* mov %rax, %rdi */
            pop_into(REG_AX);
            bytes3(0x48, 0x39, 0xF8);           /* cmp %rdi, %rax */
            jump_to(c->label_id, COND_E);
        }

        if (node->default_case)
            jump_to(node->default_case->label_id, -1);
        else
            jump_to(node->break_label, -1);

        gen_stmt(node->then);
        place_label(node->break_label);
        return;
    }

    case ND_CASE:
        place_label(node->label_id);
        gen_stmt(node->lhs);
        return;

    case ND_BREAK:
    case ND_CONTINUE:
        jump_to(node->label_id, -1);
        return;

    case ND_GOTO:
        jump_to(label_for_name(node->label), -1);
        return;

    case ND_LABEL:
        place_label(label_for_name(node->label));
        gen_stmt(node->lhs);
        return;

    default:
        gen_expr(node);
        return;
    }
}

/* ------------------------------------------------------------------ *
 *  Functions
 * ------------------------------------------------------------------ */

static void gen_function(obj_t *fn)
{
    function = fn;
    depth    = 0;
    patches  = NULL;
    labels   = NULL;
    named_labels = NULL;
    return_label = ++label_counter;

    buf_align(text(), 16);

    symbol_t *sym = unit_symbol(unit, fn->name);
    sym->section     = SEC_TEXT;
    sym->value       = here();
    sym->is_defined  = 1;
    sym->is_function = 1;
    sym->is_global   = !fn->is_static;

    fn->code_offset = here();

    /* The prologue: a frame pointer, and room for the locals. */
    byte(0x55);                                   /* push %rbp       */
    bytes3(0x48, 0x89, 0xE5);                     /* mov %rsp, %rbp  */

    if (fn->stack_size) {
        bytes3(0x48, 0x81, 0xEC);                 /* sub $imm32, %rsp */
        u32((unsigned)fn->stack_size);
    }

    /* A variadic function spills all six argument registers into its frame
     * before anything else.  va_arg walks that area and then the stack ones,
     * which is what the ABI says a va_list is. */
    if (fn->type->is_variadic) {
        int offsets[6] = { 0, 8, 16, 24, 32, 40 };
        int regs[6]    = { REG_DI, REG_SI, REG_DX, REG_CX, REG_R8, REG_R9 };

        for (int k = 0; k < 6; k++) {
            int reg = regs[k];

            /* mov %reg, (va_offset + k*8)(%rbp) */
            byte(0x48 | ((reg >= 8) ? 0x04 : 0x00));
            byte(0x89);
            byte(0x85 | ((reg & 7) << 3));
            u32((unsigned)(fn->va_offset + offsets[k]));
        }
    }

    /* The parameters arrive in registers and live in the frame like any other
     * local, so the first thing a function does is put them there. */
    int i = 0;
    for (obj_t *p = fn->params; p && i < 6; p = p->next, i++) {
        int reg = arg_regs[i];
        int size = p->type->size;

        /* mov %reg, offset(%rbp), in the width the parameter actually is. */
        int rex = 0x48 | ((reg >= 8) ? 0x04 : 0x00);
        int modrm = 0x85 | ((reg & 7) << 3);

        if (size == 1) {
            if (reg >= 8) byte(0x44); else if (reg >= 4) byte(0x40);
            bytes2(0x88, modrm);
        } else if (size == 2) {
            byte(0x66);
            if (reg >= 8) byte(0x44);
            bytes2(0x89, modrm);
        } else if (size == 4) {
            if (reg >= 8) byte(0x44);
            bytes2(0x89, modrm);
        } else {
            bytes2(rex, 0x89);
            byte(modrm);
        }
        u32((unsigned)p->offset);
    }

    gen_stmt(fn->body);

    /* Falling off the end of a function returns whatever is in rax, which for
     * main() would be a surprise; zero it so a program with no return
     * statement exits with a status somebody chose. */
    place_label(return_label);
    bytes3(0x48, 0x89, 0xEC);                     /* mov %rbp, %rsp */
    byte(0x5D);                                   /* pop %rbp       */
    byte(0xC3);                                   /* ret            */

    resolve_labels();

    sym->size = here() - sym->value;
    function = NULL;
}

/* ------------------------------------------------------------------ *
 *  Data
 * ------------------------------------------------------------------ */

static void gen_data(obj_t *var)
{
    /* Read-only if it is a string literal or was declared const; this
     * compiler only knows the first, and puts the rest in .data. */
    int is_string = var->name[0] == '.';
    section_id_t section = is_string ? SEC_RODATA : SEC_DATA;

    symbol_t *sym = unit_symbol(unit, var->name);
    sym->is_global = !var->is_static;

    if (!var->is_definition) {
        /* Only a declaration: the symbol stays undefined and the linker
         * finds it somewhere else. */
        return;
    }

    sym->is_defined = 1;
    sym->size       = var->type->size;

    if (!var->init_data) {
        /* Zero-filled: .bss, which costs nothing in the file. */
        buffer_t *bss = &unit->section[SEC_BSS];

        while (bss->len % (var->type->align ? var->type->align : 1))
            bss->len++;

        sym->section = SEC_BSS;
        sym->value   = bss->len;
        bss->len    += var->type->size;
        if (bss->len > unit->bss_size)
            unit->bss_size = bss->len;
        return;
    }

    buffer_t *out = &unit->section[section];

    buf_align(out, var->type->align ? var->type->align : 1);
    sym->section = section;
    sym->value   = out->len;

    int base = out->len;
    buf_put(out, var->init_data, var->init_len);

    /* An initialiser that names an address is a relocation the linker fills
     * in, and eight bytes of zero until it does. */
    for (init_reloc_t *r = var->init_relocs; r; r = r->next)
        unit_reloc(unit, section, base + r->offset, r->symbol,
                   R_X86_64_64, r->addend);
}

void generate(unit_t *out, obj_t *program)
{
    unit = out;

    for (obj_t *o = program; o; o = o->next)
        if (!o->is_function)
            gen_data(o);

    for (obj_t *o = program; o; o = o->next)
        if (o->is_function && o->is_definition)
            gen_function(o);

    /* Anything mentioned and not defined here is somebody else's, and the
     * symbol table has to say so for the linker to look. */
    for (obj_t *o = program; o; o = o->next) {
        if (o->is_function && !o->is_definition) {
            symbol_t *sym = unit_symbol(unit, o->name);
            sym->is_global = 1;
        }
    }
}
