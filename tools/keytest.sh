#!/bin/sh
# Boot WOS headless and type into it through the QEMU monitor.
#
# The PS/2 keyboard cannot be driven from a pipe, so interactive behaviour is
# tested by translating ASCII into QEMU `sendkey` commands and feeding them to
# the monitor.  Serial output is captured to build/serial.log and printed.
#
#   usage: tools/keytest.sh [-d SECONDS] [-t SECONDS] 'line one' 'line two' ...
#
#     -d  seconds to wait before typing (default 6), so the kernel is ready
#     -t  overall QEMU timeout (default 40)
#
# Each argument is typed and followed by Enter.

set -e
cd "$(dirname "$0")/.."

DELAY=6
LIMIT=40
while [ $# -gt 0 ]; do
    case "$1" in
        -d) DELAY=$2; shift 2 ;;
        -t) LIMIT=$2; shift 2 ;;
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
        *)    echo "# unmapped character: $c" >&2 ;;
    esac
}

emit_script() {
    sleep "$DELAY"
    for line in "$@"; do
        # Split the line into characters without relying on bash-isms.
        # The trailing newline matters: without it `read` sees the final
        # character as an unterminated line, sets the variable and *then*
        # returns non-zero, so the while loop drops the last keystroke.
        printf '%s\n' "$line" | fold -w1 | while IFS= read -r ch; do
            emit_char "$ch"
            sleep 0.03
        done
        echo "sendkey ret"
        sleep 0.6
    done
    sleep 1
    echo "quit"
}

rm -f "$LOG"
emit_script "$@" | timeout "$LIMIT" qemu-system-i386 -m 256M \
    -cdrom build/wos.iso \
    -drive file=build/wos.img,format=raw,if=ide,index=0,media=disk \
    -boot d -no-reboot \
    -serial "file:$LOG" -display none -monitor stdio >/dev/null 2>&1 || true

cat "$LOG"
