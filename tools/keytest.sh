#!/bin/sh
# Boot WOS headless and type into it through the QEMU monitor.
#
# The PS/2 keyboard cannot be driven from a pipe, so interactive behaviour is
# tested by translating ASCII into QEMU `sendkey` commands and feeding them to
# the monitor.  Serial output is captured to build/serial.log and printed.
#
#   usage: tools/keytest.sh [-d SECONDS] [-t SECONDS] [-s FILE.png]
#                           'line one' 'line two' ...
#
#     -d  seconds to wait before typing (default 6), so the kernel is ready
#     -t  overall QEMU timeout (default 40)
#     -s  capture the VGA screen to a PNG once the typing is done
#
# Each argument is typed and followed by Enter.  Backslash escapes are
# interpreted, so \t sends Tab and \b sends Backspace.
#
# -s exists because the serial log only shows the byte stream; it cannot show
# what a full-screen program actually painted on the VGA console.

set -e
cd "$(dirname "$0")/.."

DELAY=6
LIMIT=40
SHOT=
while [ $# -gt 0 ]; do
    case "$1" in
        -d) DELAY=$2; shift 2 ;;
        -t) LIMIT=$2; shift 2 ;;
        -s) SHOT=$2; shift 2 ;;
        *)  break ;;
    esac
done

LOG=build/serial.log

# Translate one character into a QEMU key name, printing the sendkey command.
emit_char() {
    c=$1
    case "$c" in
        [a-z0-9]) echo "sendkey $c" ;;
        [A-Z])    echo "sendkey shift-$(printf '%s' "$c" | tr 'A-Z' 'a-z')" ;;
        ' ')  echo "sendkey spc" ;;
        '/')  echo "sendkey slash" ;;
        '-')  echo "sendkey minus" ;;
        '.')  echo "sendkey dot" ;;
        ',')  echo "sendkey comma" ;;
        ';')  echo "sendkey semicolon" ;;
        '=')  echo "sendkey equal" ;;
        '_')  echo "sendkey shift-minus" ;;
        '*')  echo "sendkey shift-8" ;;
        '~')  echo "sendkey shift-grave_accent" ;;
        ':')  echo "sendkey shift-semicolon" ;;
        '!')  echo "sendkey shift-1" ;;
        '@')  echo "sendkey shift-2" ;;
        '#')  echo "sendkey shift-3" ;;
        '$')  echo "sendkey shift-4" ;;
        '%')  echo "sendkey shift-5" ;;
        '^')  echo "sendkey shift-6" ;;
        '&')  echo "sendkey shift-7" ;;
        '(')  echo "sendkey shift-9" ;;
        ')')  echo "sendkey shift-0" ;;
        '+')  echo "sendkey shift-equal" ;;
        '?')  echo "sendkey shift-slash" ;;
        '<')  echo "sendkey shift-comma" ;;
        '>')  echo "sendkey shift-dot" ;;
        '[')  echo "sendkey bracket_left" ;;
        ']')  echo "sendkey bracket_right" ;;
        '{')  echo "sendkey shift-bracket_left" ;;
        '}')  echo "sendkey shift-bracket_right" ;;
        '|')  echo "sendkey shift-backslash" ;;
        '\\') echo "sendkey backslash" ;;
        "'")  echo "sendkey apostrophe" ;;
        '"')  echo "sendkey shift-apostrophe" ;;
        '`')  echo "sendkey grave_accent" ;;
        "$(printf '\t')") echo "sendkey tab" ;;
        "$(printf '\b')") echo "sendkey backspace" ;;
        *)    echo "# unmapped character: $c" >&2 ;;
    esac
}

emit_script() {
    sleep "$DELAY"
    for raw in "$@"; do
        # An argument starting with '@' is a list of QEMU key names sent as
        # they are, with no trailing Enter.  Full-screen programs read single
        # keystrokes, so they cannot be driven by typed lines.
        #   e.g. '@down down q'  presses Down twice and then q
        case "$raw" in
            @*)
                for k in ${raw#@}; do
                    echo "sendkey $k"
                    sleep 0.25
                done
                sleep 0.5
                continue
                ;;
        esac

        # Interpret backslash escapes so a test can ask for a Tab keypress
        # by writing \t, which is otherwise painful to pass through argv.
        line=$(printf '%b' "$raw")
        # Walk the characters with parameter expansion rather than `fold`.
        # fold treats a backspace as column-decrementing and does not split
        # on it, so "\b\b" arrived as one unmapped two-character token and
        # only one of the two keystrokes was ever sent.
        rest=$line
        while [ -n "$rest" ]; do
            ch=${rest%"${rest#?}"}
            rest=${rest#?}
            emit_char "$ch"
            sleep 0.03
        done
        echo "sendkey ret"
        sleep 0.6
    done
    sleep 1

    if [ -n "$SHOT" ]; then
        echo "screendump $PPM"
        sleep 1
    fi

    echo "quit"
}

PPM=$(pwd)/build/screen.ppm

rm -f "$LOG" "$PPM"
emit_script "$@" | timeout "$LIMIT" qemu-system-i386 -m 256M \
    -cdrom build/wos.iso \
    -drive file=build/wos.img,format=raw,if=ide,index=0,media=disk \
    -boot d -no-reboot \
    -serial "file:$LOG" -display none -monitor stdio >/dev/null 2>&1 || true

if [ -n "$SHOT" ]; then
    if [ -f "$PPM" ]; then
        convert "$PPM" "$SHOT"
        echo "screenshot: $SHOT"
    else
        echo "screenshot: QEMU produced no screen dump" >&2
    fi
fi

cat "$LOG"
