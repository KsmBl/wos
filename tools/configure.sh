#!/usr/bin/env bash
#
# configure.sh -- pick the build settings, then build.
#
# The same settings config.mk holds, presented as a menu: the on/off ones are
# ticked and unticked, the ones with a value are typed in, and what is written
# back out is config.mk itself.  Nothing here is a second source of truth --
# the file this edits is the file make reads, so a setting changed by hand and
# a setting changed here are the same setting.
#
#   tools/configure.sh            the menu
#   tools/configure.sh --show     print the current settings and exit
#
set -uo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/.." || exit 1

CONFIG=config.mk

# ---------------------------------------------------------------------------
# Colours, when the terminal is one
# ---------------------------------------------------------------------------

if [[ -t 1 ]]; then
    BOLD=$'\e[1m'; DIM=$'\e[2m'; RESET=$'\e[0m'
    GREEN=$'\e[32m'; YELLOW=$'\e[33m'; CYAN=$'\e[36m'; RED=$'\e[31m'
else
    BOLD=''; DIM=''; RESET=''; GREEN=''; YELLOW=''; CYAN=''; RED=''
fi

# ---------------------------------------------------------------------------
# The settings
#
# One row each: key, kind, and the one-line description the menu shows.  `bool`
# is ticked and unticked; `number` and `text` are typed in.  Adding a setting
# here and in config.mk is all it takes for it to appear.
# ---------------------------------------------------------------------------

KEYS=(SELFTEST DISK_MB KHEAP_MB QEMU_MEM TIMEOUT)

declare -A KIND=(
    [SELFTEST]=bool
    [DISK_MB]=number
    [KHEAP_MB]=number
    [QEMU_MEM]=text
    [TIMEOUT]=number
)

declare -A LABEL=(
    [SELFTEST]="Boot-time self-tests"
    [DISK_MB]="Filesystem image size"
    [KHEAP_MB]="Kernel heap arena"
    [QEMU_MEM]="Memory for make run"
    [TIMEOUT]="Seconds make log waits"
)

declare -A UNIT=(
    [DISK_MB]="MiB"
    [KHEAP_MB]="MiB"
    [TIMEOUT]="s"
)

declare -A HELP=(
    [SELFTEST]="The four blocks of [ok  ] lines at boot, including a process that faults on purpose. Off leaves them out of the build entirely, so the kernel is about 40 KiB smaller and goes straight to the shell."
    [DISK_MB]="The installed system is about 4 MiB; the rest is room to write into. On a machine that cannot read its own disk the whole image is held in memory instead, and then this is memory rather than disk."
    [KHEAP_MB]="Everything kmalloc() hands out: process control blocks, kernel stacks, an executable image while it loads. Reserved at boot whether it is used or not. Below 4, spawning several processes at once starts to fail."
    [QEMU_MEM]="What make run and make log give the virtual machine. Worth raising past a gigabyte now and then: several bugs in this kernel only appeared there, where firmware stops putting everything in low memory."
    [TIMEOUT]="How long make log lets the machine run before it captures the serial log. Long enough for the self-tests, if they are on."
)

declare -A VALUE

# ---------------------------------------------------------------------------
# Reading and writing config.mk
#
# The file is edited in place rather than rewritten, so the comments in it --
# which are most of it -- survive being configured.
# ---------------------------------------------------------------------------

read_config() {
    local key
    for key in "${KEYS[@]}"; do
        VALUE[$key]=$(sed -n "s/^[[:space:]]*$key[[:space:]]*?\{0,1\}=[[:space:]]*\([^#]*\).*/\1/p" \
                      "$CONFIG" 2>/dev/null | tail -1 | sed 's/[[:space:]]*$//')
    done

    # Defaults, if the file is missing or says nothing about a setting.
    : "${VALUE[SELFTEST]:=1}"
    : "${VALUE[DISK_MB]:=64}"
    : "${VALUE[KHEAP_MB]:=8}"
    : "${VALUE[QEMU_MEM]:=256M}"
    : "${VALUE[TIMEOUT]:=12}"
}

write_config() {
    if [[ ! -f $CONFIG ]]; then
        printf '# Build settings.  See tools/configure.sh.\n\n' > "$CONFIG"
        local key
        for key in "${KEYS[@]}"; do
            printf '%s ?= %s\n' "$key" "${VALUE[$key]}" >> "$CONFIG"
        done
        return
    fi

    local key tmp
    tmp=$(mktemp)
    cp "$CONFIG" "$tmp"

    for key in "${KEYS[@]}"; do
        if grep -q "^[[:space:]]*$key[[:space:]]*?\{0,1\}=" "$tmp"; then
            sed -i "s|^[[:space:]]*$key[[:space:]]*?\{0,1\}=.*|$key ?= ${VALUE[$key]}|" "$tmp"
        else
            printf '\n%s ?= %s\n' "$key" "${VALUE[$key]}" >> "$tmp"
        fi
    done

    mv "$tmp" "$CONFIG"
}

# ---------------------------------------------------------------------------
# The menu
# ---------------------------------------------------------------------------

show_value() {
    local key=$1

    if [[ ${KIND[$key]} == bool ]]; then
        if [[ ${VALUE[$key]} == 0 ]]; then
            printf '%s[ ]%s off' "$DIM" "$RESET"
        else
            printf '%s[x]%s on' "$GREEN" "$RESET"
        fi
    else
        printf '%s%s%s %s' "$CYAN" "${VALUE[$key]}" "$RESET" "${UNIT[$key]:-}"
    fi
}

draw() {
    printf '\n%sWOS build settings%s   %s(%s)%s\n\n' \
           "$BOLD" "$RESET" "$DIM" "$CONFIG" "$RESET"

    local i key
    for i in "${!KEYS[@]}"; do
        key=${KEYS[$i]}
        printf '  %s%d%s  %-24s %s\n' \
               "$BOLD" "$((i + 1))" "$RESET" "${LABEL[$key]}" "$(show_value "$key")"
    done

    printf '\n  %sa%s  build now          %sruns make with these settings%s\n' \
           "$BOLD" "$RESET" "$DIM" "$RESET"
    printf '  %sd%s  restore defaults\n' "$BOLD" "$RESET"
    printf '  %sq%s  save and quit      %sconfig.mk is written either way%s\n\n' \
           "$BOLD" "$RESET" "$DIM" "$RESET"
}

edit() {
    local key=$1

    if [[ ${KIND[$key]} == bool ]]; then
        # Ticking a box needs no prompt: the choice is which of two it is.
        VALUE[$key]=$([[ ${VALUE[$key]} == 0 ]] && echo 1 || echo 0)
        return
    fi

    printf '\n%s%s%s\n' "$BOLD" "${LABEL[$key]}" "$RESET"
    printf '%s%s%s\n\n' "$DIM" "${HELP[$key]}" "$RESET"

    local answer
    read -r -p "  ${LABEL[$key]} [${VALUE[$key]}]: " answer || return
    [[ -z $answer ]] && return

    if [[ ${KIND[$key]} == number ]]; then
        if ! [[ $answer =~ ^[0-9]+$ ]] || (( answer == 0 )); then
            printf '  %sthat is not a number of %s%s\n' \
                   "$RED" "${UNIT[$key]:-units}" "$RESET"
            sleep 1
            return
        fi
    fi

    VALUE[$key]=$answer
}

show_help_for() {
    local key=$1
    printf '\n%s%s%s\n%s%s%s\n' \
           "$BOLD" "${LABEL[$key]}" "$RESET" "$DIM" "${HELP[$key]}" "$RESET"
    read -r -p $'\npress enter '
}

# ---------------------------------------------------------------------------

read_config

if [[ ${1:-} == --show ]]; then
    for key in "${KEYS[@]}"; do
        printf '%-9s = %s\n' "$key" "${VALUE[$key]}"
    done
    exit 0
fi

if [[ ! -t 0 ]]; then
    echo "configure.sh needs a terminal; use 'make VAR=value' instead" >&2
    exit 1
fi

while true; do
    draw
    read -r -n1 -p "  choose: " choice
    printf '\n'

    case $choice in
        [1-9])
            index=$((choice - 1))
            if (( index < ${#KEYS[@]} )); then
                key=${KEYS[$index]}
                # A bool has nothing to type, so show what it means first --
                # otherwise the tick would flip with no explanation of what it
                # just turned off.
                [[ ${KIND[$key]} == bool ]] && show_help_for "$key"
                edit "$key"
            fi
            ;;
        d|D)
            VALUE=([SELFTEST]=1 [DISK_MB]=64 [KHEAP_MB]=8 [QEMU_MEM]=256M [TIMEOUT]=12)
            ;;
        a|A)
            write_config
            printf '\n%s-- building --%s\n\n' "$BOLD" "$RESET"
            if make; then
                printf '\n%sBuilt.%s  make run to boot it, sudo tools/flash-usb.sh to write a stick.\n' \
                       "$GREEN" "$RESET"
                exit 0
            fi
            printf '\n%sThe build failed.%s\n' "$RED" "$RESET"
            exit 1
            ;;
        q|Q|'')
            write_config
            printf '\n%s written.  make, or tools/configure.sh again.\n' "$CONFIG"
            exit 0
            ;;
        *)
            ;;
    esac
done
