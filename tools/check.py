#!/usr/bin/env python3
"""Boot WOS under QEMU and check that it works.

    make check                  every scenario
    make check ARGS=-l          list them
    make check ARGS="boot mv"   only these

Almost nothing interesting about an operating system is true until it runs.
The pointer's speed, a directory entry moved atomically, a socket that refuses
another user, a window drawn where the layout said -- none of that can be
checked by compiling, and all of it broke at least once while it was written.
So the checks here drive a real machine: QEMU with a real disk image, keys sent
through the monitor, and the answers read back from the serial log or from a
screenshot of the framebuffer.

Two ways to look at the machine, because it has two faces:

  * the *console*, whose output is mirrored to the serial port -- so a shell
    command's output is text this can search.  Cheap and exact.
  * the *screen*, through QEMU's screendump -- for the compositor, where the
    question is what ended up in which pixel.

Every scenario runs against the disk image in -snapshot mode: writes go to a
temporary overlay QEMU throws away, so a check that saves a configuration file
cannot leave the build tree different from how it found it -- and nothing has
to copy two gigabytes to arrange that.
"""

import argparse, json, os, re, shutil, socket, subprocess, sys, tempfile, time

HERE  = os.path.dirname(os.path.abspath(__file__))
ROOT  = os.path.dirname(HERE)
BUILD = os.path.join(ROOT, "build")

PASSWORD = "1234"                       # root's, on a freshly built image


# ------------------------------------------------------------------ #
#  Talking to QEMU
# ------------------------------------------------------------------ #

class Monitor:
    """QEMU's human monitor: keys, mouse movement and screenshots."""

    def __init__(self, path):
        self.sock = _connect(path)
        self.sock.settimeout(0.05)
        time.sleep(0.4)
        self._drain()

    def _drain(self):
        try:
            while self.sock.recv(65536):
                pass
        except (socket.timeout, BlockingIOError):
            pass

    def cmd(self, line, wait=0.2):
        self.sock.sendall((line + "\n").encode())
        time.sleep(wait)
        self._drain()

    # --- the keyboard ---

    KEYS = {
        " ": "spc", "\n": "ret", "\t": "tab", "-": "minus", "=": "equal",
        "_": "shift-minus", "+": "shift-equal", "/": "slash", ".": "dot",
        ",": "comma", ";": "semicolon", ":": "shift-semicolon",
        "'": "apostrophe", '"': "shift-apostrophe", "#": "shift-3",
        "$": "shift-4", "%": "shift-5", "*": "shift-8", "(": "shift-9",
        ")": "shift-0", "{": "shift-bracket_left", "}": "shift-bracket_right",
        "[": "bracket_left", "]": "bracket_right", "!": "shift-1",
        "?": "shift-slash", "~": "shift-grave_accent", "|": "shift-backslash",
    }

    def type(self, text, wait=0.05):
        for ch in text:
            if ch in self.KEYS:
                self.cmd("sendkey " + self.KEYS[ch], wait=wait)
            elif ch.isupper():
                self.cmd("sendkey shift-" + ch.lower(), wait=wait)
            else:
                self.cmd("sendkey " + ch, wait=wait)

    # --- the mouse ---

    def move_to(self, x, y, speed=100):
        """The pointer at an absolute position.

        In steps of at most 200 counts: a PS/2 packet carries nine signed bits
        per axis and QEMU holds the rest of a bigger movement back for a later
        event, so one large mouse_move does not arrive as one large movement.
        The corner is reachable at any pointer speed because the kernel clamps
        there, which is what makes this absolute at all."""
        def step(dx, dy):
            while dx or dy:
                sx = max(-200, min(200, dx))
                sy = max(-200, min(200, dy))
                self.cmd("mouse_move %d %d" % (sx, sy), wait=0.1)
                dx -= sx
                dy -= sy

        step(-900, -900)
        step(x * 100 // speed, y * 100 // speed)
        time.sleep(0.25)

    def click(self, x, y, speed=100, settle=0.7):
        self.move_to(x, y, speed)
        self.cmd("mouse_button 1", wait=0.15)
        self.cmd("mouse_button 0", wait=settle)

    def screen(self, name):
        """A screenshot, as (width, height, rgb bytes)."""
        path = os.path.join(self.work, name + ".ppm")

        if os.path.exists(path):
            os.unlink(path)
        self.cmd("screendump " + path, wait=0.8)

        for _ in range(40):
            if os.path.exists(path) and os.path.getsize(path) > 1000:
                return Image(path)
            time.sleep(0.1)

        raise Failure("no screendump appeared")


class Qmp:
    """QEMU's JSON channel, which is the only one with wheel events on it: the
    human monitor's mouse_button takes a bitmask of real buttons and has
    nowhere to put a wheel."""

    def __init__(self, path):
        self.sock = _connect(path)
        self.sock.settimeout(2)
        self.f = self.sock.makefile("rwb")
        self.f.readline()                       # the greeting
        self.command({"execute": "qmp_capabilities"})

    def command(self, obj):
        self.f.write((json.dumps(obj) + "\n").encode())
        self.f.flush()
        while True:
            line = self.f.readline()
            if not line:
                return None
            reply = json.loads(line)
            if "return" in reply or "error" in reply:
                return reply

    def key(self, name, down):
        """A key held down, or let go.  The human monitor's sendkey presses
        and releases in one go, which cannot express holding a modifier -- and
        a bar that appears while the modifier is held has to be caught while
        it is."""
        self.command({"execute": "input-send-event", "arguments": {
            "events": [{"type": "key", "data": {
                "down": down,
                "key": {"type": "qcode", "data": name}}}]}})
        time.sleep(0.4)

    def wheel(self, up, times=1):
        button = "wheel-up" if up else "wheel-down"

        for _ in range(times):
            for down in (True, False):
                self.command({"execute": "input-send-event", "arguments": {
                    "events": [{"type": "btn",
                                "data": {"down": down, "button": button}}]}})
            time.sleep(0.12)


def _connect(path):
    for _ in range(150):
        try:
            s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            s.connect(path)
            return s
        except OSError:
            time.sleep(0.1)
    raise Failure("QEMU never opened " + path)


class Image:
    """A screendump, and the few questions worth asking of one."""

    def __init__(self, path):
        with open(path, "rb") as f:
            data = f.read()

        if not data.startswith(b"P6"):
            raise Failure("screendump is not a P6 ppm")

        fields, at = [], 2
        while len(fields) < 3:
            while data[at:at + 1].isspace():
                at += 1
            start = at
            while not data[at:at + 1].isspace():
                at += 1
            fields.append(int(data[start:at]))
        at += 1

        self.w, self.h, _ = fields
        self.px = data[at:at + self.w * self.h * 3]

    def at(self, x, y):
        o = (y * self.w + x) * 3
        return "#%02x%02x%02x" % (self.px[o], self.px[o + 1], self.px[o + 2])

    def count(self, colour):
        want = bytes(int(colour[i:i + 2], 16) for i in (1, 3, 5))
        return sum(1 for i in range(self.w * self.h)
                   if self.px[i * 3:i * 3 + 3] == want)

    def blocks(self, colour, min_w=60, min_h=20, solid=True):
        """Filled rectangles of one colour: buttons, swatches, segments.

        Connected regions rather than rows of pixels, because a label is drawn
        through the middle of a button and no single row crosses it."""
        want = bytes(int(colour[i:i + 2], 16) for i in (1, 3, 5))
        w, h = self.w, self.h
        hit = bytearray(w * h)

        for i in range(w * h):
            if self.px[i * 3:i * 3 + 3] == want:
                hit[i] = 1

        seen = bytearray(w * h)
        out = []

        for start in range(w * h):
            if not hit[start] or seen[start]:
                continue

            stack, x0, x1, y0, y1, size = [start], w, 0, h, 0, 0
            seen[start] = 1

            while stack:
                i = stack.pop()
                size += 1
                x, y = i % w, i // w
                x0, x1 = min(x0, x), max(x1, x)
                y0, y1 = min(y0, y), max(y1, y)

                for j in (i - 1, i + 1, i - w, i + w):
                    if 0 <= j < w * h and hit[j] and not seen[j]:
                        if abs(j % w - x) > 1:      # no wrapping round a row
                            continue
                        seen[j] = 1
                        stack.append(j)

            bw, bh = x1 - x0 + 1, y1 - y0 + 1
            if bw < min_w or bh < min_h:
                continue
            if solid and size <= bw * bh * 6 // 10:
                continue                            # a ring, not a button

            out.append({"x": x0, "y": y0, "w": bw, "h": bh})

        return sorted(out, key=lambda r: (r["y"], r["x"]))

    def find_cursor(self, size=1, fill="#ffffff"):
        """Where the compositor's arrow is, at a given cursor size.

        Both halves of the shape are checked -- the black outline and the
        colour inside it.  Matching the outline alone finds the arrow on an
        empty desktop and then finds the top left corner of any black window,
        which reads as a cursor that never moves."""
        shape = [
            "X          ", "XX         ", "X.X        ", "X..X       ",
            "X...X      ", "X....X     ", "X.....X    ", "X......X   ",
            "X.......X  ", "X....XXXXX ", "X..X.X     ", "X.X  X.X   ",
            "XX   X.X   ", "X     X.X  ", "      XXX  ",
        ]
        black = "#000000"

        for y in range(self.h - len(shape) * size):
            for x in range(self.w - len(shape[0]) * size):
                if self.at(x, y) != black:
                    continue
                ok = True
                for row, line in enumerate(shape):
                    for col, want in enumerate(line):
                        if want == " ":
                            continue
                        got = self.at(x + col * size, y + row * size)
                        if want == "X" and got != black:
                            ok = False
                        elif want == "." and got != fill:
                            ok = False
                        if not ok:
                            break
                    if not ok:
                        break
                if ok:
                    return x, y
        return None


# ------------------------------------------------------------------ #
#  A machine
# ------------------------------------------------------------------ #

class Failure(Exception):
    pass


class Machine:
    """One booted WOS, on its own copy of the disk image."""

    def __init__(self, work, headless=True):
        self.work = work
        self.serial = os.path.join(work, "serial.log")
        mon_path    = os.path.join(work, "mon.sock")
        qmp_path    = os.path.join(work, "qmp.sock")

        kvm = ["-enable-kvm", "-cpu", "host"] if os.access("/dev/kvm", os.W_OK) \
              else []

        self.qemu = subprocess.Popen([
            "qemu-system-x86_64", *kvm, "-m", "256M",
            "-cdrom", os.path.join(BUILD, "wos.iso"),
            "-drive", "file=%s,format=raw,if=ide,index=0,media=disk"
                      % os.path.join(BUILD, "wos.img"),
            # Writes go to a temporary overlay and are dropped when QEMU exits:
            # the checks are free to save files, and the image they were run
            # against is the image that is still there afterwards.
            "-snapshot",
            "-netdev", "user,id=net0", "-device", "rtl8139,netdev=net0",
            "-vga", "std", "-boot", "d", "-no-reboot",
            "-display", "none",
            "-serial", "file:" + self.serial,
            "-monitor", "unix:%s,server,nowait" % mon_path,
            "-qmp", "unix:%s,server,nowait" % qmp_path,
        ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

        self.mon = Monitor(mon_path)
        self.mon.work = work
        self._qmp_path = qmp_path
        self._qmp = None
        self.pointer_speed = 100

    @property
    def qmp(self):
        if not self._qmp:
            self._qmp = Qmp(self._qmp_path)
        return self._qmp

    def kill(self):
        self.qemu.kill()
        self.qemu.wait()

    # --- reading what it says ---

    def log(self):
        try:
            with open(self.serial, "rb") as f:
                return f.read().decode("latin-1")
        except FileNotFoundError:
            return ""

    def wait_for(self, text, seconds=60, what=None):
        """Wait for something to appear in the serial log."""
        until = time.time() + seconds

        while time.time() < until:
            if text in self.log():
                return True
            time.sleep(0.2)

        raise Failure("waited %ds for %s" % (seconds, what or repr(text)))

    def mark(self):
        """Where the log is now, so a later search only sees what follows."""
        return len(self.log())

    def since(self, mark):
        return self.log()[mark:]

    # --- getting to a shell, and using it ---

    def login_console(self):
        """F2 at the login screen: a text session, whose output reaches the
        serial log.  Everything a command prints is then searchable text."""
        self.wait_for("Choose an account", 90, "the login screen")
        self.mon.cmd("sendkey f2", wait=1.5)
        self.mon.type(PASSWORD + "\n")
        self.wait_for("whell", 30, "a shell")
        time.sleep(1)

    def login_desktop(self):
        """Enter at the login screen: sway, on the framebuffer."""
        self.wait_for("Choose an account", 90, "the login screen")
        self.mon.type(PASSWORD + "\n")
        time.sleep(7)

    def run(self, command, settle=1.2):
        """Type a command at the console and return everything it printed."""
        mark = self.mark()
        self.mon.type(command + "\n")
        time.sleep(settle)
        return self.since(mark)


# ------------------------------------------------------------------ #
#  The scenarios
# ------------------------------------------------------------------ #

SCENARIOS = {}


def scenario(name, description):
    def register(fn):
        fn.description = description
        SCENARIOS[name] = fn
        return fn
    return register


def expect(condition, message):
    if not condition:
        raise Failure(message)


@scenario("boot", "the kernel comes up and finds its hardware")
def check_boot(m):
    m.wait_for("scheduler running", 90, "the scheduler")

    log = m.log()

    for line in ("paging : enabled", "wfs    : mounted from the disk",
                 "ramfs  : /ramdisk in memory", "proc   : scheduler running",
                 "syscall gate open"):
        expect(line in log, "the boot log never said %r" % line)

    expect("mouse  : PS/2 pointer" in log, "the mouse was not found")
    expect("PANIC" not in log and "triple" not in log,
           "something in the boot log panicked")
    m.wait_for("Choose an account", 60, "the login screen")


@scenario("mv", "rename moves a file, atomically and within one filesystem")
def check_mv(m):
    m.login_console()

    out = m.run("cd /ramdisk")
    out = m.run("touch first.txt")
    out = m.run("mv first.txt second.txt")
    expect("mv:" not in out, "mv complained: " + out)

    out = m.run("ls")
    expect("second.txt" in out and "first.txt" not in out,
           "the file did not change its name: " + out)

    # Replacing an existing file is what makes a save atomic.
    m.run("touch third.txt")
    out = m.run("mv second.txt third.txt")
    expect("mv:" not in out, "replacing a file failed: " + out)
    out = m.run("ls")
    expect("third.txt" in out and "second.txt" not in out,
           "the replacement left the wrong names: " + out)

    # Across two filesystems it refuses rather than copying.
    out = m.run("mv third.txt /home/root/moved.txt")
    expect("different filesystems" in out,
           "a cross-filesystem move was not refused: " + out)

    # A directory is not silently replaced.
    m.run("mkdir adir")
    m.run("touch afile")
    out = m.run("mv afile adir/inside.txt")
    expect("mv:" not in out, "moving into a directory failed: " + out)
    out = m.run("ls adir")
    expect("inside.txt" in out, "it did not arrive: " + out)


@scenario("sockets", "a socket belongs to the user who is listening on it")
def check_sockets(m):
    m.login_console()

    # The bare display server runs as root from boot, so its socket is root's.
    out = m.run("wlprobe wayland-1", settle=2)
    expect("connected" in out, "root could not reach its own socket: " + out)

    out = m.run("adduser tester", settle=1.5)
    m.mon.type("pw\n")
    time.sleep(1)
    m.mon.type("pw\n")
    time.sleep(1.5)

    out = m.run("su tester", settle=2)
    out = m.run("whoami")
    expect("tester" in out, "su did not become tester: " + out)

    out = m.run("wlprobe wayland-1", settle=2)
    expect("connected" not in out,
           "another user reached root's display socket: " + out)
    expect("not permitted" in out or "Operation not permitted" in out
           or "permission" in out.lower(),
           "the refusal did not say why: " + out)

    # And its own socket is its own: the same program, listening this time.
    out = m.run("exit", settle=1.5)


@scenario("desktop", "sway starts, and its settings can be changed and saved")
def check_desktop(m):
    m.login_desktop()

    shot = m.mon.screen("desktop")

    # Most of an empty workspace is the background colour.  A single pixel
    # would do until it landed on the key hints or on the cursor, which start
    # in the middle of the screen -- where the obvious pixel to sample is.
    background = shot.count("#101820")
    expect(background > shot.w * shot.h * 8 // 10,
           "an empty workspace is not sway's background colour: %d of %d "
           "pixels" % (background, shot.w * shot.h))

    # The settings window, from its binding.
    m.mon.cmd("sendkey meta_l-shift-s", wait=4)
    time.sleep(2)
    shot = m.mon.screen("settings")

    buttons = shot.blocks("#3584e4", min_w=70, min_h=20)
    expect(buttons, "swaysettings did not draw its Save button")
    save = buttons[-1]

    # A colour, by clicking the third swatch along.  The swatch row sits a
    # fixed distance below the first slider, which is what the layout says.
    sliders = shot.blocks("#3584e4", min_w=200, min_h=20, solid=False)
    expect(sliders, "no focus ring on the first slider")

    m.mon.click(60 + 2 * 30, sliders[0]["y"] + 72)
    time.sleep(1)

    m.mon.click(save["x"] + save["w"] // 2, save["y"] + save["h"] // 2)
    time.sleep(1.5)
    shot = m.mon.screen("saved")

    # The status line says where it went; the window is 8x16 text, so the
    # check is that the unsaved mark has gone rather than reading the words.
    expect(shot.count("#c01c28") < 40,
           "swaysettings still shows unsaved changes after Save")


@scenario("pointer", "the pointer moves at the speed the configuration asks")
def check_pointer(m):
    m.login_desktop()

    def where():
        shot = m.mon.screen("pointer")
        at = shot.find_cursor()
        expect(at, "the cursor is not on the screen")
        return at

    start = where()
    m.mon.cmd("mouse_move 100 0", wait=0.5)
    after = where()
    expect(abs((after[0] - start[0]) - 100) <= 1,
           "100 counts moved the pointer %d pixels, not 100"
           % (after[0] - start[0]))

    # Four times as fast, asked for the way a person would.
    m.mon.cmd("sendkey meta_l-ret", wait=3)
    m.mon.type("swaymsg input m pointer_accel 1\n")
    time.sleep(2)

    m.mon.cmd("mouse_move -200 0", wait=0.4)
    start = where()
    m.mon.cmd("mouse_move 25 0", wait=0.5)
    after = where()
    expect(abs((after[0] - start[0]) - 100) <= 2,
           "at 400%%, 25 counts moved the pointer %d pixels, not 100"
           % (after[0] - start[0]))

    # A quarter speed, and the remainder that has to survive between packets.
    m.mon.type("swaymsg input m pointer_accel -1\n")
    time.sleep(2)
    m.mon.cmd("mouse_move -400 0", wait=0.4)
    start = where()
    for _ in range(4):
        m.mon.cmd("mouse_move 1 0", wait=0.3)
    after = where()
    expect((after[0] - start[0]) == 1,
           "at 25%%, four single counts moved the pointer %d pixels, not 1"
           % (after[0] - start[0]))


@scenario("wheel", "the wheel turns the way the wheel was turned")
def check_wheel(m):
    m.login_desktop()

    m.mon.cmd("sendkey meta_l-shift-s", wait=4)
    time.sleep(2)

    before = m.mon.screen("wheel-before")
    rows = before.blocks("#3584e4", min_w=200, min_h=20, solid=False)
    expect(rows, "swaysettings did not draw a focused slider")

    # The first colour slider, whose groove is filled up to its knob: how far
    # along that fill reaches is the value, in pixels this can measure.
    groove = rows[0]["y"] + rows[0]["h"] // 2
    inside = range(rows[0]["x"] + 4, rows[0]["x"] + rows[0]["w"] - 4)

    def knob_at(shot):
        # Inside the focus ring, whose own right-hand edge is the same blue
        # and sits at the far end of every row it is drawn on.
        row = [x for x in inside if shot.at(x, groove) == "#3584e4"]
        expect(row, "the slider under the pointer has no filled groove")
        return max(row)

    was = knob_at(before)

    m.mon.move_to(300, groove)
    m.qmp.wheel(up=True, times=6)
    time.sleep(1)

    after = m.mon.screen("wheel-after")
    now = knob_at(after)

    # Up is more.  It was less until the sign of wl_pointer.axis was fixed --
    # the compositor negated what the mouse already reported, so every client
    # scrolled the wrong way.
    expect(now > was,
           "wheel-up moved the slider from %d to %d, the wrong way" % (was, now))

    m.qmp.wheel(up=False, times=6)
    time.sleep(1)
    back = knob_at(m.mon.screen("wheel-back"))

    expect(back < now,
           "wheel-down did not undo it: %d, then %d, then %d" % (was, now, back))


@scenario("bar", "the bar hides, comes back on the modifier, and takes a status")
def check_bar(m):
    m.login_desktop()

    def bar_pixels(name):
        """How much of the screen is the bar's own grey.

        Counted on an empty workspace, where nothing else is that colour: a
        window's title bar is the same grey when it is not focused, so a
        screen with windows on it cannot answer this question."""
        return m.mon.screen(name).count("#222222")

    docked = bar_pixels("bar-docked")
    expect(docked > 5000, "the bar is not drawn: %d pixels" % docked)

    # The commands come from a terminal on the first workspace; the counting
    # happens on the second, which is empty.
    m.mon.cmd("sendkey meta_l-ret", wait=3)
    m.mon.type("swaymsg bar status_text cpu ${CPU} mem ${MEM}\n")
    time.sleep(2)
    m.mon.type("swaymsg bar mode hide\n")
    time.sleep(2)

    m.mon.cmd("sendkey meta_l-2", wait=2)
    hidden = bar_pixels("bar-hidden")
    expect(hidden < docked // 10,
           "the bar is still there after `bar mode hide`: %d pixels" % hidden)

    # It comes back while the modifier is held down, and goes again after.
    m.qmp.key("meta_l", True)
    showing = bar_pixels("bar-holding")
    m.qmp.key("meta_l", False)

    expect(showing > docked // 2,
           "holding the modifier did not bring the hidden bar back: %d pixels"
           % showing)

    expect(bar_pixels("bar-released") < docked // 10,
           "the bar stayed after the modifier was let go")

    # Back to the windows, and put the bar where it was: a status somebody set
    # is on the bar, so the bar has to be there to show it.
    m.mon.cmd("sendkey meta_l-1", wait=2)
    m.mon.type("swaymsg bar mode dock\n")
    time.sleep(2)
    expect(bar_pixels("bar-again") > docked // 2,
           "`bar mode dock` did not bring it back")


@scenario("kill", "F9 in htop stops a process, and only one that is yours")
def check_kill(m):
    # A full-screen program on the console is still writing to the serial
    # port, so its screen is text this can read -- but it is drawn with cursor
    # positioning rather than as lines, so the moves have to become the line
    # breaks before anything can be found by row.
    def htop_table():
        """The process table as (pid, name), top row first."""
        screen = m.log()
        at     = screen.rfind("PID COMMAND")
        expect(at >= 0, "htop never drew its process table")

        text = re.sub(r"\x1b\[\d+;\d+H", "\n", screen[at:])
        text = re.sub(r"\x1b\[[0-9;?]*[a-zA-Z]", "", text)

        rows = []
        for line in text.split("\n")[1:]:
            row = re.match(r"\s+(\d+) (\S+)", line)
            if not row:
                break                     # the first blank line ends the table
            rows.append((int(row.group(1)), row.group(2)))

        expect(rows, "htop drew a table with nothing in it")
        return rows

    def htop_stop(name):
        """Start htop, put the selection on `name`, answer F9 with yes, and
        return what the footer said afterwards."""
        started = m.mark()

        m.mon.type("htop\n")
        time.sleep(2.5)

        wanted = [i for i, (_, n) in enumerate(htop_table()) if n == name]
        expect(wanted, "htop does not list %s" % name)

        m.mon.cmd("sendkey home", wait=0.4)
        for _ in range(wanted[0]):
            m.mon.cmd("sendkey down", wait=0.3)

        m.mon.cmd("sendkey f9", wait=1.0)
        expect("Stop" in m.log()[started:], "F9 asked nothing")

        answered = m.mark()
        m.mon.type("y")
        time.sleep(1.5)
        said = m.log()[answered:]

        m.mon.type("q")
        time.sleep(1.5)
        return said

    m.login_console()

    # waylandd is a service, so it is root's however it is reached.
    out = m.run("wlprobe wayland-1", settle=2)
    expect("connected" in out, "waylandd was not running to begin with: " + out)

    m.run("adduser tester", settle=1.5)
    m.mon.type("pw\n")
    time.sleep(1)
    m.mon.type("pw\n")
    time.sleep(1.5)
    m.run("su tester", settle=2)

    said = htop_stop("waylandd")
    expect("belongs to somebody else" in said,
           "another user's process was not refused: " + said)

    # Back to root, who can see whether the refusal actually protected it --
    # tester cannot reach root's socket whether waylandd is running or not, so
    # asking from there would answer nothing.
    m.run("exit", settle=1.5)

    out = m.run("wlprobe wayland-1", settle=2)
    expect("connected" in out,
           "waylandd stopped for a user who was refused: " + out)

    # Root's own, which root may stop.
    said = htop_stop("waylandd")
    expect("asked" in said and "to stop" in said,
           "root's kill did not go through: " + said)

    # It leaves the way an exit leaves: the socket it was listening on is gone.
    out = m.run("wlprobe wayland-1", settle=2)
    expect("connected" not in out, "waylandd is still serving: " + out)

    out = m.run("systemctl status wayland", settle=1.5)
    expect("stopped" in out.lower() or "not running" in out.lower(),
           "the service manager still calls it running: " + out)

    # And the row goes with it.  A service is the kernel's own child and
    # nothing in the kernel waits for one, so this is really asking whether the
    # idle loop reaped it: without that it would sit in the table having
    # exited, and F9 would look as though it had done nothing.
    m.mon.type("htop\n")
    time.sleep(2.5)
    left = [n for _, n in htop_table()]
    m.mon.type("q")
    time.sleep(1.5)

    expect("waylandd" not in left,
           "the row stayed after the process went: %r" % (left,))


@scenario("launcher", "Super+Q opens wauncher, and Enter starts what was typed")
def check_launcher(m):
    m.login_desktop()

    # The launcher's selected row: a bar of accent blue across the window,
    # which is also the thing that disappears when nothing matches.
    def selection(shot):
        return shot.blocks("#3584e4", min_w=200, min_h=15)

    # Two seconds is a long time and this has to be well inside it: the list is
    # one directory read, and the executables behind it are read after the
    # window is up rather than before.  Checking fifty of them first put the
    # whole of that on the near side of the first frame, which is the shape of
    # regression this catches.
    m.mon.cmd("sendkey meta_l-q", wait=0)
    time.sleep(2)
    shot = m.mon.screen("wauncher")

    expect(shot.count("#f6f5f4") > shot.w * shot.h // 4,
           "Super+Q did not put a window on the screen within two seconds")
    expect(selection(shot), "wauncher listed nothing")

    # And the tags arrive on their own, without anything being pressed.
    time.sleep(2.5)
    expect(m.mon.screen("tagged").count("#8a8e8f") > 200,
           "the rows never said whether they open a window or print")

    # A query nothing can match empties the list, which is the whole of the
    # filtering visible in one picture: no row, so no selection.
    m.mon.type("zzz")
    time.sleep(1.5)
    expect(not selection(m.mon.screen("nomatch")),
           "typing something no program is called still selected a program")

    # Escape clears the typing before it closes the window.
    m.mon.cmd("sendkey esc", wait=1.5)
    expect(selection(m.mon.screen("cleared")),
           "Escape closed the launcher instead of clearing what was typed")

    # A name only one program has, and Enter.
    m.mon.type("thunar")
    time.sleep(1.5)
    m.mon.cmd("sendkey ret", wait=6)
    time.sleep(3)
    shot = m.mon.screen("launched")

    # thunar's folder icons, which the launcher has none of.
    expect(shot.count("#6da6ee") > 50,
           "Enter did not start thunar")

    # And the launcher is gone: thunar has the whole screen, which is the only
    # width its sidebar is drawn at.  A launcher that stayed would leave it
    # half as wide and the sidebar unrendered.
    expect(shot.blocks("#ebe8e6", min_w=60, min_h=100),
           "the launcher was still holding half the screen afterwards")


# ------------------------------------------------------------------ #
#  Running them
# ------------------------------------------------------------------ #

def run_one(name, keep):
    fn = SCENARIOS[name]
    work = tempfile.mkdtemp(prefix="woscheck-%s-" % name)
    started = time.time()
    machine = None

    try:
        machine = Machine(work)
        fn(machine)
        print("  ok    %-9s %-56s %4.0fs" % (name, fn.description,
                                             time.time() - started))
        return True
    except Failure as e:
        print("  FAIL  %-9s %s" % (name, e))
        print("        the machine's files are in %s" % work)
        keep.append(work)
        return False
    except Exception as e:                       # a bug in the check itself
        print("  ERROR %-9s %s: %s" % (name, type(e).__name__, e))
        keep.append(work)
        return False
    finally:
        if machine:
            machine.kill()
        if work not in keep:
            shutil.rmtree(work, ignore_errors=True)


def main():
    ap = argparse.ArgumentParser(description="check a built WOS by running it")
    ap.add_argument("only", nargs="*", help="scenarios to run (default: all)")
    ap.add_argument("-l", "--list", action="store_true",
                    help="list the scenarios and what they check")
    args = ap.parse_args()

    if args.list:
        for name, fn in SCENARIOS.items():
            print("  %-9s %s" % (name, fn.description))
        return 0

    for name in args.only:
        if name not in SCENARIOS:
            print("check: no scenario called %s (try -l)" % name)
            return 2

    for f in ("wos.iso", "wos.img"):
        if not os.path.exists(os.path.join(BUILD, f)):
            print("check: %s is not built; run make first" % f)
            return 2

    names = args.only or list(SCENARIOS)
    keep  = []
    bad   = 0

    print("checking a built WOS by booting it (%d scenario%s)"
          % (len(names), "" if len(names) == 1 else "s"))

    for name in names:
        if not run_one(name, keep):
            bad += 1

    print("%d of %d scenarios passed" % (len(names) - bad, len(names)))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
