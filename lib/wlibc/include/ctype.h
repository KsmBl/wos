/* <ctype.h> -- what kind of character this is.
 *
 * ASCII only, which is all the console draws and all a C compiler needs to
 * read a source file.  Each takes an int because the standard says so: the
 * value is either an unsigned char or EOF, and passing a plain `char` that
 * happens to be negative is the classic way to walk off the front of a table.
 * There is no table here -- the tests are comparisons -- so a negative
 * argument answers "no" rather than reading memory it should not.
 */
#ifndef WLIBC_CTYPE_H
#define WLIBC_CTYPE_H

int isalnum(int c);
int isalpha(int c);
int isascii(int c);
int isblank(int c);
int iscntrl(int c);
int isdigit(int c);
int isgraph(int c);
int islower(int c);
int isprint(int c);
int ispunct(int c);
int isspace(int c);
int isupper(int c);
int isxdigit(int c);

int tolower(int c);
int toupper(int c);

#endif /* WLIBC_CTYPE_H */
