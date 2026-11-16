"""Line-based connection to the interface's console, in machine mode.

Works the same against the real board (a USB serial port) and the simulator
(a child process), using only the standard library.
"""
import os
import re
import select
import subprocess
import time

from .uds import InterfaceRefused, NegativeResponse, NoResponse, UdsError


class LinkError(Exception):
    pass




class Link:
    END = "$"

    def __init__(self, read_fd, write_fd, proc=None):
        self._rfd, self._wfd, self._proc = read_fd, write_fd, proc
        self._buf = b""
        self._timeout_ms = None   # the console's response timeout, once we have set it
        self.read_chunk = 4096    # tests lower this to mimic small BLE packets

    # ---- constructors
    @classmethod
    def serial(cls, path):
        """Open the board's USB serial port, e.g. /dev/cu.usbmodem1101."""
        import termios
        import tty
        fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        tty.setraw(fd)
        attrs = termios.tcgetattr(fd)
        attrs[2] |= termios.CLOCAL | termios.CREAD
        termios.tcsetattr(fd, termios.TCSANOW, attrs)
        link = cls(fd, fd)
        link._enter()
        return link

    @classmethod
    def simulator(cls, binary, read_chunk=4096):
        """Start the host simulator (hardware/pico2w/firmware/sim/vw_diag_sim)."""
        proc = subprocess.Popen([binary], stdin=subprocess.PIPE, stdout=subprocess.PIPE, bufsize=0)
        link = cls(proc.stdout.fileno(), proc.stdin.fileno(), proc)
        link.read_chunk = read_chunk
        link._enter()
        return link

    def close(self):
        if self._proc:
            self._proc.kill()
            self._proc.wait()
            for pipe in (self._proc.stdin, self._proc.stdout):
                try:
                    pipe.close()
                except OSError:
                    pass   # already gone
        else:
            try:
                self._leave()
            except OSError:
                pass
            os.close(self._rfd)
        self._rfd = self._wfd = None

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    # ---- I/O
    def _write(self, text):
        self._write_bytes(text.encode("ascii"))

    def _write_bytes(self, data):
        while data:
            try:
                n = os.write(self._wfd, data)
                data = data[n:]
            except BlockingIOError:
                select.select([], [self._wfd], [], 1.0)
            except OSError as e:
                # the interface went away: unplugged, or a dongle that switched itself off
                raise LinkError("connection to the interface closed (%s)" % e.strerror)

    def _fill(self, deadline):
        """Wait for more bytes from the interface and add them to the buffer."""
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise LinkError("no reply from the interface (is it connected and running?)")
            ready, _, _ = select.select([self._rfd], [], [], remaining)
            if not ready:
                continue
            try:
                chunk = os.read(self._rfd, self.read_chunk)
            except BlockingIOError:
                continue
            if not chunk:
                raise LinkError("connection to the interface closed")
            self._buf += chunk
            return

    def _read_line(self, deadline):
        while b"\n" not in self._buf:
            self._fill(deadline)
        raw, self._buf = self._buf.split(b"\n", 1)
        return raw.decode("ascii", "replace").strip("\r ")

    def _leave(self):
        self._write("mode h\n")

    def _enter(self):
        # Whatever state the console is in, this ends with a "$" line.
        self._write("\nmode m\n")
        deadline = time.monotonic() + 3.0
        while self._read_line(deadline) != self.END:
            pass

    def command(self, text, timeout=15.0):
        """Send one console command; return its output lines (without the end marker)."""
        self._write(text.strip() + "\n")
        deadline = time.monotonic() + timeout
        lines = []
        while True:
            line = self._read_line(deadline)
            if line == self.END:
                return lines
            if line:
                lines.append(line)

    def read_lines(self, seconds):
        """Collect unsolicited lines (monitor output) for a while."""
        deadline = time.monotonic() + seconds
        lines = []
        while True:
            try:
                line = self._read_line(deadline)
            except LinkError:
                return lines
            if line and line != self.END:
                lines.append(line)

    # ---- the interface, independent of how the console is spoken to
    def set_silent(self, on):
        """Listen-only on (the power-up state) or off."""
        self.command("silent on" if on else "silent off")

    def set_monitor(self, on):
        self.command("mon on" if on else "mon off")

    def set_keepalive(self, on):
        self.command("keepalive on" if on else "keepalive off")

    def status(self, timeout=15.0):
        text = " ".join(self.command("status", timeout=timeout))
        m = re.search(r"rx (\d+) dropped (\d+) tx (\d+) errors (\d+)", text)
        if not m:
            raise LinkError("could not read the interface status")
        rx, dropped, tx, errors = (int(v) for v in m.groups())
        return {"silent": "bus listen-only" in text, "monitor": "monitor on" in text,
                "keepalive": "keepalive on" in text, "rx": rx, "dropped": dropped, "tx": tx, "errors": errors}

    def uds_request(self, request_id, response_id, payload, timeout_ms=None):
        """Send one UDS request; return the positive response bytes or raise a uds error."""
        timeout_ms = timeout_ms or 1000
        if timeout_ms != self._timeout_ms:
            self.command("timeout %d" % timeout_ms)
            self._timeout_ms = timeout_ms
        hexs = " ".join("%02X" % b for b in payload)
        lines = self.command("x %X %X %s" % (request_id, response_id, hexs), timeout=timeout_ms / 1000.0 + 70.0)
        if not lines:
            raise UdsError("empty reply from the interface")
        reply = lines[-1]
        if reply.startswith("+"):
            return bytes(int(tok, 16) for tok in reply[1:].split())
        words = reply[1:].split()
        if words[:1] == ["nrc"]:
            raise NegativeResponse(int(words[1], 16))
        if words[:1] == ["timeout"]:
            raise NoResponse("no response from %X" % response_id)
        raise InterfaceRefused(reply[1:].strip())

    # ---- logging to the card
    def _log_reply(self, lines):
        m = re.search(r"log (on|off) (\S+) frames (\d+) bytes (\d+) lost (\d+)", " ".join(lines))
        if not m:
            raise LinkError(" ".join(lines).lstrip("- ") or "could not read the logging state")
        return {"active": m.group(1) == "on", "name": "" if m.group(2) == "-" else m.group(2),
                "frames": int(m.group(3)), "bytes": int(m.group(4)), "lost": int(m.group(5))}

    def set_time(self, unix_seconds):
        """The interface has no clock; tell it the time so log files can carry real timestamps."""
        self.command("time %d" % unix_seconds)

    def log_start(self):
        return self._log_reply(self.command("log on"))

    def log_stop(self):
        return self._log_reply(self.command("log off"))

    def log_status(self):
        return self._log_reply(self.command("log"))

    def log_mark(self, text):
        self._log_reply(self.command("log mark " + text))

    def poll_add(self, request_id, response_id, period_ms, payload):
        """Have the interface repeat a short request on a timer (so its answers land in the log)."""
        reply = self.command("poll %X %X %d %s" % (request_id, response_id, period_ms,
                                                   " ".join("%02X" % b for b in payload)))
        if not reply or reply[-1].startswith("-"):
            raise LinkError("the interface refused the poll request (too many, too long, or too fast)")

    def poll_clear(self):
        self.command("poll clear")

    def read_frames(self, seconds):
        """Collect monitored CAN frames for a while, as text lines like "3C0 [4] 00 00 03 00"."""
        return self.read_lines(seconds)

    def read_frames_timed(self, seconds):
        """Monitored frames with arrival times: [(seconds, id, extended, data)].

        The text console carries no timestamps, so these are when each line reached this
        computer: good to a few milliseconds over USB, not for timing studies.
        """
        out = []
        start = time.monotonic()
        deadline = start + seconds
        while True:
            try:
                line = self._read_line(deadline)
            except LinkError:
                return out
            words = line.split()
            if len(words) >= 2 and words[1].startswith("[") and line != self.END:
                try:
                    can_id = int(words[0], 16)
                    data = bytes(int(b, 16) for b in words[2:])
                except ValueError:
                    continue
                out.append((time.monotonic() - start, can_id, len(words[0]) > 3, data))

    def take_frames(self):
        """Frames monitored while requests were running (only the packet protocol keeps them)."""
        return []

    def power(self, op=None, value=0):
        """Power state of a board that sleeps (the dongle), from the console's  power  command."""
        cmd = {None: "power", 1: "power stay %s" % ("on" if value else "off"), 2: "power idle %d" % value,
               3: "power cutoff %d" % value, 4: "power sleep"}[op]
        lines = self.command(cmd)
        text = "\n".join(lines)
        if lines and lines[-1].startswith("-"):
            raise LinkError(lines[-1].lstrip("- "))
        m = re.search(r"woke by (\S+), battery (\d+) mV", text)
        held = re.search(r"held by: (.*)", text)
        s = re.search(r"idle (\d+) s, cutoff (\d+) mV, engine running above (\d+) mV, stay awake (on|off)", text)
        if not (m and held and s):
            raise LinkError("could not read the power state")
        sleeps = re.search(r"sleeps in (\d+) s", text)
        low = re.search(r"battery low for (\d+) s", text)
        return {"holds": [] if held.group(1) == "nothing" else held.group(1).split(), "wake": m.group(1),
                "battery_mv": int(m.group(2)), "sleeps_in_s": int(sleeps.group(1)) if sleeps else 0,
                "battery_low_for_s": int(low.group(1)) if low else 0, "idle_s": int(s.group(1)),
                "cutoff_mv": int(s.group(2)), "running_mv": int(s.group(3)), "stay_awake": s.group(4) == "on"}

    # ---- copying files off the card
    READ_CHUNK = 1024   # what one "log get" returns on the text console

    def log_files(self):
        """Files on the card: [(name, size)]."""
        lines = self.command("log ls")
        if lines and lines[-1].startswith("-"):
            raise LinkError(lines[-1].lstrip("- "))
        out = []
        for line in lines[:-1]:
            name, size = line.rsplit(" ", 1)
            out.append((name, int(size)))
        return out

    def log_read(self, name, offset, length):
        """Part of a file: (file size, bytes)."""
        lines = self.command("log get %s %d %d" % (name, offset, min(length, self.READ_CHUNK)))
        reply = lines[-1] if lines else "- no reply"
        if not reply.startswith("+"):
            raise LinkError(reply.lstrip("- "))
        words = reply[1:].split()
        return int(words[0]), bytes(int(b, 16) for b in words[1:])

    def fetch(self, name, progress=None):
        """A whole file from the card, as bytes. progress(done, total) is called after each piece."""
        data = bytearray()
        size = None
        while size is None or len(data) < size:
            size, piece = self.log_read(name, len(data), self.READ_CHUNK)
            if not piece:
                break
            data += piece
            if progress:
                progress(len(data), size)
        return bytes(data)
