/* ctest -- exercise the hosted C library.
 *
 * This program includes no WOS header at all.  <stdio.h>, <stdlib.h>,
 * <string.h> and the rest are the whole of what it knows about the machine,
 * which is the point of it: if this compiles and runs, then a program written
 * for Unix has somewhere to land here, and the compiler that is coming has a
 * library underneath it.
 *
 *     ctest [directory]
 *
 * The scratch files go in /ramdisk unless a directory is named -- give it one
 * on the disk to exercise WFS instead of the filesystem in memory.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <setjmp.h>
#include <time.h>
#include <limits.h>

static int checks;
static int failures;

static void check(int ok, const char *what)
{
    checks++;
    if (!ok)
        failures++;

    printf("  [%s] %s\n", ok ? "ok  " : "FAIL", what);
}

/* ------------------------------------------------------------------ *
 *  Streams
 * ------------------------------------------------------------------ */

static void test_streams(const char *dir)
{
    char path[256];
    snprintf(path, sizeof(path), "%s/ctest.tmp", dir);

    FILE *f = fopen(path, "w");
    check(f != NULL, "a file can be opened for writing");
    if (!f)
        return;

    int n = fprintf(f, "%s %d %04x|", "line", 42, 255);
    check(n == 13, "fprintf returns the length it produced");

    /* Larger than the 1 KiB buffer, so this covers the path where a write
     * goes straight to the kernel instead of through it. */
    char big[3000];
    memset(big, 'x', sizeof(big));
    check(fwrite(big, 1, sizeof(big), f) == sizeof(big),
          "a write larger than the buffer goes through whole");

    check(fputs("\nsecond line\nthird line\n", f) == 0, "fputs succeeds");
    check(fclose(f) == 0, "the file closes");

    f = fopen(path, "r");
    check(f != NULL, "and opens again for reading");
    if (!f)
        return;

    char line[64];
    check(fgets(line, sizeof(line), f) != NULL, "fgets returns a line");
    check(strncmp(line, "line 42 00ff|xxx", 16) == 0,
          "which starts with what fprintf wrote");

    check(fseek(f, 15, SEEK_SET) == 0, "fseek to an absolute position");
    check(ftell(f) == 15, "ftell agrees with it");

    char buf[8];
    check(fread(buf, 1, 4, f) == 4, "fread reads the count it was asked for");
    check(memcmp(buf, "xxxx", 4) == 0, "and the bytes that are there");
    check(ftell(f) == 19, "ftell tracks the buffered read");

    int c = fgetc(f);
    check(c == 'x', "fgetc reads one");
    check(ungetc(c, f) == 'x', "ungetc pushes it back");
    check(fgetc(f) == 'x', "and it comes out again");

    check(fseek(f, -11, SEEK_END) == 0, "fseek from the end");
    check(fgets(line, sizeof(line), f) != NULL, "reads the last line");
    check(strcmp(line, "third line\n") == 0, "which is the last line");

    check(fgetc(f) == EOF, "the end of the file reports EOF");
    check(feof(f) != 0, "and sets the end-of-file flag");

    rewind(f);
    check(ftell(f) == 0 && feof(f) == 0, "rewind goes back and clears it");
    check(fclose(f) == 0, "it closes again");

    /* Opening something that is not there fails and says why. */
    errno = 0;
    check(fopen("/no/such/file", "r") == NULL, "a missing file will not open");
    check(errno == ENOENT, "and errno says which way it failed");
    check(strcmp(strerror(ENOENT), "no such file or directory") == 0,
          "strerror names it");

    check(remove(path) == 0, "the scratch file can be removed");
    check(fopen(path, "r") == NULL, "and is gone");
}

/* ------------------------------------------------------------------ *
 *  stdlib
 * ------------------------------------------------------------------ */

static int compare_int(const void *a, const void *b)
{
    int left  = *(const int *)a;
    int right = *(const int *)b;

    return (left > right) - (left < right);
}

static void test_stdlib(void)
{
    /* Enough elements, and enough of them already in order, to go through the
     * partitioning rather than only the insertion sort. */
    int values[64];
    for (int i = 0; i < 64; i++)
        values[i] = (i * 37) % 64;

    qsort(values, 64, sizeof(values[0]), compare_int);

    int sorted = 1;
    for (int i = 0; i < 64; i++)
        if (values[i] != i)
            sorted = 0;
    check(sorted, "qsort puts 64 elements in order");

    int key = 39;
    int *found = bsearch(&key, values, 64, sizeof(values[0]), compare_int);
    check(found && *found == 39, "bsearch finds one of them");

    key = 100;
    check(bsearch(&key, values, 64, sizeof(values[0]), compare_int) == NULL,
          "and does not find one that is absent");

    char *end;
    check(strtol("  -1234xyz", &end, 10) == -1234 && strcmp(end, "xyz") == 0,
          "strtol reads a signed number and says where it stopped");
    check(strtol("0x2a", NULL, 0) == 42, "base 0 understands 0x");
    check(strtol("0755", NULL, 0) == 493, "and a leading zero");
    check(strtoul("ff", NULL, 16) == 255, "strtoul reads hexadecimal");

    errno = 0;
    check(strtol("99999999999999999999", NULL, 10) == LONG_MAX &&
          errno == ERANGE, "a number too large clamps and sets ERANGE");

    end = NULL;
    check(strtol("nothing", &end, 10) == 0 && end && *end == 'n',
          "text with no digits converts nothing");

    check(atoi("42") == 42 && atol("-7") == -7, "atoi and atol");
    check(abs(-5) == 5 && labs(-5L) == 5L, "abs and labs");
    check(getenv("PATH") == NULL, "there are no environment variables");

    srand(1);
    int first = rand();
    srand(1);
    check(rand() == first && first >= 0, "rand repeats from the same seed");
}

/* ------------------------------------------------------------------ *
 *  Strings, characters
 * ------------------------------------------------------------------ */

static void test_strings(void)
{
    char buf[16];

    memset(buf, '?', sizeof(buf));
    strncpy(buf, "abc", 8);
    check(memcmp(buf, "abc\0\0\0\0\0", 8) == 0 && buf[8] == '?',
          "strncpy pads to the count and no further");

    strcpy(buf, "ab");
    strncat(buf, "cdef", 2);
    check(strcmp(buf, "abcd") == 0, "strncat adds at most the count");

    char *copy = strdup("duplicated");
    check(copy && strcmp(copy, "duplicated") == 0, "strdup copies");
    free(copy);

    copy = strndup("duplicated", 3);
    check(copy && strcmp(copy, "dup") == 0, "strndup copies a prefix");
    free(copy);

    check(strspn("aabbcc", "ab") == 4, "strspn counts the accepted run");
    check(strcspn("aabbcc", "c") == 4, "strcspn counts up to the rejected one");
    char *hit = strpbrk("hello", "lo");
    check(hit && *hit == 'l', "strpbrk finds the first of a set");
    check(memchr("abcdef", 'd', 6) != NULL, "memchr finds a byte");
    check(memchr("abcdef", 'z', 6) == NULL, "and reports one that is absent");

    char text[] = "one,two,,three";
    char *state = NULL;
    char *tok   = strtok_r(text, ",", &state);
    check(tok && strcmp(tok, "one") == 0, "strtok_r returns the first field");
    tok = strtok_r(NULL, ",", &state);
    check(tok && strcmp(tok, "two") == 0, "and the second");
    tok = strtok_r(NULL, ",", &state);
    check(tok && strcmp(tok, "three") == 0, "and skips an empty one");
    check(strtok_r(NULL, ",", &state) == NULL, "then says there are no more");

    check(isdigit('7') && !isdigit('a'), "isdigit");
    check(isspace(' ') && isspace('\n') && !isspace('x'), "isspace");
    check(isalpha('Z') && isalnum('0') && ispunct(',') && !isprint('\n'),
          "the other character classes");
    check(toupper('a') == 'A' && tolower('A') == 'a' && toupper('1') == '1',
          "tolower and toupper");
    check(!isdigit(EOF) && !isalpha(EOF), "EOF is not any class of character");
}

/* ------------------------------------------------------------------ *
 *  setjmp, and printing into memory
 * ------------------------------------------------------------------ */

static jmp_buf escape;

/* Never returns: every path either recurses or leaves by longjmp.  Saying so
 * is what stops the compiler from calling this an infinite recursion, which is
 * what it looks like to anything that does not know where the exit is. */
static void deep(int depth) __attribute__((noreturn));

static void deep(int depth)
{
    if (depth == 0)
        longjmp(escape, 7);

    /* Something in a callee-saved register across the call, so a longjmp that
     * failed to restore rbx or r12 would be visible. */
    volatile int keep = depth * 3;
    deep(depth - 1);
    (void)keep;
}

static void test_setjmp(void)
{
    volatile int marker = 12345;
    int value = setjmp(escape);

    if (value == 0) {
        deep(8);
        check(0, "longjmp did not come back");
        return;
    }

    check(value == 7, "longjmp returns the value it was given");
    check(marker == 12345, "and the frame it returned to is intact");

    /* Zero is the one value longjmp may not deliver. */
    if (setjmp(escape) == 0)
        longjmp(escape, 0);
    else
        check(1, "longjmp(env, 0) arrives as 1");
}

static void test_printf(void)
{
    char buf[64];

    int n = snprintf(buf, sizeof(buf), "%d %s %c %x %ld %%",
                     -12, "str", 'c', 0xbeef, 1234567890123L);
    check(strcmp(buf, "-12 str c beef 1234567890123 %") == 0 && n == 30,
          "snprintf formats what it is given");

    n = snprintf(buf, 8, "%s", "much longer than eight");
    check(n == 22 && strcmp(buf, "much lo") == 0,
          "and truncates while reporting the length it needed");

    check(snprintf(buf, sizeof(buf), "%5d|%-5d|%05d", 42, 42, 42) == 17 &&
          strcmp(buf, "   42|42   |00042") == 0, "widths and padding");
}

static void test_time(void)
{
    time_t now = time(NULL);
    check(now > 1600000000L, "time() is later than the year 2020");

    time_t copy = 0;
    time(&copy);
    check(copy >= now, "and stores it through the pointer as well");

    struct tm *t = localtime(&now);
    check(t != NULL && t->tm_year > 100 && t->tm_mon >= 0 && t->tm_mon < 12 &&
          t->tm_mday >= 1 && t->tm_mday <= 31, "localtime breaks it down");

    if (t)
        check(mktime(t) == now, "and mktime puts it back together");

    clock_t start = clock();
    check(clock() >= start, "clock() does not go backwards");
}

int main(int argc, char **argv)
{
    const char *dir = (argc > 1) ? argv[1] : "/ramdisk";

    printf("ctest -- the hosted C library, in %s\n\n", dir);

    printf("streams\n");
    test_streams(dir);
    printf("stdlib\n");
    test_stdlib();
    printf("strings and characters\n");
    test_strings();
    printf("setjmp\n");
    test_setjmp();
    printf("formatted output\n");
    test_printf();
    printf("time\n");
    test_time();

    printf("\n%d checks, %d failed\n", checks, failures);
    printf(failures ? "ctest: FAILED\n" : "ctest: all passed\n");

    return failures ? 1 : 0;
}
