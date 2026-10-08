#!/usr/bin/env python3
"""
Linux side of the Divaller shim for TLAC under Wine.

Owns the Divaller (VID 0E8F / PID 2213) through libusb and serves its pipes
to DivallerBridge.dva over TCP on 127.0.0.1. Run it before (or while) the game
is running; the shim reconnects on its own.

    pip install pyusb            # plus libusb-1.0 from your distro
    python3 divaller_bridge.py   # --port N, --verbose, --tui

Protocol (requests start with one opcode byte):
    'P'                         -> status(1)                   device present?
    'R' ep(1) max(1)            -> status(1) len(1) data[len]  latest IN packet
    'W' ep(1) len(1) data[len]  -> status(1)                   queue OUT packet
"""

import argparse
import collections
import socket
import socketserver
import threading
import time

import usb.core
import usb.util

VID, PID = 0x0E8F, 0x2213
EP_IN, EP_OUT = 0x84, 0x03
# TLAC checks for this header ("BVZ") on every input packet.
HEADER = bytes((0x42, 0x56, 0x5A))

# Log sink: plain print normally; the TUI swaps in a scrolling buffer.
LOG = collections.deque(maxlen=200)
_log_to_buffer = False


def log(*args):
    msg = " ".join(str(a) for a in args)
    if _log_to_buffer:
        LOG.append(time.strftime("%H:%M:%S ") + msg)
    else:
        print(msg, flush=True)


def decode(pkt):
    """Decode an input packet the same way TLAC's Divaller.cpp does.
    Returns (buttons dict, list of 32 slider sensors where index 0 is the
    sensor TLAC calls 0)."""
    s = (pkt or b"") + bytes(24)
    buttons = {
        "triangle": s[3] >> 4 & 1,
        "square":   s[3] >> 2 & 1,
        "cross":    s[3] >> 5 & 1,
        "circle":   s[3] >> 3 & 1,
        "fn":       s[3] >> 1 & 1,
        "l1":       s[5] & 1,
        "l2":       s[4] >> 7 & 1,
        "l3":       s[4] >> 6 & 1,
    }
    raw = (s[5] >> 4 | s[6] << 4 | s[7] << 12 | s[8] << 20 | s[9] << 28) & 0xFFFFFFFF
    rev = int(f"{raw:032b}"[::-1], 2)   # TLAC reverses the bit order
    # TLAC's TouchSliderEmulator: sensor i is bit (31 - i).
    sensors = [rev >> (31 - i) & 1 for i in range(32)]
    return buttons, sensors


class Divaller:
    """Keeps the newest input packet and the newest LED packet, so the game
    never waits on USB: reads return the latest state, writes are queued."""

    def __init__(self, verbose=False):
        self.verbose = verbose
        self.lock = threading.Lock()
        self.dev = None
        self.latest = None          # newest IN packet (bytes)
        self.pending_out = None     # newest OUT packet waiting to be sent
        self.out_event = threading.Event()
        self.packets = 0            # IN packets received, for the rate display
        self.clients = 0            # connected game instances
        threading.Thread(target=self._reader, daemon=True).start()
        threading.Thread(target=self._writer, daemon=True).start()

    @property
    def present(self):
        return self.dev is not None

    def _open(self):
        dev = usb.core.find(idVendor=VID, idProduct=PID)
        if dev is None:
            return None
        try:
            cfg = dev.get_active_configuration()
        except usb.core.USBError:
            cfg = None
        if cfg is None:
            self._step("setting configuration", dev.set_configuration)
            cfg = dev.get_active_configuration()

        # Use the interface that actually carries the input endpoint rather
        # than assuming interface 0.
        intf = None
        for candidate in cfg:
            eps = [ep.bEndpointAddress for ep in candidate]
            if self.verbose or EP_IN not in eps:
                log(f"  interface {candidate.bInterfaceNumber} alt {candidate.bAlternateSetting}: "
                      f"class 0x{candidate.bInterfaceClass:02x}, endpoints "
                      + ", ".join(f"0x{e:02x}" for e in eps))
            if intf is None and EP_IN in eps:
                intf = candidate
        if intf is None:
            raise usb.core.USBError(
                f"no interface has endpoint 0x{EP_IN:02x} (layout printed above)")

        n = intf.bInterfaceNumber
        try:
            if dev.is_kernel_driver_active(n):
                self._step(f"detaching kernel driver from interface {n}",
                           lambda: dev.detach_kernel_driver(n))
        except NotImplementedError:
            pass
        self._step(f"claiming interface {n}", lambda: usb.util.claim_interface(dev, n))
        if intf.bAlternateSetting != 0:
            self._step(f"selecting alt setting {intf.bAlternateSetting}",
                       lambda: dev.set_interface_altsetting(n, intf.bAlternateSetting))
        log(f"Using interface {n}")
        return dev

    @staticmethod
    def _step(what, fn):
        try:
            return fn()
        except usb.core.USBError as e:
            raise usb.core.USBError(f"{what}: {e}") from e

    def _drop(self, why):
        log(f"Divaller lost: {why}")
        with self.lock:
            dev, self.dev, self.latest = self.dev, None, None
        if dev is not None:
            try:
                usb.util.dispose_resources(dev)
            except usb.core.USBError:
                pass

    def _reader(self):
        while True:
            if self.dev is None:
                try:
                    dev = self._open()
                except usb.core.USBError as e:
                    hint = ""
                    if getattr(e, "errno", None) == 13 or "Access denied" in str(e):
                        hint = " (check the udev rule / permissions)"
                    elif "busy" in str(e).lower():
                        hint = " (something else has claimed it; see README troubleshooting)"
                    log(f"Found the Divaller but couldn't open it: {e}{hint}")
                    dev = None
                    time.sleep(4)  # don't spam the same error every second
                if dev is None:
                    time.sleep(1)
                    continue
                with self.lock:
                    self.dev = dev
                log("Divaller connected")
            try:
                data = bytes(self.dev.read(EP_IN, 64, timeout=100))
            except usb.core.USBTimeoutError:
                continue
            except usb.core.USBError as e:
                self._drop(e)
                continue
            if data[:3] != HEADER and self.verbose:
                log(f"unexpected packet: {data.hex()}")
            with self.lock:
                self.latest = data
                self.packets += 1

    def _writer(self):
        while True:
            self.out_event.wait()
            self.out_event.clear()
            with self.lock:
                data, self.pending_out = self.pending_out, None
                dev = self.dev
            if data is None or dev is None:
                continue
            try:
                dev.write(EP_OUT, data, timeout=100)
            except usb.core.USBError as e:
                if self.verbose:
                    log(f"LED write failed: {e}")

    def read(self, max_len):
        with self.lock:
            if self.dev is None:
                return None
            if self.latest is None:
                # Nothing received yet: report "header, nothing pressed".
                return (HEADER + bytes(21))[:max_len]
            return self.latest[:max_len]

    def write(self, data):
        with self.lock:
            if self.dev is None:
                return False
            self.pending_out = data
        self.out_event.set()
        return True


def make_handler(divaller):
    class Handler(socketserver.BaseRequestHandler):
        def recv_exact(self, n):
            buf = b""
            while len(buf) < n:
                chunk = self.request.recv(n - len(buf))
                if not chunk:
                    raise ConnectionError
                buf += chunk
            return buf

        def handle(self):
            self.request.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            log(f"game connected from {self.client_address[0]}")
            divaller.clients += 1
            try:
                while True:
                    op = self.recv_exact(1)
                    if op == b"P":
                        self.request.sendall(b"\x01" if divaller.present else b"\x00")
                    elif op == b"R":
                        _ep, max_len = self.recv_exact(2)
                        data = divaller.read(max_len)
                        if data is None:
                            self.request.sendall(b"\x00\x00")
                        else:
                            self.request.sendall(bytes((1, len(data))) + data)
                    elif op == b"W":
                        _ep, length = self.recv_exact(2)
                        data = self.recv_exact(length)
                        self.request.sendall(b"\x01" if divaller.write(data) else b"\x00")
                    else:
                        log(f"unknown opcode {op!r}, dropping client")
                        return
            except (ConnectionError, OSError):
                pass
            divaller.clients -= 1
            log("game disconnected")

    return Handler


class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


# ------------------------------------------------------------------ TUI

FACE = [  # left to right, as on the controller
    ("triangle", "△", "Triangle", "green"),
    ("square",   "□", "Square",   "magenta"),
    ("cross",    "✕", "Cross",    "blue"),
    ("circle",   "○", "Circle",   "red"),
]
SMALL = [("l1", "L1", "coin"), ("l2", "L2", "service"), ("l3", "L3", "test")]


def tui(stdscr, divaller, port):
    import curses

    curses.curs_set(0)
    stdscr.nodelay(True)
    stdscr.timeout(16)  # ~60 fps

    pairs = {}
    if curses.has_colors():
        curses.start_color()
        try:
            curses.use_default_colors()
            bg = -1
        except curses.error:
            bg = curses.COLOR_BLACK
        for n, (name, col) in enumerate([
                ("green", curses.COLOR_GREEN), ("magenta", curses.COLOR_MAGENTA),
                ("blue", curses.COLOR_BLUE), ("red", curses.COLOR_RED),
                ("cyan", curses.COLOR_CYAN), ("yellow", curses.COLOR_YELLOW)], start=1):
            curses.init_pair(n, col, bg)
            pairs[name] = curses.color_pair(n)

    def color(name):
        return pairs.get(name, 0)

    def put(y, x, text, attr=0):
        try:
            stdscr.addstr(y, x, text, attr)
        except curses.error:
            pass  # off-screen on a small terminal

    rate, last_count, last_t = 0.0, 0, time.monotonic()

    while True:
        key = stdscr.getch()
        if key in (ord("q"), ord("Q"), 27):
            return

        now = time.monotonic()
        if now - last_t >= 0.5:
            rate = (divaller.packets - last_count) / (now - last_t)
            last_count, last_t = divaller.packets, now

        with divaller.lock:
            pkt = divaller.latest
            present = divaller.dev is not None
        buttons, sensors = decode(pkt)

        stdscr.erase()
        W = 76
        x0 = 1

        # Frame
        put(0, x0, "╭" + "─" * (W - 2) + "╮", color("cyan"))
        for y in range(1, 12):
            put(y, x0, "│", color("cyan"))
            put(y, x0 + W - 1, "│", color("cyan"))
        put(12, x0, "╰" + "─" * (W - 2) + "╯", color("cyan"))
        put(0, x0 + 3, " DIVALLER ", curses.A_BOLD)

        # Small buttons (top left) and FN (top right)
        x = x0 + 3
        for key_, label, role in SMALL:
            on = buttons[key_]
            attr = color("magenta") | (curses.A_REVERSE | curses.A_BOLD if on else 0)
            put(1, x, f" {'●' if on else '○'} {label} ", attr)
            put(1, x + 7, role, curses.A_DIM)
            x += 7 + len(role) + 2
        on = buttons["fn"]
        attr = color("yellow") | (curses.A_REVERSE | curses.A_BOLD if on else 0)
        put(1, x0 + W - 18, f" {'●' if on else '○'} FN ", attr)
        put(1, x0 + W - 10, "start", curses.A_DIM)

        # Slider: 32 sensors, 2 columns each
        sx = x0 + 4
        put(3, sx + 27, "TOUCH SLIDER", curses.A_DIM)
        put(4, sx, "L ▕", curses.A_BOLD)
        for i, on in enumerate(sensors):
            if on:
                put(4, sx + 3 + i * 2, "██", color("cyan") | curses.A_BOLD)
            else:
                put(4, sx + 3 + i * 2, "··" if i % 4 else "╎·", curses.A_DIM)
        put(4, sx + 3 + 64, "▏ R", curses.A_BOLD)
        for i in (0, 8, 16, 24, 31):
            put(5, sx + 3 + i * 2, str(i), curses.A_DIM)
        touched = sum(sensors)
        bits = "".join(str(b) for b in sensors)
        put(6, sx + 3, " ".join(bits[i:i + 8] for i in range(0, 32, 8)), curses.A_DIM)
        put(6, sx + 3 + 40, f"{touched:2d}/32 touched", curses.A_DIM)

        # Face buttons
        bx = x0 + 9
        for key_, sym, name, col in FACE:
            on = buttons[key_]
            box = color(col) | (curses.A_BOLD if on else 0)
            fill = color(col) | curses.A_REVERSE | curses.A_BOLD if on else color(col)
            put(8, bx, "╭───────╮", box)
            put(9, bx, "│", box)
            put(9, bx + 1, f"   {sym}   ", fill)
            put(9, bx + 8, "│", box)
            put(10, bx, "╰───────╯", box)
            put(11, bx + (9 - len(name)) // 2, name, curses.A_BOLD if on else curses.A_DIM)
            bx += 16

        # Status
        dev_txt = "connected" if present else "not found"
        put(13, x0 + 1, "Divaller: ", curses.A_DIM)
        put(13, x0 + 11, dev_txt, color("green" if present else "red") | curses.A_BOLD)
        game_txt = f"connected ({divaller.clients})" if divaller.clients else "waiting"
        put(13, x0 + 24, "Game: ", curses.A_DIM)
        put(13, x0 + 30, game_txt, color("green" if divaller.clients else "yellow") | curses.A_BOLD)
        put(13, x0 + 48, f"{rate:6.0f} pkt/s   port {port}", curses.A_DIM)
        raw = pkt.hex(" ") if pkt else "(no packet yet)"
        put(14, x0 + 1, "raw: " + raw[:W - 7], curses.A_DIM)
        put(15, x0 + 1, "q to quit", curses.A_DIM)

        # Log tail
        h, _ = stdscr.getmaxyx()
        lines = list(LOG)[-(max(0, h - 17)):] if h > 17 else []
        for n, line in enumerate(lines):
            put(17 + n, x0 + 1, line[:W])

        stdscr.refresh()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", type=int, default=45710)
    ap.add_argument("--verbose", action="store_true")
    ap.add_argument("--tui", action="store_true",
                    help="show a live diagram of the buttons and slider")
    args = ap.parse_args()

    global _log_to_buffer
    _log_to_buffer = args.tui

    divaller = Divaller(args.verbose)
    with Server(("127.0.0.1", args.port), make_handler(divaller)) as srv:
        log(f"Divaller bridge listening on 127.0.0.1:{args.port}")
        if not args.tui:
            try:
                srv.serve_forever()
            except KeyboardInterrupt:
                pass
            return

        import curses
        import locale
        locale.setlocale(locale.LC_ALL, "")
        threading.Thread(target=srv.serve_forever, daemon=True).start()
        try:
            curses.wrapper(tui, divaller, args.port)
        except KeyboardInterrupt:
            pass
        finally:
            _log_to_buffer = False
            srv.shutdown()


if __name__ == "__main__":
    main()
