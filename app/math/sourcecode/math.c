/* math -- evaluate an arithmetic expression, like fish's `math`.
 *
 *   math "2 + 3 * 4"      -> 14
 *   math '(1 + 2) / 4'    -> 0.75
 *   math 2 ^ 10           -> 1024
 *   math 'sqrt(2)'        -> 1.414213
 *
 * The expression can be one quoted argument or several bare ones -- they are
 * joined with spaces -- or, with no arguments, a line read from standard
 * input.
 *
 * WOS user programs are built with no FPU and no SSE, so there is no hardware
 * floating point to lean on.  Numbers are therefore fixed point: a 64-bit
 * integer of millionths, with 128-bit intermediates for multiply and divide.
 * That gives six decimal places, which is also fish's default.
 *
 * Grammar (lowest precedence first):
 *   expr   = term  (('+' | '-') term)*
 *   term   = power (('*' | '/' | '%') power)*
 *   power  = unary ('^' power)?              right associative
 *   unary  = ('+' | '-') unary | primary
 *   primary= number | '(' expr ')' | name '(' expr ')'
 */

#include <wkernel.h>

typedef long long fx;              /* fixed point: value * SCALE          */
#define SCALE 1000000LL            /* six decimal places                  */

static const char *cur;            /* parser cursor                       */
static int   had_error;
static char  errmsg[80];

static void fail(const char *what)
{
    if (!had_error) {              /* keep the first, most specific error */
        had_error = 1;
        strlcpy(errmsg, what, sizeof(errmsg));
    }
}

/* ------------------------------------------------------------------ *
 *  128-bit helpers
 *
 *  The fixed-point multiply and divide need a 128-bit intermediate, but this
 *  program is built with no libgcc, so the compiler's 128-bit divide
 *  (__divti3) is not available.  Everything below therefore works on an
 *  explicit hi:lo pair of 64-bit words, using only 64-bit operations.
 * ------------------------------------------------------------------ */

typedef unsigned long long u64;

/* Full 64x64 -> 128 product, split into 32-bit pieces so nothing overflows. */
static void umul64(u64 a, u64 b, u64 *hi, u64 *lo)
{
    u64 a0 = a & 0xFFFFFFFFu, a1 = a >> 32;
    u64 b0 = b & 0xFFFFFFFFu, b1 = b >> 32;

    u64 p00 = a0 * b0, p01 = a0 * b1, p10 = a1 * b0, p11 = a1 * b1;

    u64 mid   = p01 + p10;
    u64 mid_c = (mid < p01) ? (1ULL << 32) : 0;   /* carry out of bit 63 */

    u64 low   = p00 + (mid << 32);
    u64 low_c = (low < p00) ? 1 : 0;

    *lo = low;
    *hi = p11 + (mid >> 32) + mid_c + low_c;
}

/* (hi:lo) / d by restoring long division, assuming the quotient fits in 64
 * bits -- which it does for every value this calculator produces. */
static u64 udiv128(u64 hi, u64 lo, u64 d)
{
    u64 rem = 0, quo = 0;

    for (int i = 63; i >= 0; i--) {
        rem = (rem << 1) | ((hi >> i) & 1);
        quo <<= 1;
        if (rem >= d) { rem -= d; quo |= 1; }
    }
    for (int i = 63; i >= 0; i--) {
        rem = (rem << 1) | ((lo >> i) & 1);
        quo <<= 1;
        if (rem >= d) { rem -= d; quo |= 1; }
    }
    return quo;
}

static int ucmp128(u64 ahi, u64 alo, u64 bhi, u64 blo)
{
    if (ahi != bhi) return ahi < bhi ? -1 : 1;
    if (alo != blo) return alo < blo ? -1 : 1;
    return 0;
}

/* ------------------------------------------------------------------ *
 *  Fixed-point arithmetic
 * ------------------------------------------------------------------ */

static fx fx_mul(fx a, fx b)
{
    int neg = (a < 0) ^ (b < 0);
    u64 ua = (a < 0) ? -(u64)a : (u64)a;
    u64 ub = (b < 0) ? -(u64)b : (u64)b;

    u64 hi, lo;
    umul64(ua, ub, &hi, &lo);
    u64 q = udiv128(hi, lo, (u64)SCALE);
    return neg ? -(fx)q : (fx)q;
}

static fx fx_div(fx a, fx b)
{
    if (b == 0) {
        fail("division by zero");
        return 0;
    }
    int neg = (a < 0) ^ (b < 0);
    u64 ua = (a < 0) ? -(u64)a : (u64)a;
    u64 ub = (b < 0) ? -(u64)b : (u64)b;

    u64 hi, lo;
    umul64(ua, (u64)SCALE, &hi, &lo);      /* a * SCALE, then / b */
    u64 q = udiv128(hi, lo, ub);
    return neg ? -(fx)q : (fx)q;
}

/* Integer square root of the 128-bit value hi:lo, bit by bit from the top. */
static u64 isqrt_hilo(u64 nhi, u64 nlo)
{
    u64 res = 0;
    for (int bit = 63; bit >= 0; bit--) {
        u64 t = res | (1ULL << bit);
        u64 thi, tlo;
        umul64(t, t, &thi, &tlo);
        if (ucmp128(thi, tlo, nhi, nlo) <= 0)
            res = t;
    }
    return res;
}

static fx fx_sqrt(fx a)
{
    if (a < 0) {
        fail("sqrt of a negative number");
        return 0;
    }
    /* sqrt(a/SCALE)*SCALE == sqrt(a*SCALE). */
    u64 hi, lo;
    umul64((u64)a, (u64)SCALE, &hi, &lo);
    return (fx)isqrt_hilo(hi, lo);
}

/* base^exp, with an integer exponent (fixed point cannot do fractional ones). */
static fx fx_pow(fx base, fx exp)
{
    if (exp % SCALE != 0) {
        fail("exponent must be a whole number");
        return 0;
    }

    long long e = exp / SCALE;
    long long k = (e < 0) ? -e : e;

    fx result = SCALE;             /* 1.0 */
    for (long long i = 0; i < k; i++)
        result = fx_mul(result, base);

    if (e < 0)
        result = fx_div(SCALE, result);
    return result;
}

/* fmod: a - trunc(a/b)*b, so 7 % 3 == 1 and 5.5 % 2 == 1.5. */
static fx fx_mod(fx a, fx b)
{
    if (b == 0) {
        fail("modulo by zero");
        return 0;
    }
    fx q = fx_div(a, b);
    fx qi = (q / SCALE) * SCALE;    /* truncate toward zero */
    return a - fx_mul(qi, b);
}

/* ------------------------------------------------------------------ *
 *  Parser
 * ------------------------------------------------------------------ */

static void skip_ws(void)
{
    while (*cur == ' ' || *cur == '\t')
        cur++;
}

static fx parse_expr(void);

static fx parse_number(void)
{
    fx value = 0;

    while (*cur >= '0' && *cur <= '9') {
        value = value * 10 + (*cur - '0');
        cur++;
    }
    value *= SCALE;

    if (*cur == '.') {
        cur++;
        fx place = SCALE / 10;
        while (*cur >= '0' && *cur <= '9') {
            if (place > 0)
                value += (fx)(*cur - '0') * place;
            place /= 10;
            cur++;                 /* extra digits past six are dropped */
        }
    }
    return value;
}

static int name_is(const char *start, int len, const char *word)
{
    return (int)strlen(word) == len && strncmp(start, word, (wsize_t)len) == 0;
}

static fx parse_primary(void)
{
    skip_ws();

    if (*cur == '(') {
        cur++;
        fx v = parse_expr();
        skip_ws();
        if (*cur == ')')
            cur++;
        else
            fail("missing ')'");
        return v;
    }

    if (*cur >= '0' && *cur <= '9')
        return parse_number();

    /* A function name, e.g. sqrt(2) or abs(-3). */
    if ((*cur >= 'a' && *cur <= 'z') || (*cur >= 'A' && *cur <= 'Z')) {
        const char *start = cur;
        while ((*cur >= 'a' && *cur <= 'z') || (*cur >= 'A' && *cur <= 'Z'))
            cur++;
        int len = (int)(cur - start);

        skip_ws();
        if (*cur != '(') {
            fail("expected '(' after a function name");
            return 0;
        }
        cur++;
        fx arg = parse_expr();
        skip_ws();
        if (*cur == ')')
            cur++;
        else
            fail("missing ')'");

        if (name_is(start, len, "sqrt")) return fx_sqrt(arg);
        if (name_is(start, len, "abs"))  return arg < 0 ? -arg : arg;

        fail("unknown function");
        return 0;
    }

    fail("expected a number");
    return 0;
}

static fx parse_unary(void)
{
    skip_ws();
    if (*cur == '-') { cur++; return -parse_unary(); }
    if (*cur == '+') { cur++; return  parse_unary(); }
    return parse_primary();
}

static fx parse_power(void)
{
    fx base = parse_unary();
    skip_ws();
    if (*cur == '^') {
        cur++;
        fx exp = parse_power();     /* right associative */
        return fx_pow(base, exp);
    }
    return base;
}

static fx parse_term(void)
{
    fx v = parse_power();
    for (;;) {
        skip_ws();
        char op = *cur;
        if (op != '*' && op != '/' && op != '%')
            break;
        cur++;
        fx rhs = parse_power();
        if (op == '*') v = fx_mul(v, rhs);
        else if (op == '/') v = fx_div(v, rhs);
        else v = fx_mod(v, rhs);
    }
    return v;
}

static fx parse_expr(void)
{
    fx v = parse_term();
    for (;;) {
        skip_ws();
        char op = *cur;
        if (op != '+' && op != '-')
            break;
        cur++;
        fx rhs = parse_term();
        v = (op == '+') ? v + rhs : v - rhs;
    }
    return v;
}

/* ------------------------------------------------------------------ *
 *  Output
 * ------------------------------------------------------------------ */

static void print_fixed(fx v)
{
    if (v < 0) {
        wputs("-");
        v = -v;
    }

    long long ip = v / SCALE;
    long long fp = v % SCALE;

    /* Six-digit fraction, with trailing zeros trimmed; nothing at all when the
     * value is a whole number. */
    char frac[7];
    for (int i = 5; i >= 0; i--) {
        frac[i] = (char)('0' + fp % 10);
        fp /= 10;
    }
    frac[6] = '\0';

    int end = 6;
    while (end > 0 && frac[end - 1] == '0')
        end--;
    frac[end] = '\0';

    if (end > 0)
        wprintf("%lld.%s\n", ip, frac);
    else
        wprintf("%lld\n", ip);
}

int main(int argc, char **argv)
{
    char expr[512];
    expr[0] = '\0';

    if (argc > 1) {
        /* Join the arguments with spaces, so both `math "1 + 2"` and
         * `math 1 + 2` work. */
        int at = 0;
        for (int i = 1; i < argc && at < (int)sizeof(expr) - 1; i++) {
            if (i > 1 && at < (int)sizeof(expr) - 1)
                expr[at++] = ' ';
            for (const char *s = argv[i]; *s && at < (int)sizeof(expr) - 1; s++)
                expr[at++] = *s;
        }
        expr[at] = '\0';
    } else {
        /* No arguments: evaluate a line from standard input. */
        int n = wread(W_STDIN, expr, sizeof(expr) - 1);
        if (n <= 0)
            return 0;
        while (n > 0 && (expr[n - 1] == '\n' || expr[n - 1] == '\r'))
            n--;
        expr[n] = '\0';
    }

    cur = expr;
    skip_ws();
    if (*cur == '\0') {
        wfprintf(W_STDERR, "math: no expression\n");
        return 2;
    }

    fx result = parse_expr();
    skip_ws();

    if (!had_error && *cur != '\0')
        fail("unexpected trailing characters");

    if (had_error) {
        wfprintf(W_STDERR, "math: %s\n", errmsg);
        return 1;
    }

    print_fixed(result);
    return 0;
}
