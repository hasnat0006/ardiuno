"""Send your PC activity status to the Arduino OLED over USB serial.

Privacy: this only counts HOW MANY keys you press. It never records or sends
WHICH keys you press, so nothing you type ever leaves this script.

Usage:
    pip install -r tools/requirements.txt
    python tools/status_sender.py            # auto-detects the Arduino
    python tools/status_sender.py --port COM5

Note: while this runs it holds the serial port, so stop it (Ctrl+C) before
uploading new firmware from PlatformIO.
"""

import argparse
import ctypes
import threading
import time
from collections import deque

import serial
import serial.tools.list_ports
from pynput import keyboard, mouse

TYPING_WINDOW = 3.0  # seconds since last key press that still count as "typing"
MOUSE_WINDOW = 1.5    # seconds since last mouse move that still count as "mouse"
AWAY_AFTER = 300      # seconds without any input before showing "away"
SEND_INTERVAL = 0.1   # 10 updates per second

# USB vendor IDs of common Arduino boards and USB-serial chips
ARDUINO_VIDS = {0x2341, 0x2A03, 0x1A86, 0x0403, 0x10C4}

user32 = ctypes.windll.user32
kernel32 = ctypes.windll.kernel32
kernel32.GetTickCount.restype = ctypes.c_uint
user32.SetProcessDPIAware()  # real pixel coordinates on scaled displays


class LASTINPUTINFO(ctypes.Structure):
    _fields_ = [("cbSize", ctypes.c_uint), ("dwTime", ctypes.c_uint)]


class POINT(ctypes.Structure):
    _fields_ = [("x", ctypes.c_long), ("y", ctypes.c_long)]


def idle_seconds():
    """Seconds since the last keyboard or mouse input anywhere on the PC."""
    info = LASTINPUTINFO(cbSize=ctypes.sizeof(LASTINPUTINFO))
    user32.GetLastInputInfo(ctypes.byref(info))
    return ((kernel32.GetTickCount() - info.dwTime) & 0xFFFFFFFF) / 1000.0


def mouse_percent():
    """Mouse position as 0-100 percent of the whole (multi-monitor) desktop."""
    pt = POINT()
    user32.GetCursorPos(ctypes.byref(pt))
    left, top = user32.GetSystemMetrics(76), user32.GetSystemMetrics(77)
    width, height = user32.GetSystemMetrics(78), user32.GetSystemMetrics(79)
    mx = (pt.x - left) * 100 // max(width, 1)
    my = (pt.y - top) * 100 // max(height, 1)
    return max(0, min(100, mx)), max(0, min(100, my))


class KeyCounter:
    """Counts key presses without remembering which keys they were."""

    def __init__(self):
        self.total = 0
        self.last_press = 0.0
        self.recent = deque()  # timestamps of presses in the last 10 s
        self.lock = threading.Lock()
        keyboard.Listener(on_press=self._on_press, daemon=True).start()

    def _on_press(self, _key):
        now = time.monotonic()
        with self.lock:
            self.total += 1
            self.last_press = now
            self.recent.append(now)

    def snapshot(self):
        now = time.monotonic()
        with self.lock:
            while self.recent and now - self.recent[0] > 10:
                self.recent.popleft()
            return self.total, len(self.recent) * 6, self.last_press


class ClickCounter:
    """Counts left and right mouse clicks (the OLED face winks on each one)."""

    def __init__(self):
        self.left = 0
        self.right = 0
        self.last_click = 0.0
        mouse.Listener(on_click=self._on_click, daemon=True).start()

    def _on_click(self, _x, _y, button, pressed):
        if not pressed:
            return
        if button == mouse.Button.left:
            self.left += 1
        elif button == mouse.Button.right:
            self.right += 1
        self.last_click = time.monotonic()


def find_port():
    for p in serial.tools.list_ports.comports():
        if p.vid in ARDUINO_VIDS:
            return p.device
    return None


def open_serial(port_arg):
    while True:
        port = port_arg or find_port()
        if port:
            try:
                ser = serial.Serial(port, 115200, timeout=0)
                print(f"Connected to {port}")
                time.sleep(2)  # opening the port resets the Arduino
                return ser
            except serial.SerialException as e:
                print(f"Can't open {port}: {e}")
        else:
            print("Arduino not found, retrying...")
        time.sleep(2)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--port", help="serial port, e.g. COM5 (default: auto-detect)")
    args = parser.parse_args()

    keys = KeyCounter()
    clicks = ClickCounter()
    ser = open_serial(args.port)
    last_mouse_pos = mouse_percent()
    last_mouse_move = 0.0
    last_state = None

    while True:
        now = time.monotonic()
        total, kpm, last_press = keys.snapshot()
        mx, my = mouse_percent()
        if (mx, my) != last_mouse_pos:
            last_mouse_pos = (mx, my)
            last_mouse_move = now
        last_mouse_move = max(last_mouse_move, clicks.last_click)
        idle = idle_seconds()

        if now - last_press < TYPING_WINDOW:
            state = "T"
        elif now - last_mouse_move < MOUSE_WINDOW:
            state = "A"
        elif idle >= AWAY_AFTER:
            state = "Z"
        else:
            state = "I"

        if state != last_state:
            print({"T": "typing", "A": "mouse", "I": "idle", "Z": "away"}[state])
            last_state = state

        msg = (f"{state},{total & 0xFFFF},{min(kpm, 999)},{mx},{my},"
               f"{int(min(idle, 65535))},{clicks.left & 0xFF},{clicks.right & 0xFF},"
               f"{time.strftime('%H:%M')}\n")
        try:
            ser.write(msg.encode())
            if b"boot" in ser.read(256):
                print(f"{time.strftime('%H:%M:%S')} Arduino restarted")
        except serial.SerialException:
            print("Lost connection, reconnecting...")
            ser.close()
            ser = open_serial(args.port)

        time.sleep(SEND_INTERVAL)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        pass
