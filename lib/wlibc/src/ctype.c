/* Character classes, by comparison rather than by table.
 *
 * A table would be faster and would also be 257 bytes that have to be indexed
 * correctly by every caller, including the ones that pass a signed char.  At
 * the sizes anything here works on -- a compiler reading a line of source --
 * the comparisons cost nothing worth measuring, and a negative argument
 * answers "no" instead of reading the byte before the table.
 */

#include <ctype.h>

int isascii(int c)  { return c >= 0 && c <= 127; }
int isdigit(int c)  { return c >= '0' && c <= '9'; }
int islower(int c)  { return c >= 'a' && c <= 'z'; }
int isupper(int c)  { return c >= 'A' && c <= 'Z'; }
int isalpha(int c)  { return islower(c) || isupper(c); }
int isalnum(int c)  { return isalpha(c) || isdigit(c); }
int isblank(int c)  { return c == ' ' || c == '\t'; }
int iscntrl(int c)  { return (c >= 0 && c < 32) || c == 127; }
int isgraph(int c)  { return c > 32 && c < 127; }
int isprint(int c)  { return c >= 32 && c < 127; }
int ispunct(int c)  { return isgraph(c) && !isalnum(c); }

int isspace(int c)
{
    return c == ' ' || c == '\t' || c == '\n' ||
           c == '\v' || c == '\f' || c == '\r';
}

int isxdigit(int c)
{
    return isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

int tolower(int c) { return isupper(c) ? c + ('a' - 'A') : c; }
int toupper(int c) { return islower(c) ? c - ('a' - 'A') : c; }
