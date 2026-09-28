#!/usr/bin/env python3
"""vnc.py - a minimal RFB client for driving the test VM.

VMware's own captureScreen and typeKeystrokesInGuest both require VMware Tools
inside the guest, which KestrelOS does not have.  Its VNC server needs nothing
in the guest at all, so this speaks just enough RFB 3.8 to grab the framebuffer
and to send keystrokes.

    python vnc.py shot out.png
    python vnc.py type "ls -l" --enter
    python vnc.py key Down Down Enter
    python vnc.py wait "kestrel:/" --timeout 30
"""
import argparse
import socket
import struct
import sys
import time
import zlib

DEFAULT_HOST = "127.0.0.1"
DEFAULT_PORT = 5940

# X11 keysyms for the keys a test needs to press.
KEYSYMS = {
    "enter": 0xFF0D, "return": 0xFF0D, "esc": 0xFF1B, "escape": 0xFF1B,
    "tab": 0xFF09, "backspace": 0xFF08, "delete": 0xFFFF,
    "up": 0xFF52, "down": 0xFF54, "left": 0xFF51, "right": 0xFF53,
    "home": 0xFF50, "end": 0xFF57, "pageup": 0xFF55, "pagedown": 0xFF56,
    "space": 0x0020, "shift": 0xFFE1, "ctrl": 0xFFE3, "alt": 0xFFE9,
    "f1": 0xFFBE, "f2": 0xFFBF, "f3": 0xFFC0, "f4": 0xFFC1,
    "f5": 0xFFC2, "f6": 0xFFC3, "f7": 0xFFC4, "f8": 0xFFC5,
    "f9": 0xFFC6, "f10": 0xFFC7, "f11": 0xFFC8, "f12": 0xFFC9,
}

# Characters that need Shift held on a US layout.
SHIFTED = set('~!@#$%^&*()_+{}|:"<>?')


class Vnc:
    def __init__(self, host=DEFAULT_HOST, port=DEFAULT_PORT, timeout=15):
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.sock.settimeout(timeout)
        self._x = None
        self._y = None
        self._buttons = 0
        self._handshake()

    # ------------------------------------------------------------- plumbing

    def _recv(self, n):
        out = bytearray()
        while len(out) < n:
            chunk = self.sock.recv(n - len(out))
            if not chunk:
                raise ConnectionError("the VNC server closed the connection")
            out += chunk
        return bytes(out)

    def _handshake(self):
        version = self._recv(12)
        if not version.startswith(b"RFB "):
            raise ConnectionError("not an RFB server: %r" % version)
        self.sock.sendall(b"RFB 003.008\n")

        count = self._recv(1)[0]
        if count == 0:
            reason_len = struct.unpack(">I", self._recv(4))[0]
            raise ConnectionError(self._recv(reason_len).decode("utf-8", "replace"))
        types = self._recv(count)
        if 1 not in types:
            raise ConnectionError("the server requires authentication (types %s)" % list(types))
        self.sock.sendall(bytes([1]))                     # security type: None

        result = struct.unpack(">I", self._recv(4))[0]
        if result != 0:
            reason_len = struct.unpack(">I", self._recv(4))[0]
            raise ConnectionError(self._recv(reason_len).decode("utf-8", "replace"))

        self.sock.sendall(bytes([1]))                     # ClientInit: shared

        head = self._recv(24)
        self.width, self.height = struct.unpack(">HH", head[0:4])
        (self.bpp, self.depth, self.big_endian, self.true_colour,
         self.red_max, self.green_max, self.blue_max,
         self.red_shift, self.green_shift, self.blue_shift) = struct.unpack(">BBBBHHHBBB3x", head[4:20])
        name_len = struct.unpack(">I", head[20:24])[0]
        self.name = self._recv(name_len).decode("utf-8", "replace")

        # Ask for 32-bit true colour so the pixel handling below is the only
        # case that has to work.
        fmt = struct.pack(">BBBBHHHBBBxxx", 32, 24, 0, 1, 255, 255, 255, 16, 8, 0)
        self.sock.sendall(struct.pack(">Bxxx", 0) + fmt)
        self.bpp, self.depth = 32, 24
        self.red_shift, self.green_shift, self.blue_shift = 16, 8, 0
        self.red_max = self.green_max = self.blue_max = 255
        self.big_endian = 0

        # Raw encoding only: simple, and the framebuffer is small.
        self.sock.sendall(struct.pack(">BxH", 2, 1) + struct.pack(">i", 0))

    # ---------------------------------------------------------------- input

    def _key(self, keysym, down):
        self.sock.sendall(struct.pack(">BBxxI", 4, 1 if down else 0, keysym))

    def press(self, keysym, hold=0.012):
        self._key(keysym, True)
        time.sleep(hold)
        self._key(keysym, False)
        time.sleep(hold)

    def press_named(self, name):
        key = KEYSYMS.get(name.lower())
        if key is None:
            if len(name) == 1:
                self.type_text(name)
                return
            raise ValueError("unknown key %r" % name)
        self.press(key)

    def type_text(self, text, delay=0.02):
        for ch in text:
            if ch == "\n":
                self.press(KEYSYMS["enter"])
                continue
            if ch == "\t":
                self.press(KEYSYMS["tab"])
                continue
            needs_shift = ch.isupper() or ch in SHIFTED
            if needs_shift:
                self._key(KEYSYMS["shift"], True)
                time.sleep(0.008)
            self.press(ord(ch))
            if needs_shift:
                self._key(KEYSYMS["shift"], False)
                time.sleep(0.008)
            time.sleep(delay)

    def ctrl(self, ch):
        self._key(KEYSYMS["ctrl"], True)
        time.sleep(0.01)
        self.press(ord(ch))
        self._key(KEYSYMS["ctrl"], False)
        time.sleep(0.01)

    def ctrl_alt(self, ch):
        self._key(KEYSYMS["ctrl"], True)
        self._key(KEYSYMS["alt"], True)
        time.sleep(0.02)
        self.press(ord(ch))
        self._key(KEYSYMS["alt"], False)
        self._key(KEYSYMS["ctrl"], False)
        time.sleep(0.02)

    # -------------------------------------------------------------- pointer

    def _pointer(self, x, y, buttons):
        self.sock.sendall(struct.pack(">BBHH", 5, buttons, int(x), int(y)))

    def move(self, x, y, steps=8, delay=0.012):
        """Move in steps: a guest that tracks relative motion needs to see the
        journey, not just the destination."""
        if self._x is None:
            self._x, self._y = x, y
            self._pointer(x, y, self._buttons)
            time.sleep(delay)
            return
        x0, y0 = self._x, self._y
        for i in range(1, steps + 1):
            self._pointer(x0 + (x - x0) * i // steps,
                          y0 + (y - y0) * i // steps, self._buttons)
            time.sleep(delay)
        self._x, self._y = x, y

    def home(self):
        """Drive the pointer into the top-left corner.

        VNC carries absolute positions, but a PS/2 mouse only reports movement,
        so the hypervisor turns one into the other and the guest's idea of where
        the cursor is drifts.  Pushing well past the corner makes the guest
        clamp to (0, 0), which re-synchronises both sides."""
        for _ in range(3):
            self._pointer(0, 0, self._buttons)
            time.sleep(0.02)
        self._x, self._y = 0, 0
        time.sleep(0.05)

    def point(self, x, y):
        """Put the pointer exactly on (x, y), however far it has drifted."""
        self.home()
        self.move(x, y, steps=10)
        time.sleep(0.05)

    def click(self, x=None, y=None, button=1, hold=0.06):
        if x is not None:
            self.point(x, y)
        mask = 1 << (button - 1)
        self._buttons |= mask
        self._pointer(self._x, self._y, self._buttons)
        time.sleep(hold)
        self._buttons &= ~mask
        self._pointer(self._x, self._y, self._buttons)
        time.sleep(hold)

    def double_click(self, x=None, y=None):
        self.click(x, y)
        time.sleep(0.05)
        self.click()

    def drag(self, x0, y0, x1, y1, steps=14):
        self.point(x0, y0)
        self._buttons |= 1
        self._pointer(self._x, self._y, self._buttons)
        time.sleep(0.05)
        self.move(x1, y1, steps=steps)
        self._buttons &= ~1
        self._pointer(self._x, self._y, self._buttons)
        time.sleep(0.05)

    def scroll(self, notches):
        """Wheel up is button 4, down is button 5."""
        button = 4 if notches > 0 else 5
        for _ in range(abs(notches)):
            self.click(button=button, hold=0.03)

    # ----------------------------------------------------------- framebuffer

    def capture(self, incremental=False):
        """Return (width, height, rows) where rows are 3-byte-per-pixel RGB.

        The pixel buffer belongs to the connection and persists between calls.
        VMware's server sends only the rectangles that changed even when a full
        update is asked for, so starting from a blank buffer each time would
        leave everything that had not moved since the last capture black - which
        looks exactly like the guest having drawn a black screen."""
        self.sock.sendall(struct.pack(">BBHHHH", 3, 1 if incremental else 0,
                                      0, 0, self.width, self.height))

        if getattr(self, "pixels", None) is None:
            self.pixels = bytearray(self.width * self.height * 3)
            self._seen_any = False
        pixels = self.pixels
        deadline = time.time() + 15
        had_buffer = self._seen_any

        while time.time() < deadline:
            try:
                msg = self._recv(1)[0]
            except (TimeoutError, socket.timeout):
                # VMware answers a full-update request with silence when it
                # believes the client is already up to date.  The buffer we are
                # holding is that content, so return it rather than failing.
                if had_buffer:
                    return self.width, self.height, pixels
                raise
            if msg != 0:
                # Bell, clipboard or colour map: skip its payload and carry on.
                if msg == 2:
                    continue
                if msg == 3:
                    self._recv(3)
                    length = struct.unpack(">I", self._recv(4))[0]
                    self._recv(length)
                    continue
                if msg == 1:
                    self._recv(3)
                    n = struct.unpack(">H", self._recv(2))[0]
                    self._recv(n * 6)
                    continue
                raise ConnectionError("unexpected server message %d" % msg)

            self._recv(1)
            rects = struct.unpack(">H", self._recv(2))[0]
            for _ in range(rects):
                x, y, w, h, enc = struct.unpack(">HHHHi", self._recv(12))
                if enc != 0:
                    raise ConnectionError("server used encoding %d, expected raw" % enc)
                data = self._recv(w * h * 4)
                for row in range(h):
                    src = row * w * 4
                    dst = ((y + row) * self.width + x) * 3
                    for col in range(w):
                        b, g, r = data[src + col * 4], data[src + col * 4 + 1], data[src + col * 4 + 2]
                        pixels[dst + col * 3] = r
                        pixels[dst + col * 3 + 1] = g
                        pixels[dst + col * 3 + 2] = b
            self._seen_any = True
            return self.width, self.height, pixels

        if had_buffer:
            return self.width, self.height, pixels
        raise TimeoutError("no framebuffer update arrived")

    def screenshot(self, path):
        w, h, pixels = self.capture()
        stride = w * 3
        raw = bytearray()
        for y in range(h):
            raw.append(0)                       # PNG filter: none
            raw += pixels[y * stride:(y + 1) * stride]

        def chunk(tag, data):
            return (struct.pack(">I", len(data)) + tag + data
                    + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))

        png = (b"\x89PNG\r\n\x1a\n"
               + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
               + chunk(b"IDAT", zlib.compress(bytes(raw), 6))
               + chunk(b"IEND", b""))
        with open(path, "wb") as fh:
            fh.write(png)
        return w, h

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("action", choices=["shot", "type", "key", "ctrl", "info",
                                       "click", "move", "drag", "scroll", "dclick", "ctrlalt"])
    ap.add_argument("args", nargs="*")
    ap.add_argument("--host", default=DEFAULT_HOST)
    ap.add_argument("--port", type=int, default=DEFAULT_PORT)
    ap.add_argument("--enter", action="store_true", help="press Enter after typing")
    ap.add_argument("--delay", type=float, default=0.02)
    opts = ap.parse_args()

    vnc = Vnc(opts.host, opts.port)
    try:
        if opts.action == "info":
            print("%s: %dx%d, %d bpp" % (vnc.name, vnc.width, vnc.height, vnc.bpp))
        elif opts.action == "shot":
            path = opts.args[0] if opts.args else "screen.png"
            w, h = vnc.screenshot(path)
            print("wrote %s (%dx%d)" % (path, w, h))
        elif opts.action == "type":
            vnc.type_text(" ".join(opts.args), delay=opts.delay)
            if opts.enter:
                vnc.press(KEYSYMS["enter"])
        elif opts.action == "key":
            for name in opts.args:
                vnc.press_named(name)
        elif opts.action == "ctrl":
            for ch in opts.args:
                vnc.ctrl(ch)
        elif opts.action == "ctrlalt":
            for ch in opts.args:
                vnc.ctrl_alt(ch)
        elif opts.action == "move":
            vnc.point(int(opts.args[0]), int(opts.args[1]))
        elif opts.action == "click":
            vnc.click(int(opts.args[0]), int(opts.args[1]))
        elif opts.action == "dclick":
            vnc.double_click(int(opts.args[0]), int(opts.args[1]))
        elif opts.action == "drag":
            vnc.drag(int(opts.args[0]), int(opts.args[1]),
                     int(opts.args[2]), int(opts.args[3]))
        elif opts.action == "scroll":
            vnc.scroll(int(opts.args[0]))
    finally:
        vnc.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
