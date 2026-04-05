# Applications

Every application lives in `/app/<name>/`, with its executable at
`/app/<name>/launch` and its source in `/app/<name>/sourcecode/`. Typing the
bare name in `whell` runs it.

## A note on the "ports"

`fastfetch`, `htop`, `vim` and `fish` here are **WOS-native programs written in
the spirit of the originals, not builds of the upstream source.** That is not a
shortcut taken to save effort — the upstream programs cannot run here:

| Program | What it needs that WOS does not have |
|---|---|
| fastfetch | `/proc`, `/sys`, DRM and PCI ID databases, a full libc |
| htop | ncurses, `/proc`, signals, `ioctl` |
| vim | ~400k lines of C over a full libc, `termios`, `fork`, signals, regex |
| fish | a C++ runtime, `fork`/`exec` job control, `termios`, PCRE |

WOS has a 30-call kernel API, no `fork`, no signals, no `termios`, no dynamic
linking, and a 268 KiB limit on any single file. Building the real thing would
mean writing a POSIX layer several times the size of the whole system.

What is here instead: each program does what the original is *for*, using the
same key bindings and the same output shapes where they apply, so the muscle
memory transfers. Where a feature depends on something WOS lacks, it is absent
and said so rather than faked.

---

# fastfetch

```
fastfetch
```

Prints an ASCII WOS logo beside a summary of the machine.

```
                                wos@wos
                                ------
  __        __   ___    ____    OS: WOS 0.1 (i386)
  \ \      / /  / _ \  / ___|   Kernel: wos-0.1
   \ \ /\ / /  | | | | \___ \   Uptime: 8 seconds
    \ V  V /   | |_| |  ___) |  Shell: whell
     \_/\_/     \___/  |____/   Terminal: VGA 80x25
                                CPU: QEMU Virtual CPU version 2.5+
                                Memory: 5.3M / 255.8M
                                Disk (/): 406.0K / 64.0M (wfs)
                                Processes: 2
```

| Field | Where it comes from |
|---|---|
| Uptime | `wuptime_ms()` |
| CPU | the `CPUID` instruction, executed directly — it is unprivileged, so a ring 3 program can identify the processor without asking the kernel |
| Memory | `wmeminfo()` |
| Disk | `wdiskinfo()` |
| Processes | `wproclist()` |

The logo is ASCII rather than the block characters a real fetch tool uses. The
VGA font is code page 437 and the serial port usually reaches a UTF-8 terminal,
and no single byte sequence looks right on both.

The colour bar shows the normal eight colours as backgrounds and the bright
eight as foreground blocks — VGA text mode spends the top attribute bit on
blink rather than on a bright background.

**Exit status:** 0.

---

# htop

```
htop
```

A full-screen process and resource monitor. It refreshes once a second and
redraws immediately when a key is pressed.

```
 htop   a process monitor for WOS                      Uptime 00:00:08

  Mem [|                              5.3M/255.8M]
  Dsk [|                             476.0K/64.0M]

  Tasks: 2      Threads: 2      Kernel: 5.1M

   PID COMMAND       RESIDENT     CODE     DATA     HEAP    STACK  THR
     6 whell           100.0K    20.0K     8.0K       0B    64.0K    1
     7 htop             88.0K     8.0K     8.0K       0B    64.0K    1

 up/dn  Select  q  Quit   r  Refresh now
```

| Key | Effect |
|---|---|
| Up / Down, or `k` / `j` | move the selection |
| Home / End | first / last process |
| `q`, or Ctrl+C | quit |
| any other key | refresh immediately |

The meters change colour with load, as htop's do: green below 70%, yellow to
90%, red above. A non-zero amount always shows at least one bar, so a small
but real allocation does not read as nothing.

`RESIDENT` is what the process actually has mapped, counted from its page
tables, which is why it exceeds the four columns beside it — those do not
include the process's own page tables.

The display stays responsive because it never blocks: it polls with
`wpollin()` between repaints rather than waiting inside `wread()`, which would
freeze the clock until someone pressed a key.

**Exit status:** 0.
