#!/usr/bin/env python3
"""Flash, run and talk to a TinyQV design on a Tiny Tapeout demo board.

Command line equivalent of https://github.com/MichaelBell/tinytapeout-flasher
(branch tinyQV-sky25a).  The demo board's RP2350 runs MicroPython; this drives
its raw REPL over USB serial and uploads run_tinyqv.py, which does the real
work: bit-banging the QSPI Pmod flash, muxing in the design, clocking it and
bridging its UART to the console.

Needs nothing but Python 3.8+ -- no pyserial.

    ./tqv.py flash prism-test/prism_test.bin   # program, run, open console
    ./tqv.py run                               # run what is already in flash
    ./tqv.py console                           # re-attach to a running design
    ./tqv.py info                              # board and flash identification
    ./tqv.py reset                             # hand the board back to its REPL

In the console, Ctrl-Q q stops the design and returns the board to its REPL;
Ctrl-] detaches this tool and leaves the design running.
"""

import argparse
import struct
import zlib
import glob
import os
import re
import select
import sys
import termios
import time
import tty

# Defaults for TinyQV on TT Sky 25a ("Asteroids", design 495).  Other tapeouts:
# Sky 25a Berzerk 687, GF 0p2 39 (24MHz), IHP 25b 490, Sky 25b FemtoRV 514.
DEFAULT_DESIGN = 495
DEFAULT_LATENCY = 2
DEFAULT_FREQ_MHZ = 64

PAYLOAD = os.path.join(os.path.dirname(os.path.abspath(__file__)), "run_tinyqv.py")

SECTOR_SIZE = 4096
FLASH_SIZE = 16 * 1024 * 1024

CTRL_A, CTRL_B, CTRL_C, CTRL_D, CTRL_Q = b"\x01", b"\x02", b"\x03", b"\x04", b"\x11"
DETACH = 0x1D  # Ctrl-] quits this tool without disturbing the board

# Printed by the design's 'baud' command just before it switches its own
# UART divider: the cue for this side to reopen at the same rate.
BAUD_MARK_RE = re.compile(rb"\x05TQVBAUD:(\d+)\x05")

# A MicroPython traceback anywhere in a reply means the step failed; without
# this every failure would just look like a timeout.
TRACEBACK_RE = re.compile(rb"Traceback \(most recent call last\):.*", re.S)


class BoardError(Exception):
    pass


class Serial:
    """The subset of a serial port this needs, on a raw file descriptor."""

    def __init__(self, path):
        try:
            self.fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        except OSError as e:
            raise BoardError("cannot open %s: %s" % (path, e.strerror))
        self.path = path
        try:
            attrs = termios.tcgetattr(self.fd)
        except termios.error as e:
            os.close(self.fd)
            raise BoardError("%s is not a serial port: %s" % (path, e))
        self._saved = list(attrs)
        cc = list(attrs[6])
        cc[termios.VMIN] = 0
        cc[termios.VTIME] = 0
        # Fully raw: no translation, no echo, no flow control.  The board is USB
        # CDC so the line speed is ignored, but something has to be set.
        termios.tcsetattr(self.fd, termios.TCSANOW, [
            0, 0, termios.CS8 | termios.CREAD | termios.CLOCAL, 0,
            termios.B115200, termios.B115200, cc])

    def write(self, data, timeout=10.0):
        view = memoryview(data)
        deadline = time.monotonic() + timeout
        while view:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise BoardError("timed out writing to %s" % self.path)
            _, writable, _ = select.select([], [self.fd], [], remaining)
            if not writable:
                continue
            try:
                view = view[os.write(self.fd, view):]
            except BlockingIOError:
                pass

    def read(self, timeout):
        if timeout > 0:
            readable, _, _ = select.select([self.fd], [], [], timeout)
            if not readable:
                return b""
        try:
            return os.read(self.fd, 65536)
        except (BlockingIOError, OSError):
            return b""

    def close(self):
        try:
            termios.tcsetattr(self.fd, termios.TCSANOW, self._saved)
        except (termios.error, OSError):
            pass
        os.close(self.fd)


# Requests arrive wrapped in an APC string (ESC _ ... ESC \) so that a
# design probing a plain terminal - no tqv.py, e.g. the web console -
# leaves nothing on screen: terminals discard APC strings silently.
FS_MARK = b"\x1b_\x05TQVFS\x05"
FS_TERM_LEN = 2                 # the closing ESC backslash
FS_RESP = b"\x06"
FS_MAX_HANDLES = 8              # must match tqv_fs.h
FS_MAX_PAYLOAD = 4096           # sanity bound on a WRITE we will buffer


class HostFs:
    """Serves a directory to the design over the console link.

    The design frames requests as \\x05TQVFS\\x05<VERB> <args>\\n[payload]
    and we answer \\x06<status><4 hex len>\\n<payload>.  feed() is handed
    every byte the design says and returns the subset the terminal should
    see: while a request is being collected nothing is displayed, so the
    frames stay invisible to the user.

    Everything resolves inside root - a path escaping it (via .. or a
    leading /) is refused rather than clamped, so a design cannot reach
    the rest of the disk.
    """

    def __init__(self, root, port, verbose=False):
        self.root = os.path.realpath(root)
        self.port = port
        self.verbose = verbose
        self.handles = {}
        self.reset()

    def reset(self):
        self.state = "idle"     # idle -> req -> [payload] -> term -> idle
        self.pending = b""      # request line (or payload) being collected
        self.hold = b""         # possible partial marker held back
        self.want = 0           # payload bytes still to collect
        self.term = 0           # terminator bytes still to swallow
        self.args = None
        self.payload = b""

    def close_all(self):
        for f in self.handles.values():
            try:
                f.close()
            except OSError:
                pass
        self.handles = {}

    def log(self, msg):
        if self.verbose:
            sys.stderr.write("  fs: %s\r\n" % msg)

    # -- path safety -------------------------------------------------
    def _resolve(self, rel):
        rel = rel.strip()
        if rel in ("", "."):
            return self.root
        p = os.path.realpath(os.path.join(self.root, rel.lstrip("/")))
        if p != self.root and not p.startswith(self.root + os.sep):
            raise ValueError("path outside the served root")
        return p

    # -- the byte stream ---------------------------------------------
    def feed(self, data):
        """Consume design output; return what the terminal should show."""
        out = bytearray()
        data = self.hold + data
        self.hold = b""

        while data:
            if self.state == "idle":
                idx = data.find(FS_MARK)
                if idx < 0:
                    # Hold back a trailing partial marker so a frame split
                    # across two reads is still recognised
                    keep = 0
                    for n in range(1, len(FS_MARK)):
                        if data.endswith(FS_MARK[:n]):
                            keep = n
                    if keep:
                        self.hold = data[-keep:]
                        data = data[:-keep]
                    out += data
                    break
                out += data[:idx]
                data = data[idx + len(FS_MARK):]
                self.state = "req"
                continue

            if self.state == "req":
                idx = data.find(b"\n")
                if idx < 0:
                    self.pending += data
                    break
                line = (self.pending + data[:idx]).decode("utf-8", "replace")
                data = data[idx + 1:]
                self.pending = b""
                self.args = line.split()
                self.payload = b""
                nbytes = self._payload_bytes(self.args)
                if nbytes > 0:
                    self.state = "payload"
                    self.want = nbytes
                else:
                    self.state = "term"
                    self.term = FS_TERM_LEN
                continue

            if self.state == "payload":
                take = min(self.want, len(data))
                self.pending += data[:take]
                data = data[take:]
                self.want -= take
                if self.want == 0:
                    self.payload, self.pending = self.pending, b""
                    self.state = "term"
                    self.term = FS_TERM_LEN
                continue

            # swallow the APC terminator, then answer
            take = min(self.term, len(data))
            data = data[take:]
            self.term -= take
            if self.term == 0:
                args, self.args = self.args, None
                payload, self.payload = self.payload, b""
                self.state = "idle"
                self._serve(args, payload)

        return bytes(out)

    def busy(self):
        """True while a frame is half-collected (keyboard stays parked)."""
        return self.state != "idle"

    @staticmethod
    def _payload_bytes(args):
        if len(args) >= 3 and args[0] == "WRITE":
            try:
                return max(0, min(int(args[2]), FS_MAX_PAYLOAD))
            except ValueError:
                return 0
        return 0

    # -- request handling --------------------------------------------
    @staticmethod
    def _escape(payload):
        """Protect bytes the RP2350 bridge eats on the way to the design.

        run_tinyqv.py watches everything it forwards for Ctrl-Q (0x11):
        it swallows every one, and 0x11 followed by 'q' STOPS the design.
        Binary files are full of 0x11, so reading one used to corrupt the
        data and could kill the run outright.  Escape it (and 0x13, in
        case anything downstream ever does XON/XOFF) as 0x1a followed by
        the byte plus 0x20, which lands on printable characters.
        """
        if not any(b in (0x11, 0x13, 0x1a) for b in payload):
            return payload                  # the common case, untouched
        out = bytearray()
        for b in payload:
            if b in (0x11, 0x13, 0x1a):
                out += bytes((0x1a, b + 0x20))
            else:
                out.append(b)
        return bytes(out)

    def _reply(self, ok, payload=b""):
        if isinstance(payload, str):
            payload = payload.encode("utf-8", "replace")
        # The length is the DECODED byte count: the design unescapes as
        # it reads and stops when it has that many.
        header = b"%s%s%04x\n" % (FS_RESP, b"+" if ok else b"-", len(payload))
        self.port.write(header + self._escape(payload))

    def _file(self, args, idx=1):
        f = self.handles.get(int(args[idx]))
        if f is None:
            raise KeyError("bad handle")
        return f

    def _serve(self, args, payload):
        if not args:
            return
        verb = args[0]
        try:
            if verb == "PING":
                self._reply(True, "tqvfs 1 %s" % os.path.basename(self.root))
            elif verb == "LIST":
                self._reply(True, self._list(args[1] if len(args) > 1 else "."))
            elif verb == "OPEN":
                self._reply(True, "%d" % self._open(args[1],
                                                    " ".join(args[2:])))
            elif verb == "CLOSE":
                h = int(args[1])
                self._file(args).close()
                del self.handles[h]
                self._reply(True)
            elif verb == "READ":
                self._reply(True, self._file(args).read(int(args[2])))
            elif verb == "WRITE":
                self.log("WRITE %d bytes %r" % (len(payload), payload[:72]))
                self._reply(True, "%d" % self._file(args).write(payload))
            elif verb == "SEEK":
                f = self._file(args)
                f.seek(int(args[2]), int(args[3]))
                self._reply(True, "%d" % f.tell())
            else:
                self._reply(False, "unknown verb %s" % verb)
        except (OSError, ValueError, KeyError, IndexError) as e:
            self.log("%s -> %s" % (" ".join(args), e))
            self._reply(False, str(e))

    def _list(self, rel):
        path = self._resolve(rel)
        rows = []
        for name in sorted(os.listdir(path)):
            full = os.path.join(path, name)
            if os.path.isdir(full):
                rows.append("d 0 %s" % name)
            else:
                try:
                    rows.append("f %d %s" % (os.path.getsize(full), name))
                except OSError:
                    continue
        return "".join(r + "\n" for r in rows)

    def _open(self, mode, rel):
        if len(self.handles) >= FS_MAX_HANDLES:
            raise OSError("too many open files")
        mode = mode.replace("b", "") + "b"      # always binary on this side
        if mode not in ("rb", "wb", "ab", "r+b", "w+b", "a+b"):
            raise ValueError("bad mode")
        path = self._resolve(rel)
        if mode[0] in "wa":
            parent = os.path.dirname(path)
            if parent and not os.path.isdir(parent):
                os.makedirs(parent, exist_ok=True)
        f = open(path, mode)
        for h in range(FS_MAX_HANDLES):
            if h not in self.handles:
                self.handles[h] = f
                self.log("%s %s -> handle %d" % (mode, rel, h))
                return h
        raise OSError("no free handle")


class Board:
    def __init__(self, port, verbose=False):
        self.baud = 115200          # design link rate; 'baud' can raise it
        self.target = None          # (design, latency, freq_hz) once started
        self.port = port
        self.buf = b""
        self.consumed = b""
        self.verbose = verbose
        self.sdk_version = None
        self.micropython = None
        self.fs = None

    def log(self, msg):
        if self.verbose:
            sys.stderr.write("  %s\n" % msg)

    def drain(self, seconds=0.3):
        """Throw away whatever the board has said so far."""
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            self.port.read(0.05)
        self.buf = b""

    def expect(self, pattern, timeout=10.0, what=None, errors=True):
        """Consume input up to and including the first match of `pattern`.

        Everything consumed is kept in `self.consumed` so callers can mine it
        for things that scrolled past, such as the boot banner.
        """
        rx = re.compile(pattern)
        deadline = time.monotonic() + timeout
        while True:
            m = rx.search(self.buf)
            if m:
                self.consumed = self.buf[:m.end()]
                self.buf = self.buf[m.end():]
                return m
            err = TRACEBACK_RE.search(self.buf) if errors else None
            if err:
                detail = err.group(0).replace(b"\x04", b"").strip().decode("utf-8", "replace")
                raise BoardError("the board reported an error:\n%s" % detail)
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise BoardError("timed out waiting for %s" % (what or pattern.decode()))
            self.buf += self.port.read(min(0.2, remaining))

    def exec_raw(self, command):
        """Run one statement in the raw REPL (which must already be active)."""
        self.port.write(command.encode() + CTRL_D)

    def enter_raw_repl(self):
        for attempt in range(4):
            self.port.write(CTRL_C + CTRL_C)
            time.sleep(0.05)
            self.port.write(CTRL_A)
            try:
                self.expect(rb"raw REPL; CTRL-B to exit", timeout=2.0, errors=False)
                return
            except BoardError:
                self.log("no raw REPL prompt yet (attempt %d)" % (attempt + 1))
        raise BoardError(
            "could not get a raw REPL prompt from %s.\n"
            "Is another program holding the port, or is the board wedged? "
            "Unplug and replug it, then retry." % self.port.path)

    def connect(self):
        """Reboot into a known state and upload the MicroPython payload."""
        try:
            with open(PAYLOAD, "rb") as f:
                payload = f.read()
        except OSError as e:
            raise BoardError("cannot read %s: %s" % (PAYLOAD, e.strerror))

        # Ctrl-Q q first, in case a previous session left the board inside
        # run_tinyqv's UART bridge, where Ctrl-C is disabled.  At an ordinary
        # REPL the same keys are just a stray `q`, so discard the NameError
        # complaint before anything starts looking for real errors.
        self.port.write(CTRL_Q + b"q\n")
        time.sleep(0.2)
        self.port.write(CTRL_C + CTRL_C + CTRL_B)
        self.drain()
        self.port.write(CTRL_D)
        self.expect(rb"MPY: soft reboot", timeout=10.0, what="the board to reboot",
                    errors=False)
        # The reboot runs the Tiny Tapeout SDK's main.py, which takes well over
        # a second.  Interrupting midway leaves the board half-initialised, so
        # wait for the prompt that means it has finished.
        self.expect(rb"\r\n>>> ", timeout=30.0, errors=False,
                    what="the Tiny Tapeout SDK to finish booting")
        boot = self.consumed
        m = re.search(rb"tt\.sdk_version=(\S+)", boot)
        self.sdk_version = m.group(1).decode() if m else None
        m = re.search(rb"MicroPython (\S+) on ([^;\r\n]+)", boot)
        self.micropython = "%s (%s)" % (m.group(1).decode(), m.group(2).decode()) if m else None
        self.log("TT SDK %s, MicroPython %s" % (self.sdk_version, self.micropython))
        self.enter_raw_repl()
        self.log("uploading %s (%d bytes)" % (os.path.basename(PAYLOAD), len(payload)))
        self.port.write(payload + CTRL_D)
        m = self.expect(rb"tt\.flash_id=([0-9a-fA-F]+)", timeout=30.0,
                        what="the QSPI Pmod flash to identify itself")
        return m.group(1).decode()

    def program(self, offset, data):
        """Erase, program and verify `data` at `offset`, reporting progress."""
        chunk_re = rb"flash_prog=(ok|[0-9A-Fa-f]+)"
        self.exec_raw("program_flash(%#x)" % offset)
        self.expect(chunk_re, timeout=15.0, what="the flash to be ready")

        total = len(data)
        start = time.monotonic()
        for i in range(0, total, SECTOR_SIZE):
            chunk = data[i:i + SECTOR_SIZE]
            self.port.write(b"%d\r\n" % len(chunk))
            self.port.write(chunk, timeout=30.0)
            m = self.expect(chunk_re, timeout=30.0, what="sector at %#x to program" % (offset + i))
            if m.group(1) != b"ok":
                written = int(m.group(1), 16) - offset
                self.progress(written, total, start)
        self.port.write(b"0\r\n")
        self.expect(rb"flash_prog=ok", timeout=30.0, what="programming to finish")
        self.progress(total, total, start)
        if sys.stderr.isatty():
            sys.stderr.write("\n")

    def progress(self, written, total, start):
        if not sys.stderr.isatty():
            return
        elapsed = time.monotonic() - start
        rate = written / elapsed / 1024 if elapsed > 0 else 0
        sys.stderr.write("\r  %6.1f/%.1f kB  (%3d%%, %.0f kB/s)" % (
            written / 1024, total / 1024, 100 * written // total, rate))
        sys.stderr.flush()

    def program_ram(self, bank, data):
        """Write and verify `data` into PSRAM bank 0 (A) or 1 (B) at
        offset 0, using the RP2350's PIO SPI - the fast path, no UART."""
        self.exec_raw(PSRAM_PAYLOAD)
        self.expect(rb">", timeout=5.0, errors=False,
                    what="the PSRAM helper to load")
        chunk_re = rb"ram_prog=(ok|[0-9A-Fa-f]+)"
        self.exec_raw("program_ram(%d)" % bank)
        m = self.expect(rb"ram_id=([0-9a-f]+)", timeout=5.0,
                        what="the PSRAM to identify")
        banner("PSRAM bank %s id %s" % ("AB"[bank], m.group(1).decode()))
        self.expect(chunk_re, timeout=5.0, what="the PSRAM to be ready")

        total = len(data)
        start = time.monotonic()
        for i in range(0, total, SECTOR_SIZE):
            chunk = data[i:i + SECTOR_SIZE]
            self.port.write(b"%d\r\n" % len(chunk))
            self.port.write(chunk, timeout=30.0)
            m = self.expect(chunk_re, timeout=30.0,
                            what="RAM at %#x to program" % i)
            if m.group(1) != b"ok":
                self.progress(int(m.group(1), 16), total, start)
        self.port.write(b"0\r\n")
        self.expect(rb"ram_prog=ok", timeout=30.0, what="programming to finish")
        self.progress(total, total, start)
        if sys.stderr.isatty():
            sys.stderr.write("\n")

    def run(self, design, latency, freq_hz):
        self.exec_raw("run(%d, %d, %d)" % (design, latency, freq_hz))
        self.expect(rb"design=%d\b" % design, timeout=15.0, what="the design to be selected")

    def ram_open(self, bank):
        self.exec_raw(PSRAM_PAYLOAD)
        time.sleep(0.2)
        self.drain()
        self.exec_raw("ram_io(%d)" % bank)
        self.expect(rb"ram_io=ready", timeout=15.0, what="the PSRAM to be ready")

    def ram_close(self):
        self.port.write(b"Q\n")
        self.expect(rb"ram_io=done", timeout=10.0, what="the PSRAM session to end")

    def ram_read(self, off, n):
        self.port.write(b"R %d %d\n" % (off, n))
        # terminator required: the hex arrives over time and a bare
        # [0-9a-f]+ match fires on the first partial line
        m = self.expect(rb"D=([0-9a-f]+)\r?\n", timeout=15.0,
                        what="a PSRAM read")
        return bytes.fromhex(m.group(1).decode())

    def ram_write(self, off, data):
        for i in range(0, len(data), 1024):
            chunk = data[i:i + 1024]
            self.port.write(b"W %d %d\n" % (off + i, len(chunk)))
            self.port.write(chunk)
            self.expect(rb"ok", timeout=15.0,
                        what="a PSRAM write at %#x" % (off + i))

    def flash_read(self, addr, n):
        self.exec_raw(PSRAM_PAYLOAD)
        time.sleep(0.2)
        self.drain()
        self.exec_raw("flash_read(%d, %d)" % (addr, n))
        m = self.expect(rb"D=([0-9a-f]+)\r?\n", timeout=20.0,
                        what="a flash read")
        return bytes.fromhex(m.group(1).decode())

    def boot_baud(self):
        """The design's reset-divider link rate at the target clock.

        The divider resets to 555 (115200 at 64MHz); at any other project
        clock the same divider yields a proportionally scaled rate, which
        run() already matches on this side when it builds the UARTs.
        """
        if self.target:
            return int(round(115200 * self.target[2] / 64_000_000))
        return 115200

    def rebridge(self, baud):
        """Re-open the host side of the link at `baud`.

        Called after the design has switched its own divider.  The bridge
        loop is stopped with its own Ctrl-Q q (which never reaches the
        design) and restarted from REBRIDGE_PAYLOAD; the clock and reset
        are untouched, so the design keeps running - and keeps whatever it
        had in RAM - across the change.
        """
        self.port.write(CTRL_Q + b"q")
        self.expect(rb"TinyQV stop", timeout=5.0,
                    what="the bridge to stop for the baud change")
        self.drain()
        self.exec_raw(REBRIDGE_PAYLOAD)
        time.sleep(0.2)
        self.drain()
        self.exec_raw("rebridge(%d)" % baud)
        self.expect(rb"TinyQV baud=%d\b" % baud, timeout=10.0,
                    what="the bridge to reopen at %d baud" % baud)
        self.baud = baud

    def leave_raw_repl(self):
        self.port.write(CTRL_B)

    def _console_send_song(self, interactive, stdin_fd, saved):
        # Called mid-console when the design prints the download marker.
        if not interactive:
            banner("design requested a song but stdin is not a terminal; "
                   "let it time out or use './tqv.py send <file>'")
            return
        termios.tcsetattr(stdin_fd, termios.TCSADRAIN, saved)
        try:
            sys.stderr.write("\n-- design requests a song download\n")
            sys.stderr.write("-- file to send (Enter to skip): ")
            sys.stderr.flush()
            path = input().strip()
            if path:
                path = os.path.expanduser(path)
                if os.path.exists(path):
                    self.send_song(path)
                else:
                    banner("no such file: %s (design will time out)" % path)
        except (EOFError, KeyboardInterrupt):
            banner("skipped; the design's download will time out")
        finally:
            tty.setraw(stdin_fd)

    def _console_fast_load(self, interactive, stdin_fd, saved):
        # Called mid-console when the design prints the fast-load marker
        # ('downloadf').  Stops the design, writes the song into RAM B over
        # SPI, restarts the design and lets the console pick it back up.
        if not interactive:
            banner("fast load requested but stdin is not a terminal; "
                   "use './tqv.py load <file>' instead")
            return
        termios.tcsetattr(stdin_fd, termios.TCSADRAIN, saved)
        try:
            sys.stderr.write("\n-- fast load: design will stop and restart\n")
            sys.stderr.write("-- file to load (Enter to cancel): ")
            sys.stderr.flush()
            path = input().strip()
            if not path:
                banner("cancelled; the design keeps running")
                return
            image, kname = build_song_image(os.path.expanduser(path),
                                            8 * 1024 * 1024)
            banner("%s song: %d bytes; stopping the design"
                   % (kname, len(image) - 16))
            self.connect()
            self.program_ram(1, image)
            design, latency, freq_hz = self.target
            banner("restarting design %d" % design)
            self.run(design, latency, freq_hz)
            banner("loaded; 'play' plays it")
        except (EOFError, KeyboardInterrupt):
            banner("cancelled; the design keeps running")
        except (BoardError, OSError) as e:
            banner("fast load failed: %s" % e)
            try:                    # best effort: bring the design back
                design, latency, freq_hz = self.target
                self.run(design, latency, freq_hz)
            except BoardError:
                banner("could not restart the design; run './tqv.py run'")
        finally:
            tty.setraw(stdin_fd)

    def _probe_baud(self, baud):
        """True if the design already talks at `baud` (clean prompt seen)."""
        if baud == self.baud:
            return False                 # no probe needed; ask normally
        try:
            self.rebridge(baud)
        except BoardError:
            return False
        self.port.write(b"\r")
        deadline = time.monotonic() + 0.8
        got = b""
        while time.monotonic() < deadline:
            d = self.port.read(0.1)
            if d:
                got += d
                if b"prism> " in got or b"> " in got[-8:]:
                    return True
        # Not there: return to the boot rate and let the command path run
        try:
            self.rebridge(self.boot_baud())
        except BoardError:
            pass
        self.drain()
        return False

    def _console_set_baud(self, baud, interactive, stdin_fd, saved):
        # Called mid-console when the design announces a baud change.  It
        # drains its transmitter and switches its divider on its own; all
        # that is left here is to reopen this side at the same rate.
        if interactive:
            termios.tcsetattr(stdin_fd, termios.TCSADRAIN, saved)
        try:
            time.sleep(0.2)         # let the design finish its own switch
            self.rebridge(baud)

            def prompt_at(rate):
                self.port.write(b"\r")
                deadline = time.monotonic() + 2.0
                got = b""
                while time.monotonic() < deadline:
                    d = self.port.read(0.1)
                    if d:
                        got += d
                        if b"> " in got[-8:]:
                            return True
                return False

            if prompt_at(baud):
                banner("link now at %d baud" % baud)
            else:
                # No prompt: the divider the design chose does not give
                # this rate - almost always because it believes a clock
                # ('clk N' / --freq N) it is not really running at.  If
                # it is actually at the 64MHz default, its rate is off
                # by exactly that ratio; meet it there, correct it, and
                # redo the switch (the fresh marker re-enters here).
                claimed = self.target[2] if self.target else 64_000_000
                actual = int(round(baud * 64_000_000 / claimed))
                healed = False
                if actual != baud:
                    self.rebridge(actual)
                    if prompt_at(actual):
                        banner("design is really at 64 MHz (answers at %d"
                               " baud, not %d) - correcting" % (actual, baud))
                        self.target = (self.target[0], self.target[1],
                                       64_000_000)
                        self.port.write(b"clk 64\r")
                        time.sleep(0.4)
                        self.port.write(b"baud %d\r" % baud)
                        healed = True
                if not healed:
                    banner("no prompt at %d baud after the switch; check"
                           " --freq against the design's real clock" % baud)
        except BoardError as e:
            banner("baud change failed: %s" % e)
            try:
                boot = self.boot_baud()
                self.rebridge(boot)
                banner("back at %d; the design may still be at %d -"
                       " './tqv.py run' resets it" % (boot, baud))
            except BoardError:
                banner("could not reopen the link; run './tqv.py run'")
        finally:
            if interactive:
                tty.setraw(stdin_fd)

    def send_song(self, path):
        """Stream a song file to a design waiting in its 'download' command.

        The design expects {u32 'TQVD', u32 kind, u32 length, u32 crc32} +
        payload, DLE stuffed (0x10/0x11/0x03 escaped) so the RP2350 UART
        bridge's Ctrl-Q watcher and the SDK's Ctrl-C interrupt character
        never see their trigger bytes.  Paced slightly under the 115200
        wire rate; the design's 64KB receive buffer does the rest.
        """
        with open(path, "rb") as f:
            blob = f.read()
        kind, kname = song_kind(blob)
        if kind is None:
            raise BoardError("%s is not a recognized song blob (expected "
                             "ADPCM or L4Z .bin from the tools)" % path)
        hdr = struct.pack("<IIII", SONG_MAGIC, kind, len(blob),
                          zlib.crc32(blob) & 0xFFFFFFFF)
        payload = song_stuff(hdr + blob)
        banner("sending %s song: %d bytes (%d on the wire, ~%ds)"
               % (kname, len(blob), len(payload), len(payload) // 11000 + 1))
        t0 = time.monotonic()
        sent = 0
        for i in range(0, len(payload), 512):
            chunk = payload[i:i + 512]
            self.port.write(chunk)
            sent += len(chunk)
            # echo the design's progress dots while pacing the stream
            data = self.port.read(0)
            if data:
                os.write(1, data)
            target = t0 + sent / 11000.0
            now = time.monotonic()
            if target > now:
                time.sleep(target - now)
            if i % (64 * 512) == 0:
                sys.stderr.write("\r-- %3d%% " % (100 * sent // len(payload)))
                sys.stderr.flush()
        sys.stderr.write("\r-- 100%\n")

    def console(self, settle=0.0, set_baud=None):
        """Bridge the terminal to the design's UART until detach or stop.

        `set_baud` asks the design to move the link to that rate as soon as
        the console is up; the switch itself is driven by the marker the
        design prints, so typing 'baud <rate>' by hand works the same way.

        `settle` waits that many seconds for the design's first output before
        forwarding any input.  Straight after a reset the core is still
        fetching its startup code over QSPI and its UART receiver is not up
        yet, so anything sent before then is simply lost -- which piped input,
        unlike a human typing, is fast enough to hit.
        """
        banner("Ctrl-Q q stops the design | Ctrl-] detaches and leaves it running")
        self.baud = self.boot_baud()    # what a fresh boot talks at
        if settle:
            deadline = time.monotonic() + settle
            quiet_until = None
            while time.monotonic() < deadline:
                data = self.port.read(0.05)
                if data:
                    # Serve here too: the design probes for a filesystem
                    # before its first prompt, which is while we are still
                    # settling.  Unserved, that probe would decide there
                    # is no host and the design would run fileless.
                    if self.fs is not None:
                        data = self.fs.feed(data)
                    self.buf += data
                    # Let the startup banner finish so we hand over at a prompt.
                    quiet_until = time.monotonic() + 0.15
                elif quiet_until and time.monotonic() > quiet_until:
                    break
        stdin_fd = sys.stdin.fileno()
        interactive = os.isatty(stdin_fd)
        saved = termios.tcgetattr(stdin_fd) if interactive else None
        if interactive:
            # Raw, so Ctrl-C and friends reach the design instead of this tool.
            tty.setraw(stdin_fd)
        # Anything already buffered is output from before the console attached.
        pending, self.buf = self.buf, b""
        try:
            if pending:
                os.write(1, pending)
            tail = pending[-32:]
            clk_confirmed = True
            if self.target and self.target[2] != 64_000_000:
                # Tell the firmware its real clock so playback and baud
                # math are right.  The design may still be booting (its
                # UART RX comes up last, and bytes sent before that are
                # lost), so poke for a prompt first - and if the bridge
                # was left at some other rate by a dead session, re-open
                # it at the likely rates until the design answers.
                def poke():
                    self.port.write(b"\r")
                    deadline = time.monotonic() + 2.5
                    got = b""
                    while time.monotonic() < deadline:
                        d = self.port.read(0.1)
                        if d:
                            if self.fs is not None:
                                d = self.fs.feed(d)
                            got += d
                            if b"> " in got[-8:]:
                                return True
                    return False

                mhz = int(round(self.target[2] / 1_000_000))
                alive = poke()
                if not alive:
                    for rate in (self.boot_baud(), 115200):
                        try:
                            self.rebridge(rate)
                        except BoardError:
                            continue
                        if poke():
                            alive = True
                            break
                clk_confirmed = False
                if alive:
                    self.port.write(b"clk %d\r" % mhz)
                    want = b"clock %d" % mhz
                    deadline = time.monotonic() + 2.0
                    got = b""
                    while want not in got and time.monotonic() < deadline:
                        d = self.port.read(0.1)
                        if d:
                            got += d
                    clk_confirmed = want in got
                if clk_confirmed:
                    banner("design clock set to %d MHz" % mhz)
                else:
                    banner("design did not confirm 'clk %d' - is it"
                           " really running at %d MHz?  Start it with:"
                           " %s run --freq %d" % (mhz, mhz, sys.argv[0], mhz))
            if set_baud and not clk_confirmed:
                # Switching baud with wrong clock math bricks the link
                # (the design picks a divider for a clock it is not at),
                # so refuse rather than strand the session.
                banner("skipping --baud %d until the clock is confirmed"
                       % set_baud)
                set_baud = None
            if set_baud and clk_confirmed and self.target and \
                    self.target[2] != 64_000_000:
                # The clock announce just proved the design answers at the
                # bridge's current rate - ask for the switch on that live
                # link (probing first could strand us at a guessed rate).
                self.port.write(b"baud %d\r" % set_baud)
            elif set_baud:
                # The design may already be at this rate (reattaching to a
                # live session that switched earlier): probe there first.
                # A design at the high rate would see the 115200-framed
                # command as line noise, and one at 115200 sees the high
                # rate the same way - only the matching rate answers with
                # a clean prompt.
                if self._probe_baud(set_baud):
                    banner("link already at %d baud" % set_baud)
                else:
                    self.port.write(b"\rbaud %d\r" % set_baud)
            watching = [stdin_fd, self.port.fd]
            while True:
                # While a filesystem frame is half-collected the keyboard
                # stays parked: a keystroke landing between the request
                # and our reply would be read as part of the transaction.
                # The terminal buffers what the user types meanwhile.
                if self.fs is not None and self.fs.busy():
                    poll = [self.port.fd]
                    if self.verbose:
                        sys.stderr.write("  fs: keyboard parked, state=%s "
                                         "want=%d term=%d\r\n"
                                         % (self.fs.state, self.fs.want,
                                            self.fs.term))
                else:
                    poll = watching
                readable, _, _ = select.select(poll, [], [], 0.2)
                if self.port.fd in readable:
                    data = self.port.read(0)
                    if data:
                        if self.fs is not None:
                            # Serves any complete request and returns only
                            # what the terminal should see
                            shown = self.fs.feed(data)
                        else:
                            shown = data
                        if shown:
                            os.write(1, shown)
                        # Keep a little history so the marker is still found
                        # when it straddles two reads.  Scan what the
                        # terminal sees, not the raw stream: a binary
                        # filesystem payload must never be mistaken for
                        # one of these markers.
                        window = tail + shown
                        if b"TinyQV stop" in window:
                            return True
                        if b"\x05TQVLD\x05" in window:
                            # 'downloadf': stop, SPI-load, restart, reattach
                            tail = b""
                            self._console_fast_load(interactive, stdin_fd,
                                                    saved)
                            continue
                        if b"\x05TQVRX\x05" in window:
                            # The design asked for a song (its 'download'
                            # command).  Prompt for a file and stream it.
                            tail = b""
                            self._console_send_song(interactive, stdin_fd,
                                                    saved)
                            continue
                        m = BAUD_MARK_RE.search(window)
                        if m:
                            # The design is switching its UART divider;
                            # follow it on this side.
                            tail = b""
                            self._console_set_baud(int(m.group(1)),
                                                   interactive, stdin_fd,
                                                   saved)
                            continue
                        tail = window[-32:]
                if stdin_fd in readable:
                    data = os.read(stdin_fd, 1024)
                    if not data:
                        # Piped input ran out.  Keep printing what the design
                        # says; the user can stop us with Ctrl-C.
                        if interactive:
                            return False
                        watching = [self.port.fd]
                        continue
                    if DETACH in data:
                        self.port.write(data[:data.index(DETACH)])
                        return False
                    self.port.write(data)
        finally:
            if interactive:
                termios.tcsetattr(stdin_fd, termios.TCSADRAIN, saved)
            sys.stdout.write("\r\n")
            sys.stdout.flush()


def banner(msg):
    sys.stderr.write("-- %s\n" % msg)


def attach_fs(board, args):
    """Give the console a directory to serve, unless --no-fs."""
    if getattr(args, "no_fs", False):
        banner("filesystem service disabled (--no-fs)")
        return
    root = getattr(args, "fs_root", None)
    if root is None:
        root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "tqvfs")
    root = os.path.expanduser(root)
    try:
        os.makedirs(root, exist_ok=True)
    except OSError as e:
        banner("filesystem off: cannot use %s (%s)" % (root, e.strerror))
        return
    board.fs = HostFs(root, board.port, verbose=args.verbose)
    banner("serving %s to the design ('fs' in the pwl> console checks)" % root)



# MicroPython snippet appended after run_tinyqv.py: re-open the bridge
# UARTs at a different baud rate WITHOUT touching the clock, reset or
# design selection, so the design keeps running across the change.
#
# run_tinyqv.py builds its UARTs from the design clock (115200 at 64MHz)
# and its bridge loop owns them for as long as the design runs, so a
# faster link cannot be had by asking it - this is the same loop, minus
# the reset, with the baud as an argument and deeper buffers to match.
REBRIDGE_PAYLOAD = r"""
def rebridge(baud):
    import sys, uselect, micropython, time, gc, machine

    # Retune the PL011s through their divisor registers: this build's
    # machine.UART leaves RX permanently dead after any .init() or second
    # construction (only the first object's IRQ plumbing works), so the
    # UART objects run() rooted in globals stay in charge and only the
    # baud divisor changes underneath them.  uartclk is 48MHz (clk_peri:
    # IBRD/FBRD read back 26+3/64 at 115200).
    for u in (0x40070000, 0x40078000):          # UART0, UART1
        div = 48000000 / (16.0 * baud)
        ibrd = int(div)
        fbrd = int((div - ibrd) * 64 + 0.5)
        while machine.mem32[u + 0x18] & 8:      # UARTFR.BUSY
            pass
        cr = machine.mem32[u + 0x30]
        machine.mem32[u + 0x30] = cr & ~1       # UARTEN off
        machine.mem32[u + 0x24] = ibrd
        machine.mem32[u + 0x28] = fbrd
        machine.mem32[u + 0x2c] = machine.mem32[u + 0x2c]   # latch brds
        machine.mem32[u + 0x30] = cr

    uart_out, uart_in = _tqv_uart_out, _tqv_uart_in
    time.sleep(0.05)
    uart_in.read()                      # drop resync junk from the switch
    gc.collect()                        # start the loop with a clean heap
    print("TinyQV baud=%d" % baud)

    try:
        micropython.kbd_intr(-1)
        poll = uselect.poll()
        poll.register(sys.stdin, uselect.POLLIN)
        CTRL_Q = 17
        is_ctrl_q = False
        stop = False

        while not stop:
            if poll.poll(0):
                # One byte per read AND per write, fused into a single
                # pass: the design has a single byte receiver and a
                # per-byte RX interrupt costing ~7us on its fetch-bound
                # core, so bytes must not arrive back to back at 1Mbaud
                # (a batched write turned "cat prism.cfg" into "catism").
                # The interpreter overhead of this loop IS the pacing
                # (~60us/byte, ~15kB/s) - collecting into a batch first
                # and then writing byte-wise paid that overhead twice.
                n = 0
                while n < 256:
                    c = sys.stdin.buffer.read(1)
                    if not c:
                        break
                    if is_ctrl_q:
                        is_ctrl_q = False
                        if c[0] == ord('q') or c[0] == ord('Q'):
                            stop = True
                            break
                    elif c[0] == CTRL_Q:
                        is_ctrl_q = True
                        if not poll.poll(0):
                            break
                        continue
                    uart_out.write(c)
                    n += 1
                    if not poll.poll(0):
                        break
                if stop:
                    break

            uart_data = uart_in.read(512)
            while uart_data:
                sys.stdout.buffer.write(uart_data)
                uart_data = uart_in.read(512)
    finally:
        micropython.kbd_intr(3)
        print("TinyQV stop")
"""

# MicroPython snippet appended after run_tinyqv.py: direct PSRAM writes
# over the QSPI Pmod, for fast song loading into the design's RAM banks.
PSRAM_PAYLOAD = r"""
def _psram_qpi_exit(cs):
    # QPI commands are 4 bits wide: bit-bang 0xF5 (Exit QPI) on SD0..SD3.
    # Harmless if the chip is already in SPI mode (2 clocks of an
    # incomplete command are ignored at CS rise).
    sck = Pin(GPIO_UIO[3], Pin.OUT, value=0)
    sd = [Pin(GPIO_UIO[i], Pin.OUT, value=0) for i in (1, 2, 4, 5)]
    cs(0)
    for nib in (0xF, 0x5):
        for i in range(4):
            sd[i]((nib >> i) & 1)
        sck(1)
        sck(0)
    cs(1)
    for i in (2, 4, 5):
        Pin(GPIO_UIO[i], Pin.IN, Pin.PULL_DOWN)

class PSRAM:
    def __init__(self, bank):
        Pin(GPIO_UIO[0], Pin.OUT, value=1)
        Pin(GPIO_UIO[6], Pin.OUT, value=1)
        Pin(GPIO_UIO[7], Pin.OUT, value=1)
        self.cs = Pin(GPIO_UIO[6 + bank], Pin.OUT, value=1)
        _psram_qpi_exit(self.cs)
        self.spi = PIOSPI(2, Pin(GPIO_UIO[1]), Pin(GPIO_UIO[2]),
                          Pin(GPIO_UIO[3]), freq=10000000)
        for cmd in (b"\x66", b"\x99"):    # reset enable, reset
            self.cs(0)
            self.spi.write(cmd)
            self.cs(1)
        time.sleep_us(300)

    def read_id(self):
        self.cs(0)
        self.spi.write(b"\x9f\x00\x00\x00")
        rid = self.spi.read(2)
        self.cs(1)
        return rid

    @micropython.native
    def write(self, addr, data):
        # 1KB bursts: technically past the 8us tCEM refresh window, but
        # retention is tens of ms and every chunk is verified by readback
        mv = memoryview(data)
        o = 0
        while o < len(data):
            n = 1024 if len(data) - o > 1024 else len(data) - o
            self.cs(0)
            self.spi.write(b"\x02" + (addr + o).to_bytes(3, "big"))
            self.spi.write(mv[o:o + n])
            self.cs(1)
            o += n

    @micropython.native
    def read_into(self, addr, buf):
        mv = memoryview(buf)
        o = 0
        while o < len(buf):
            n = 1024 if len(buf) - o > 1024 else len(buf) - o
            self.cs(0)
            self.spi.write(b"\x03" + (addr + o).to_bytes(3, "big"))
            self.spi.readinto(mv[o:o + n])
            self.cs(1)
            o += n

def program_ram(bank):
    ram = PSRAM(bank)
    print("ram_id=" + binascii.hexlify(ram.read_id()).decode())
    addr = 0
    gc.collect()
    vbuf = bytearray(1)
    try:
        micropython.kbd_intr(-1)
        print("ram_prog=%X" % addr)
        while True:
            line = sys.stdin.buffer.readline()
            if not line:
                break
            n = int(line.strip())
            if n == 0:
                break
            data = sys.stdin.buffer.read(n)
            ram.write(addr, data)
            if n != len(vbuf):
                vbuf = bytearray(n)
            ram.read_into(addr, vbuf)
            if vbuf != data:
                raise RuntimeError("RAM verification failed at %X" % addr)
            addr += n
            print("ram_prog=%X" % addr)
    finally:
        micropython.kbd_intr(3)
    ram.spi._sm.active(0)
    print("ram_prog=ok")


def ram_io(bank):
    # Arbitrary-offset PSRAM access for the allocation-table tooling:
    #   R <off> <n>          -> "D=<hex>"
    #   W <off> <n> + n raw  -> verified, "ok"
    #   Q                    -> "ram_io=done"
    import binascii
    ram = PSRAM(bank)
    try:
        micropython.kbd_intr(-1)
        print("ram_io=ready")
        while True:
            line = sys.stdin.buffer.readline()
            if not line:
                break
            parts = line.split()
            if not parts or parts[0] == b"Q":
                break
            off = int(parts[1]); n = int(parts[2])
            if parts[0] == b"R":
                buf = bytearray(n)
                ram.read_into(off, buf)
                print("D=" + binascii.hexlify(buf).decode())
            elif parts[0] == b"W":
                data = sys.stdin.buffer.read(n)
                ram.write(off, data)
                buf = bytearray(n)
                ram.read_into(off, buf)
                if buf != data:
                    raise RuntimeError("verify failed at %X" % off)
                print("ok")
    finally:
        micropython.kbd_intr(3)
    ram.spi._sm.active(0)
    print("ram_io=done")


def flash_read(addr, n):
    import binascii
    flash = SPIFlash()
    buf = bytearray(n)
    flash.read_data_into(addr, buf)
    print("D=" + binascii.hexlify(buf).decode())
"""


SONG_MAGIC = 0x44565154          # 'TQVD'

# --------------------------------------------------------------------------
# Allocation tables: the top 1KB of RAM B and the top flash sector name
# descriptor-wrapped images so the design can play them without anyone
# remembering addresses.  Format (little endian, absolute addresses):
#   u32 'TQF1', u32 count, then count x { char name[24]; u32 addr; u32 len }
# The flash table owns its whole 4KB erase sector; entries there are 4KB
# aligned so rewriting or deleting never disturbs file data.
# --------------------------------------------------------------------------
TQFAT_MAGIC = 0x31465154         # 'TQF1'
TQFAT_MAX = 31
TQFAT_SIZE = 1024
RAM_BASE, RAM_FAT = 0x1800000, 0x1FFFC00
RAM_FAT_OFF = RAM_FAT - RAM_BASE
# Songs may use everything above the app-image guard (the same 2MB floor
# the raw --flash mode enforces).  The old 0x800000 floor dated from the
# era of the app carrying the 4.4MB DSM song compiled in (FULL_SONG=1
# made a 5.1MB image); with songs stored here instead, the app stays
# small - but a FULL_SONG build would grow into the low song region, so
# entries loaded below 0x600000 and FULL_SONG=1 are mutually exclusive.
FLASH_LO, FLASH_FAT = 0x200000, 0xFFF000


def fat_parse(blob):
    """-> list of (name, addr, len), or None if no valid table."""
    if len(blob) < 8:
        return None
    magic, count = struct.unpack_from("<II", blob, 0)
    if magic != TQFAT_MAGIC or count > TQFAT_MAX or len(blob) < 8 + count * 32:
        return None
    out = []
    for i in range(count):
        name, addr, ln = struct.unpack_from("<24sII", blob, 8 + i * 32)
        out.append((name.split(b"\0")[0].decode("ascii", "replace"), addr, ln))
    return out


def fat_build(entries):
    blob = struct.pack("<II", TQFAT_MAGIC, len(entries))
    for name, addr, ln in entries:
        blob += struct.pack("<24sII", name.encode()[:23], addr, ln)
    return blob + b"\xff" * (TQFAT_SIZE - len(blob))


def fat_alloc(entries, size, lo, hi, align):
    """First-fit gap for `size` bytes in [lo, hi), respecting `align`."""
    def up(a):
        return (a + align - 1) & ~(align - 1)
    spans = sorted((a, a + ln) for _, a, ln in entries)
    at = up(lo)
    for a, b in spans:
        if at + size <= a:
            return at
        if b > at:
            at = up(b)
    if at + size <= hi:
        return at
    return None


def fat_name(args, path):
    name = args.name or re.sub(r"[^A-Za-z0-9_.-]", "_",
                               os.path.splitext(os.path.basename(path))[0])
    if not name or len(name) > 23:
        raise BoardError("entry name must be 1..23 characters "
                         "(--name to override)")
    return name


def song_kind(blob):
    """Sniff a song blob: a sound pack starts with its own magic, ADPCM has
    block-samples 1024 at offset 8, an L4Z DSM stream has block-size 3072
    at offset 4."""
    if len(blob) >= 16 and blob[:4] == b"SPK1":
        return 3, "sound pack"
    if len(blob) >= 12 and struct.unpack_from("<I", blob, 8)[0] == 1024:
        return 1, "ADPCM"
    if len(blob) >= 8 and struct.unpack_from("<I", blob, 4)[0] == 3072:
        return 2, "L4Z DSM"
    return None, None


def build_song_image(path, limit):
    """Read a song blob and wrap it in the descriptor the design's 'play'
    command expects: {u32 'TQVD', u32 kind, u32 length, u32 crc32} + payload."""
    with open(path, "rb") as f:
        blob = f.read()
    kind, kname = song_kind(blob)
    if kind is None:
        raise BoardError("%s is not a recognized song blob (expected ADPCM "
                         "or L4Z .bin from the tools)" % path)
    if len(blob) + 16 > limit:
        raise BoardError("%s does not fit (%d bytes, limit %d)"
                         % (path, len(blob) + 16, limit))
    return (struct.pack("<IIII", SONG_MAGIC, kind, len(blob),
                        zlib.crc32(blob) & 0xFFFFFFFF) + blob, kname)


def song_stuff(data):
    out = bytearray()
    for b in data:
        if b == 0x10:
            out += b"\x10\x00"
        elif b == 0x11:
            out += b"\x10\x01"
        elif b == 0x03:
            out += b"\x10\x02"
        else:
            out.append(b)
    return bytes(out)


def find_port(explicit):
    if explicit:
        return explicit
    if os.environ.get("TQV_PORT"):
        return os.environ["TQV_PORT"]
    candidates = sorted(glob.glob("/dev/cu.usbmodem*") + glob.glob("/dev/ttyACM*"))
    if not candidates:
        raise BoardError("no board found; pass --port or set TQV_PORT")
    if len(candidates) > 1:
        raise BoardError("several possible boards (%s); pass --port"
                         % ", ".join(candidates))
    return candidates[0]


def main(argv=None):
    # These are accepted both before and after the subcommand.  The SUPPRESS
    # defaults matter: without them the subparser's copy would overwrite a
    # value given before the subcommand with its own default.
    common = argparse.ArgumentParser(add_help=False)
    common.add_argument("-p", "--port", default=argparse.SUPPRESS,
                        help="serial port (default: autodetect, or $TQV_PORT)")
    common.add_argument("--baud", type=int, default=argparse.SUPPRESS,
                        help="ask the design to move the console link to this "
                             "baud rate once it is up (the design resets to "
                             "115200); e.g. --baud 2000000")
    common.add_argument("-v", "--verbose", action="store_true", default=argparse.SUPPRESS,
                        help="show protocol progress")

    target = argparse.ArgumentParser(add_help=False)
    target.add_argument("--design", type=int, default=DEFAULT_DESIGN,
                        help="tt mux design number (default: %(default)s)")
    target.add_argument("--latency", type=int, default=DEFAULT_LATENCY,
                        help="QSPI read latency, 1-3 (default: %(default)s)")
    target.add_argument("--freq", type=float, default=DEFAULT_FREQ_MHZ,
                        metavar="MHZ", help="project clock in MHz (default: %(default)s)")
    target.add_argument("--no-console", action="store_true",
                        help="start the design but do not attach the console")
    target.add_argument("--fs-root", default=argparse.SUPPRESS, metavar="DIR",
                        help="directory served to the design as its "
                             "filesystem (default: tqvfs/ beside this tool)")
    target.add_argument("--no-fs", action="store_true",
                        help="do not serve a filesystem to the design")

    parser = argparse.ArgumentParser(
        prog="tqv.py", parents=[common],
        description="Flash, run and talk to a TinyQV design on a Tiny Tapeout demo board.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="In the console: Ctrl-Q q stops the design, Ctrl-] detaches this tool.")
    sub = parser.add_subparsers(dest="command", required=True)

    p_flash = sub.add_parser("flash", parents=[common, target],
                             help="program a .bin into the QSPI flash, then run it")
    p_flash.add_argument("file", help="binary to program (e.g. prism-test/prism_test.bin)")
    p_flash.add_argument("--offset", type=lambda s: int(s, 0), default=0,
                         help="flash offset, decimal or 0x hex (default: 0)")
    p_flash.add_argument("--no-run", action="store_true",
                         help="program only; do not start the design afterwards")

    sub.add_parser("run", parents=[common, target],
                   help="run whatever is already in flash")
    sub.add_parser("console", parents=[common, target],
                   help="attach to a design already running, without resetting it")
    p_load = sub.add_parser("load", parents=[common, target],
                            help="load a song .bin into the design's RAM "
                                 "via the RP2350 SPI (fast; restarts the "
                                 "design), then run it")
    p_load.add_argument("file", help="song blob (.bin from mp3_to_dsm.py "
                                     "--format adpcm or tools/l4z)")
    p_load.add_argument("--flash", type=lambda s: int(s, 0), default=None,
                        nargs="?", const=0, metavar="OFFSET",
                        help="write to flash instead of RAM: bare --flash is "
                             "table-managed (persistent, 'play <name>'); an "
                             "explicit OFFSET is the legacy raw mode")
    p_load.add_argument("--name", default=None,
                        help="allocation-table entry name (default: the "
                             "file's basename; replaces an existing entry "
                             "of the same name)")
    p_send = sub.add_parser("send", parents=[common],
                            help="download a song .bin into the running "
                                 "design's RAM (design must be at the "
                                 "prism> prompt)")
    p_send.add_argument("file", help="song blob (.bin from mp3_to_dsm.py "
                                     "--format adpcm or tools/l4z)")
    p_fat = sub.add_parser("fat", parents=[common, target],
                           help="list / manage the RAM B and flash "
                                "allocation tables")
    p_fat.add_argument("--delete", metavar="NAME", default=None,
                       help="remove NAME from the selected table")
    p_fat.add_argument("--format", action="store_true",
                       help="write a fresh empty table (files unreferenced, "
                            "space reusable)")
    p_fat.add_argument("--ram", action="store_true",
                       help="operate on the RAM B table")
    p_fat.add_argument("--flash", action="store_true",
                       help="operate on the flash table")
    sub.add_parser("info", parents=[common],
                   help="report the MicroPython build and QSPI flash JEDEC ID")
    sub.add_parser("reset", parents=[common],
                   help="stop the design and hand the board back to its REPL")

    args = parser.parse_args(argv)


    baud_req = getattr(args, "baud", None)

    if baud_req and not 7000 <= baud_req <= 3_000_000:

        parser.error("--baud %d is outside the link's range: "

                     "the RP2350 UART tops out at 3000000 "

                     "(48MHz/16) and the design is ISR-bound "

                     "above ~1-2M; 1000000 is the tested sweet "

                     "spot" % baud_req)
    args.port = getattr(args, "port", None)
    args.verbose = getattr(args, "verbose", False)

    ram_image = None
    if args.command == "load":
        if args.flash is not None and args.flash > 0:
            # legacy raw mode: explicit offset, no table involvement
            if args.flash < 0x200000 or args.flash >= FLASH_SIZE:
                raise BoardError("--flash offset must be in 0x200000..0xFFFFFF"
                                 " (below that risks the application image)")
            limit = FLASH_SIZE - args.flash
        else:
            limit = 8 * 1024 * 1024
        try:
            ram_image, kname = build_song_image(args.file, limit)
        except OSError as e:
            raise BoardError("cannot read %s: %s" % (args.file, e.strerror))
        banner("%s song: %d bytes" % (kname, len(ram_image) - 16))

    data = None
    if args.command == "flash":
        try:
            with open(args.file, "rb") as f:
                data = f.read()
        except OSError as e:
            raise BoardError("cannot read %s: %s" % (args.file, e.strerror))
        if not data:
            raise BoardError("%s is empty" % args.file)
        if args.offset + len(data) > FLASH_SIZE:
            raise BoardError("%s does not fit at offset %#x (flash is %d MB)"
                             % (args.file, args.offset, FLASH_SIZE // (1024 * 1024)))

    path = find_port(args.port)
    port = Serial(path)
    board = Board(port, verbose=args.verbose)

    if args.command == "fat":
        if (args.delete or args.format) and args.ram == args.flash:
            raise BoardError("pick a table: --ram or --flash")
        board.connect()
        try:
            def show(label, entries):
                if entries is None:
                    banner("%s: no allocation table" % label)
                    return
                banner("%s: %d of %d entries" % (label, len(entries), TQFAT_MAX))
                for name, addr, ln in entries:
                    sys.stderr.write("   %-23s %08x %7d KiB\n"
                                     % (name, addr, (ln + 1023) // 1024))
            # RAM side
            if not args.flash or args.ram or not (args.delete or args.format):
                board.ram_open(1)
                try:
                    entries = fat_parse(board.ram_read(RAM_FAT_OFF, TQFAT_SIZE))
                    if args.ram and args.format:
                        board.ram_write(RAM_FAT_OFF, fat_build([]))
                        banner("RAM B table formatted")
                    elif args.ram and args.delete:
                        if entries is None or all(e[0] != args.delete for e in entries):
                            raise BoardError("no RAM entry '%s'" % args.delete)
                        entries = [e for e in entries if e[0] != args.delete]
                        board.ram_write(RAM_FAT_OFF, fat_build(entries))
                        banner("deleted '%s' from RAM B" % args.delete)
                    else:
                        show("RAM B", entries)
                finally:
                    board.ram_close()
            # flash side
            if not args.ram or args.flash or not (args.delete or args.format):
                entries = fat_parse(board.flash_read(FLASH_FAT, TQFAT_SIZE))
                if args.flash and args.format:
                    board.program(FLASH_FAT, fat_build([]))
                    banner("flash table formatted")
                elif args.flash and args.delete:
                    if entries is None or all(e[0] != args.delete for e in entries):
                        raise BoardError("no flash entry '%s'" % args.delete)
                    entries = [e for e in entries if e[0] != args.delete]
                    board.program(FLASH_FAT, fat_build(entries))
                    banner("deleted '%s' from flash" % args.delete)
                else:
                    show("flash", entries)
        finally:
            design, latency, freq_hz = (args.design, args.latency,
                                        int(args.freq * 1_000_000))
            board.run(design, latency, freq_hz)
            banner("design restarted")
        return 0

    try:
        if args.command == "send":
            banner("attaching to %s" % path)
            board.drain()
            port.write(b"\r")
            board.expect(rb"prism> ", timeout=3.0, errors=False,
                         what="the prism> prompt (is the design running "
                              "the PRISM CLI?)")
            port.write(b"download\r")
            board.expect(rb"\x05TQVRX\x05", timeout=5.0, errors=False,
                         what="the design's download handshake")
            board.send_song(args.file)
            m = board.expect(rb"(OK - 'play' plays it|FAIL|timed out[^\r\n]*)",
                             timeout=15.0, errors=False,
                             what="the design's verdict")
            os.write(1, board.consumed)
            sys.stdout.write("\n")
            ok = b"OK" in m.group(0)
            banner("download %s" % ("succeeded - run 'play' in the console"
                                    if ok else "FAILED"))
            return 0 if ok else 1

        # `console` deliberately skips the reset so it can join a live session.
        if args.command == "console":
            board.target = (args.design, args.latency, int(args.freq * 1_000_000))
            banner("attaching to %s" % path)
            attach_fs(board, args)
            stopped = board.console(set_baud=getattr(args, "baud", None))
            if stopped:
                board.leave_raw_repl()
                banner("design stopped")
            else:
                banner("detached; the design is still running")
            return 0

        banner("connecting to %s" % path)
        flash_id = board.connect()
        banner("QSPI flash JEDEC ID %s" % flash_id)

        if args.command == "info":
            print("port          %s" % path)
            print("micropython   %s" % (board.micropython or "unknown"))
            print("tt sdk        %s" % (board.sdk_version or "unknown"))
            print("flash id      %s" % flash_id)
            board.leave_raw_repl()
            return 0

        if args.command == "reset":
            board.leave_raw_repl()
            banner("board is back at its MicroPython REPL")
            return 0

        if args.command == "flash":
            banner("programming %s (%d bytes) at %#x" % (args.file, len(data), args.offset))
            board.program(args.offset, data)
            banner("programmed and verified")
            if args.no_run:
                board.leave_raw_repl()
                return 0

        if args.command == "load":
            if args.flash is not None and args.flash > 0:
                # legacy raw offset (no table)
                banner("writing %s (%d bytes) to flash at %#x"
                       % (args.file, len(ram_image), args.flash))
                board.program(args.flash, ram_image)
                banner("written at %#x (raw mode - no FAT entry; managed 'load' names it for 'play')"
                       % args.flash)
            elif args.flash is not None:
                # flash, table-managed
                name = fat_name(args, args.file)
                blob = board.flash_read(FLASH_FAT, TQFAT_SIZE)
                entries = fat_parse(blob) or []
                entries = [e for e in entries if e[0] != name]
                size = (len(ram_image) + 4095) & ~4095
                addr = fat_alloc(entries, size, FLASH_LO, FLASH_FAT, 4096)
                if addr is None:
                    raise BoardError("no room in flash for %d bytes "
                                     "('fat --delete' frees entries)" % size)
                banner("flash '%s': %d bytes at %#x" % (name, len(ram_image), addr))
                board.program(addr, ram_image)
                entries.append((name, addr, len(ram_image)))
                board.program(FLASH_FAT, fat_build(entries))
                banner("written and verified; 'play %s' plays it" % name)
            else:
                # RAM B, table-managed
                name = fat_name(args, args.file)
                board.ram_open(1)
                try:
                    blob = board.ram_read(RAM_FAT_OFF, TQFAT_SIZE)
                    entries = fat_parse(blob) or []
                    entries = [e for e in entries if e[0] != name]
                    size = (len(ram_image) + 1023) & ~1023
                    addr = fat_alloc(entries, size, RAM_BASE, RAM_FAT, 1024)
                    if addr is None:
                        raise BoardError("no room in RAM B for %d bytes "
                                         "('fatdel' frees entries)" % size)
                    banner("RAM '%s': %d bytes at %#x" % (name, len(ram_image), addr))
                    board.ram_write(addr - RAM_BASE, ram_image)
                    entries.append((name, addr, len(ram_image)))
                    board.ram_write(RAM_FAT_OFF, fat_build(entries))
                finally:
                    board.ram_close()
                banner("loaded and verified; 'play %s' plays it" % name)

        freq_hz = int(args.freq * 1_000_000)
        board.target = (args.design, args.latency, freq_hz)
        banner("starting design %d at %g MHz (latency %d)"
               % (args.design, args.freq, args.latency))
        board.run(args.design, args.latency, freq_hz)

        if args.no_console:
            banner("design running; attach later with: %s console" % sys.argv[0])
            return 0

        attach_fs(board, args)
        stopped = board.console(settle=3.0,
                                set_baud=getattr(args, "baud", None))
        if stopped:
            board.leave_raw_repl()
            banner("design stopped")
        else:
            banner("detached; the design is still running")
        return 0
    finally:
        port.close()


if __name__ == "__main__":
    try:
        sys.exit(main())
    except BoardError as e:
        sys.stderr.write("error: %s\n" % e)
        sys.exit(1)
    except KeyboardInterrupt:
        sys.stderr.write("\ninterrupted\n")
        sys.exit(130)
