"""Line-based connection to the interface's console, in machine mode.

Works the same against the real board (a USB serial port) and the simulator
(a child process), using only the standard library.
"""
import os
import select
import subprocess
import time


class LinkError(Exception):
    pass


class Link:
    END = "$"

    def __init__(self, read_fd, write_fd, proc=None):
        self._rfd, self._wfd, self._proc = read_fd, write_fd, proc
        self._buf = b""

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
        link._enter_machine_mode()
        return link

    @classmethod
    def simulator(cls, binary):
        """Start the host simulator (firmware/pico2w/sim/vw_diag_sim)."""
        proc = subprocess.Popen([binary], stdin=subprocess.PIPE, stdout=subprocess.PIPE, bufsize=0)
        link = cls(proc.stdout.fileno(), proc.stdin.fileno(), proc)
        link._enter_machine_mode()
        return link

    def close(self):
        if self._proc:
            self._proc.kill()
            self._proc.wait()
            self._proc.stdin.close()
            self._proc.stdout.close()
        else:
            try:
                self._write("mode h\n")
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
        data = text.encode("ascii")
        while data:
            try:
                n = os.write(self._wfd, data)
                data = data[n:]
            except BlockingIOError:
                select.select([], [self._wfd], [], 1.0)

    def _read_line(self, deadline):
        while b"\n" not in self._buf:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise LinkError("no reply from the interface (is it connected and running?)")
            ready, _, _ = select.select([self._rfd], [], [], remaining)
            if not ready:
                continue
            try:
                chunk = os.read(self._rfd, 4096)
            except BlockingIOError:
                continue
            if not chunk:
                raise LinkError("connection to the interface closed")
            self._buf += chunk
        raw, self._buf = self._buf.split(b"\n", 1)
        return raw.decode("ascii", "replace").strip("\r ")

    def _enter_machine_mode(self):
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
