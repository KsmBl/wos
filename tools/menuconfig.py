#!/usr/bin/env python3
"""Choose what goes into the build.

    make menuconfig

Three lists: the drivers the kernel is built with, the programs that go onto
the disk, and a handful of settings that change how the kernel itself is
built.  Everything is written back to config.mk, which is the same file the
Makefile has always read -- so what this produces can equally well be edited
by hand, and a setting changed here is a setting changed for `make` too.

Nothing is applied until it is saved, and quitting with unsaved changes asks
first.
"""

import curses
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CONFIG = os.path.join(ROOT, "config.mk")

# ---------------------------------------------------------------------------
# What can be turned off
#
# Drivers not in this list are not optional: the console, the keyboard, the
# PCI bus and the interrupt controllers are how the machine works at all, and
# a build without them would not boot to anything that could say so.
# ---------------------------------------------------------------------------

DRIVERS = [
    ("RTL8139", "Realtek RTL8139 wired network card (QEMU's -device rtl8139)"),
    ("IWLWIFI", "Intel Wireless-AC 9000 series -- the wireless adapter"),
    ("XHCI",    "USB host controller, and the disk behind it"),
    ("ATA",     "IDE/SATA disk"),
    ("MOUSE",   "PS/2 pointer"),
    ("BATTERY", "battery status, through ACPI"),
]

# Settings that are neither a driver nor a program.  A bool is a tick; an int
# is typed in.
ADVANCED = [
    ("SMP",      "bool", "use every processor, not just the first"),
    ("SELFTEST", "bool", "run the self-tests at boot"),
    ("DISK_MB",  "int",  "size of the filesystem image, in MiB"),
    ("KHEAP_MB", "int",  "kernel heap arena, in MiB"),
    ("QEMU_MEM", "str",  "memory given to QEMU by `make run`"),
    ("TIMEOUT",  "int",  "seconds `make log` waits before dumping the log"),
]

DEFAULTS = {
    "SMP": "y", "SELFTEST": "0", "DISK_MB": "2048", "KHEAP_MB": "8",
    "QEMU_MEM": "256M", "TIMEOUT": "12",
}
# Drivers are spelled CONFIG_<NAME> in config.mk, and just <NAME> on screen:
# the Makefile wants the prefix, a person reading a list of drivers does not.
def driver_key(name):
    return "CONFIG_" + name


for name, _ in DRIVERS:
    DEFAULTS[driver_key(name)] = "y"


def discover_apps():
    """Every program in app/, with the first line of its source as a
    description -- which is where each one already says what it is for."""
    apps = []
    appdir = os.path.join(ROOT, "app")

    for name in sorted(os.listdir(appdir)):
        src = os.path.join(appdir, name, "sourcecode")
        if not os.path.isdir(src):
            continue
        if not any(f.endswith(".c") for f in os.listdir(src)):
            continue

        summary = ""
        main = os.path.join(src, name + ".c")
        candidates = [main] if os.path.exists(main) else \
                     [os.path.join(src, f) for f in sorted(os.listdir(src))
                      if f.endswith(".c")]
        for path in candidates[:1]:
            try:
                with open(path, encoding="utf-8", errors="replace") as fh:
                    for line in fh:
                        line = line.strip()
                        if not line.startswith("/*"):
                            break
                        text = line.lstrip("/* ").strip()
                        # "name -- what it does" is the house style.
                        if "--" in text:
                            summary = text.split("--", 1)[1].strip()
                        else:
                            summary = text
                        break
            except OSError:
                pass
        apps.append((name, summary))
    return apps


# ---------------------------------------------------------------------------
# config.mk
# ---------------------------------------------------------------------------

def read_config():
    """Read the settings out of config.mk, leaving everything else alone."""
    values = dict(DEFAULTS)
    off = set()

    if os.path.exists(CONFIG):
        with open(CONFIG, encoding="utf-8") as fh:
            for line in fh:
                m = re.match(r"\s*([A-Za-z_][A-Za-z0-9_]*)\s*\??=\s*(.*?)\s*$",
                             line)
                if not m:
                    continue
                key, value = m.group(1), m.group(2)
                if key == "APPS_OFF":
                    off = set(value.split())
                elif key in values:
                    values[key] = value
    return values, off


def write_config(values, apps_off):
    """Rewrite config.mk, keeping every line that is not ours.

    A settings file people edit by hand deserves to survive a tool that reads
    it: comments, spacing and anything this program has never heard of are
    left exactly as they were."""
    lines = []
    if os.path.exists(CONFIG):
        with open(CONFIG, encoding="utf-8") as fh:
            lines = fh.read().splitlines()

    ours = set(values) | {"APPS_OFF"}
    seen = set()
    out = []

    for line in lines:
        m = re.match(r"\s*([A-Za-z_][A-Za-z0-9_]*)\s*\??=", line)
        key = m.group(1) if m else None

        if key in ours:
            seen.add(key)
            if key == "APPS_OFF":
                out.append("APPS_OFF ?= " + " ".join(sorted(apps_off)))
            else:
                out.append("%s ?= %s" % (key, values[key]))
        else:
            out.append(line)

    missing = [k for k in list(values) + ["APPS_OFF"] if k not in seen]
    if missing:
        out.append("")
        out.append("# Added by `make menuconfig`.")
        for key in missing:
            if key == "APPS_OFF":
                out.append("APPS_OFF ?= " + " ".join(sorted(apps_off)))
            else:
                out.append("%s ?= %s" % (key, values[key]))

    with open(CONFIG, "w", encoding="utf-8") as fh:
        fh.write("\n".join(out).rstrip() + "\n")


# ---------------------------------------------------------------------------
# The screen
# ---------------------------------------------------------------------------

TABS = ["Drivers", "Programs", "Advanced"]


class Menu:
    def __init__(self, stdscr):
        self.stdscr = stdscr
        self.values, self.apps_off = read_config()
        self.apps = discover_apps()
        self.tab = 0
        self.cursor = [0, 0, 0]
        self.top = [0, 0, 0]
        self.dirty = False
        self.message = ""

    # -- what the current tab is a list of --------------------------------

    def rows(self):
        if self.tab == 0:
            return [(n, d) for n, d in DRIVERS]
        if self.tab == 1:
            return self.apps
        return [(n, d) for n, _, d in ADVANCED]

    def is_on(self, name):
        if self.tab == 0:
            return self.values.get(driver_key(name), "y") == "y"
        if self.tab == 1:
            return name not in self.apps_off
        kind = dict((n, k) for n, k, _ in ADVANCED)[name]
        if kind == "bool":
            v = self.values.get(name, "y")
            return v not in ("n", "0", "")
        return None

    def toggle(self, name):
        if self.tab == 0:
            self.values[driver_key(name)] = "n" if self.is_on(name) else "y"
        elif self.tab == 1:
            if name in self.apps_off:
                self.apps_off.discard(name)
            else:
                self.apps_off.add(name)
        else:
            kind = dict((n, k) for n, k, _ in ADVANCED)[name]
            if kind != "bool":
                return False
            # SELFTEST is spelled 0/1 because the Makefile has always spelled
            # it that way; the rest are y/n.
            if name == "SELFTEST":
                self.values[name] = "0" if self.is_on(name) else "1"
            else:
                self.values[name] = "n" if self.is_on(name) else "y"
        self.dirty = True
        return True

    # -- drawing ----------------------------------------------------------

    def draw(self):
        s = self.stdscr
        s.erase()
        h, w = s.getmaxyx()

        title = " WOS build configuration "
        s.attron(curses.A_REVERSE)
        s.addstr(0, 0, title.ljust(w - 1)[:w - 1])
        s.attroff(curses.A_REVERSE)

        # Tabs
        x = 1
        for i, name in enumerate(TABS):
            label = " %s " % name
            if i == self.tab:
                s.attron(curses.A_BOLD | curses.A_UNDERLINE)
                s.addstr(1, x, label)
                s.attroff(curses.A_BOLD | curses.A_UNDERLINE)
            else:
                s.addstr(1, x, label, curses.A_DIM)
            x += len(label) + 2

        count = ""
        if self.tab == 1:
            on = len(self.apps) - len(self.apps_off)
            count = "%d of %d programs" % (on, len(self.apps))
        elif self.tab == 0:
            on = sum(1 for n, _ in DRIVERS if self.is_on(n))
            count = "%d of %d drivers" % (on, len(DRIVERS))
        if count and w > len(count) + 4:
            s.addstr(1, w - len(count) - 2, count, curses.A_DIM)

        rows = self.rows()
        body_top, body_h = 3, max(1, h - 6)
        cur, top = self.cursor[self.tab], self.top[self.tab]

        if cur < top:
            top = cur
        elif cur >= top + body_h:
            top = cur - body_h + 1
        self.top[self.tab] = top

        for i in range(body_h):
            idx = top + i
            if idx >= len(rows):
                break
            name, desc = rows[idx]
            state = self.is_on(name)

            if state is None:                      # a typed-in value
                mark = "    "
                value = self.values.get(name, "")
                text = "%-14s %-10s %s" % (name, value, desc)
            else:
                mark = "[x] " if state else "[ ] "
                text = "%-14s %s" % (name, desc)

            line = (mark + text)[:w - 2]
            attr = curses.A_REVERSE if idx == cur else curses.A_NORMAL
            if state is False:
                attr |= curses.A_DIM
            s.addstr(body_top + i, 1, line.ljust(w - 2), attr)

        help1 = ("space toggle   enter edit   tab/←→ section   "
                 "a all   n none   s save   q quit")
        s.addstr(h - 2, 1, help1[:w - 2], curses.A_DIM)

        if self.message:
            s.attron(curses.A_BOLD)
            s.addstr(h - 1, 1, self.message[:w - 2])
            s.attroff(curses.A_BOLD)
        elif self.dirty:
            s.addstr(h - 1, 1, "modified -- press s to save", curses.A_DIM)

        s.refresh()

    # -- editing a typed value --------------------------------------------

    def edit(self, name):
        kind = dict((n, k) for n, k, _ in ADVANCED).get(name)
        if self.tab != 2 or kind == "bool":
            return

        h, w = self.stdscr.getmaxyx()
        prompt = "%s = " % name
        self.stdscr.addstr(h - 1, 1, " " * (w - 2))
        self.stdscr.addstr(h - 1, 1, prompt)
        curses.echo()
        curses.curs_set(1)
        try:
            raw = self.stdscr.getstr(h - 1, 1 + len(prompt), 20)
            text = raw.decode("utf-8", "replace").strip()
        except Exception:
            text = ""
        curses.noecho()
        curses.curs_set(0)

        if not text:
            return
        if kind == "int" and not text.isdigit():
            self.message = "%s must be a number" % name
            return

        self.values[name] = text
        self.dirty = True

    # -- the loop ---------------------------------------------------------

    def run(self):
        curses.curs_set(0)
        while True:
            self.draw()
            try:
                key = self.stdscr.getch()
            except KeyboardInterrupt:
                key = ord("q")

            self.message = ""
            rows = self.rows()
            cur = self.cursor[self.tab]

            if key in (curses.KEY_DOWN, ord("j")):
                self.cursor[self.tab] = min(cur + 1, len(rows) - 1)
            elif key in (curses.KEY_UP, ord("k")):
                self.cursor[self.tab] = max(cur - 1, 0)
            elif key == curses.KEY_NPAGE:
                self.cursor[self.tab] = min(cur + 10, len(rows) - 1)
            elif key == curses.KEY_PPAGE:
                self.cursor[self.tab] = max(cur - 10, 0)
            elif key == curses.KEY_HOME:
                self.cursor[self.tab] = 0
            elif key == curses.KEY_END:
                self.cursor[self.tab] = len(rows) - 1
            elif key in (ord("\t"), curses.KEY_RIGHT):
                self.tab = (self.tab + 1) % len(TABS)
            elif key in (curses.KEY_BTAB, curses.KEY_LEFT):
                self.tab = (self.tab - 1) % len(TABS)
            elif key == ord(" "):
                if rows and not self.toggle(rows[cur][0]):
                    self.message = "%s takes a value -- press enter" % rows[cur][0]
            elif key in (curses.KEY_ENTER, 10, 13):
                if rows:
                    name = rows[cur][0]
                    if self.tab == 2 and \
                       dict((n, k) for n, k, _ in ADVANCED)[name] != "bool":
                        self.edit(name)
                    else:
                        self.toggle(name)
            elif key == ord("a"):
                if self.tab == 0:
                    for n, _ in DRIVERS:
                        self.values[driver_key(n)] = "y"
                    self.dirty = True
                elif self.tab == 1:
                    self.apps_off.clear()
                    self.dirty = True
            elif key == ord("n"):
                if self.tab == 0:
                    for n, _ in DRIVERS:
                        self.values[driver_key(n)] = "n"
                    self.dirty = True
                elif self.tab == 1:
                    self.apps_off = {n for n, _ in self.apps}
                    self.dirty = True
            elif key == ord("s"):
                write_config(self.values, self.apps_off)
                self.dirty = False
                self.message = "written to config.mk -- run make"
            elif key in (ord("q"), 27):
                if not self.dirty:
                    return
                self.message = "unsaved changes: s to save, q again to discard"
                self.draw()
                if self.stdscr.getch() in (ord("q"), 27):
                    return


def main():
    if not os.path.isdir(os.path.join(ROOT, "app")):
        sys.exit("run this from the WOS tree")
    try:
        curses.wrapper(lambda s: Menu(s).run())
    except curses.error as exc:
        sys.exit("the terminal is too small for the menu (%s)" % exc)


if __name__ == "__main__":
    main()
