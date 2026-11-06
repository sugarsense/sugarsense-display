"""Talk to a connected display over its USB serial console (hw.h, the three
line commands), to check a build from the Mac without looking at the glass.

    devtool.py shot out.png      the frame the board is showing, as a PNG
    devtool.py tap X Y           a tap at logical coordinates
    devtool.py info              one status line
    devtool.py log SECONDS       print what the board logs
    devtool.py trace             the board's last 64 events with their time

Needs pyserial, and Pillow for `shot`. The port is the first
/dev/cu.usbmodem*; pass --port to name another.
"""
import base64
import glob
import sys
import time

import serial


def open_port(port):
    ser = serial.Serial()
    ser.port = port
    ser.baudrate = 115200
    ser.timeout = 0.2
    # Leave DTR and RTS at pyserial's default (both asserted, as the OS sets
    # them on open). The ESP32-S3's USB serial resets the chip when RTS is up
    # while DTR is down, and clearing the two one after the other passes
    # through exactly that state: the board would reboot on every command.
    ser.open()
    return ser


def read_lines(ser, seconds, stop=None):
    deadline = time.time() + seconds
    buf = b""
    lines = []
    while time.time() < deadline:
        buf += ser.read(65536)
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            text = line.decode("utf-8", "replace").rstrip("\r")
            lines.append(text)
            if stop and stop(text):
                return lines
    return lines


def shot(ser, out):
    from PIL import Image
    ser.reset_input_buffer()
    ser.write(b"shot\n")
    lines = read_lines(ser, 20, stop=lambda t: t == "END")
    start = next((i for i, t in enumerate(lines) if t.startswith("SHOT ")), None)
    if start is None or lines[-1] != "END":
        sys.exit("no frame came back (is the firmware running?)")
    nw, nh, rot = (int(v) for v in lines[start].split()[1:4])
    raw = b"".join(base64.b64decode(t) for t in lines[start + 1:-1])
    if len(raw) != nw * nh * 2:
        sys.exit(f"short frame: {len(raw)} bytes")
    w, h = (nh, nw) if rot & 1 else (nw, nh)
    img = Image.new("RGB", (w, h))
    px = img.load()
    for y in range(h):
        for x in range(w):
            # the firmware's own mapping (gx.h at): logical -> the native cell
            if rot == 1:
                idx = x * nw + (nw - 1 - y)
            elif rot == 2:
                idx = (nh - 1 - y) * nw + (nw - 1 - x)
            elif rot == 3:
                idx = (nh - 1 - x) * nw + y
            else:
                idx = y * nw + x
            c = raw[2 * idx] | (raw[2 * idx + 1] << 8)
            r, g, b = (c >> 11) & 31, (c >> 5) & 63, c & 31
            px[x, y] = ((r * 527 + 23) >> 6, (g * 259 + 33) >> 6, (b * 527 + 23) >> 6)
    img.save(out)
    print(f"saved {out} ({w}x{h}, rotation {rot})")


def main():
    args = sys.argv[1:]
    port = None
    if "--port" in args:
        i = args.index("--port")
        port = args[i + 1]
        del args[i:i + 2]
    if not port:
        ports = glob.glob("/dev/cu.usbmodem*")
        if not ports:
            sys.exit("no /dev/cu.usbmodem* port: is the display plugged in?")
        port = ports[0]
    if not args:
        sys.exit(__doc__)
    ser = open_port(port)
    cmd = args[0]
    if cmd == "shot":
        shot(ser, args[1])
    elif cmd == "tap":
        ser.write(f"tap {int(args[1])} {int(args[2])}\n".encode())
        time.sleep(0.3)
    elif cmd == "info":
        ser.reset_input_buffer()
        ser.write(b"info\n")
        for t in read_lines(ser, 5, stop=lambda t: t.startswith("INFO ")):
            if t.startswith("INFO "):
                print(t)
    elif cmd == "trace":
        ser.reset_input_buffer()
        ser.write(b"trace\n")
        prev = None
        for t in read_lines(ser, 5, stop=lambda t: t == "TRACE END"):
            if not t.startswith("TRACE ") or t == "TRACE END":
                continue
            _, ms, kind, a, b = t.split()
            gap = "" if prev is None else f"+{int(ms) - prev}"
            prev = int(ms)
            print(f"{int(ms):>9} {gap:>7}  {kind:<7} {a} {b}")
    elif cmd == "log":
        end = time.time() + float(args[1])
        while time.time() < end:
            data = ser.read(4096)
            if data:
                sys.stdout.write(data.decode("utf-8", "replace"))
                sys.stdout.flush()
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
