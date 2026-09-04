#!/usr/bin/env python3
"""Raw serial terminal, purpose-built for playing the emulator over USB.

    python3 tools/serial_pad.py                  # finds the board itself
    python3 tools/serial_pad.py /dev/cu.usbmodem2101 115200

Exists because the two obvious tools do not work here. macOS ships screen
4.00.03 from 2006 without its setuid bit, so it dies with "Sorry, could not find
a PTY", and SIP will not let you put the bit back. picocom and pyserial are not
installed. This needs neither - stock python3 has termios.

What it adds over a plain terminal: it sends every keystroke immediately (the
Arduino Serial Monitor waits for Enter, which turns a held direction into one
90 ms tap) and it translates the arrow keys to WASD, so the sketch's serial
gamepad works without anyone remembering the letters.

    arrows / WASD   d-pad          K   A          J   B
    Enter  START    Space SELECT   Ctrl-]  quit

Holding a key relies on the OS key repeat renewing NES_SERIAL_HOLD_MS. macOS
waits ~500 ms before repeating, so a held direction gives one step, a pause,
then smooth movement. Raise the delay in System Settings > Keyboard, or raise
NES_SERIAL_HOLD_MS in hw_config.h, if it bothers you.
"""
import glob
import os
import select
import sys
import termios
import tty

QUIT = 0x1D                                  # Ctrl-]

# Arrow keys arrive as three-byte escape sequences; the sketch wants letters.
ARROWS = {b"\x1b[A": b"w", b"\x1b[B": b"s", b"\x1b[D": b"a", b"\x1b[C": b"d"}


def find_port():
    ports = sorted(glob.glob("/dev/cu.usbmodem*") + glob.glob("/dev/cu.wchusb*")
                   + glob.glob("/dev/cu.SLAB_USBtoUART*"))
    if not ports:
        sys.exit("no board found - is it plugged in? "
                 "(looked for /dev/cu.usbmodem*)")
    if len(ports) > 1:
        print(f"several ports found, using {ports[0]}: {', '.join(ports)}",
              file=sys.stderr)
    return ports[0]


def open_serial(path, baud):
    """cu.* rather than tty.*: opening a cu device does not assert DTR, so this
    does not reset the board out from under a running game."""
    fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)

    speed = getattr(termios, f"B{baud}", None)
    if speed is None:
        os.close(fd)
        sys.exit(f"unsupported baud rate {baud}")

    attrs = termios.tcgetattr(fd)
    attrs[0] = 0                                          # iflag: no XON, no CR mapping
    attrs[1] = 0                                          # oflag: no post-processing
    attrs[2] = termios.CREAD | termios.CLOCAL | termios.CS8
    attrs[3] = 0                                          # lflag: no echo, no canon
    attrs[4] = attrs[5] = speed
    attrs[6][termios.VMIN] = 0
    attrs[6][termios.VTIME] = 0
    termios.tcsetattr(fd, termios.TCSANOW, attrs)
    return fd


def translate(data, pending):
    """Rewrites arrow sequences to WASD. `pending` carries a partial escape
    sequence across reads, which happens whenever key repeat lands on a buffer
    boundary - without it a held arrow would occasionally emit a stray '['."""
    buf = pending + data
    out = bytearray()

    while buf:
        if buf[0:1] == b"\x1b":
            if len(buf) < 3:
                return bytes(out), buf          # incomplete - wait for more
            seq, buf = buf[:3], buf[3:]
            out += ARROWS.get(seq, b"")         # unknown escape: drop it
        else:
            out += buf[0:1]
            buf = buf[1:]

    return bytes(out), b""


def main(argv):
    path = argv[1] if len(argv) > 1 else find_port()
    baud = int(argv[2]) if len(argv) > 2 else 115200

    fd = open_serial(path, baud)
    stdin_fd = sys.stdin.fileno()
    saved = termios.tcgetattr(stdin_fd)

    print(f"{path} at {baud} - arrows/WASD, K=A, J=B, Enter=START, "
          f"Space=SELECT, Ctrl-] quits\r")

    pending = b""
    try:
        tty.setraw(stdin_fd)
        while True:
            ready, _, _ = select.select([fd, stdin_fd], [], [])

            if fd in ready:
                try:
                    data = os.read(fd, 4096)
                except BlockingIOError:
                    data = b""
                if data:
                    os.write(sys.stdout.fileno(), data)

            if stdin_fd in ready:
                data = os.read(stdin_fd, 64)
                if not data or QUIT in data:
                    break
                keys, pending = translate(data, pending)
                if keys:
                    os.write(fd, keys)
    finally:
        termios.tcsetattr(stdin_fd, termios.TCSADRAIN, saved)
        os.close(fd)
        print("\ndisconnected")

    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
