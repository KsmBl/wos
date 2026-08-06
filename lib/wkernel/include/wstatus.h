/* wstatus -- a line of text with the machine's figures in it.
 *
 * A desktop has two or three places where somebody writes a line and expects
 * the numbers in it to move: across the background, along a bar, in a window
 * previewing either.  The template is the same in all of them, so the names
 * and the readings behind them are here rather than in whichever program drew
 * it first.
 *
 *     background_text "WOS\ncpu ${CPU}   mem ${MEM}"
 *     bar status_text "${BATTERY}   ${DATE}  ${TIME}"
 *
 * | ${CPU}     | how much of the processors is busy, as a percentage      |
 * | ${MEM}     | how much of the memory is in use, as a percentage        |
 * | ${TIME}    | the clock, HH:MM                                         |
 * | ${DATE}    | the date, YYYY-MM-DD                                     |
 * | ${BATTERY} | "BAT 50%", "CHG 90%", "AC", or nothing without a battery |
 *
 * A name this does not know is left exactly as it was written, so a mistake in
 * a template looks like a mistake rather than quietly becoming nothing.
 *
 * The template is expanded when it is drawn and not when it is set, because
 * the figures move: ${CPU} is a rate, and a rate is the difference between two
 * readings.  A template expanded once would show the load at the moment
 * somebody typed it and never again.
 */
#ifndef WKERNEL_WSTATUS_H
#define WKERNEL_WSTATUS_H

#include <wkernel.h>

/**
 * Expand a template into @p out.
 *
 * The readings behind the names are taken at most once a second, however often
 * this is called: the clock cannot move faster than that in a form anybody
 * reads, and a load is a figure over an interval that has to be long enough to
 * have timer ticks in it.  So this is cheap enough to call every time round a
 * loop, which is what lets a caller redraw only when the answer changes.
 *
 * @param in   The template.  NULL or empty gives an empty string.
 * @param out  Where the expansion goes, always NUL-terminated.
 * @param size How big @p out is; the expansion is cut to fit.
 */
void wstatus_expand(const char *in, char *out, wsize_t size);

#endif /* WKERNEL_WSTATUS_H */
