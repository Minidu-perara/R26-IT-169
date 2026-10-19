#!/usr/bin/env python3
"""Capture ESP32 serial output for a fixed number of seconds.

The board is reset on open by pulsing RTS (which drives EN on the usual
auto-reset circuit) while DTR -- which drives IO0 -- is held de-asserted, so
the chip reboots into the application and never into the ROM bootloader. The
capture therefore starts at the boot banner.

Output is read as raw bytes and split on newlines here rather than with
Serial.readline(): the firmware emits partial lines (e.g. "[MQTT]
connecting..." with no newline before its result), and readline() under a read
timeout splits those at arbitrary points. Each completed line is written and
flushed immediately, so a crash or a kill still leaves a usable partial log.
The capture never runs past --seconds.

Usage:
    python tools/capture_serial.py --port COM6 --baud 115200 \
        --seconds 120 --outfile eval/serial_bringup.log
"""

import argparse
import os
import sys
import time

import serial


def main():
    ap = argparse.ArgumentParser(description="Timed ESP32 serial capture.")
    ap.add_argument("--port", required=True, help="Serial port, e.g. COM6")
    ap.add_argument("--baud", type=int, default=115200, help="Baud rate")
    ap.add_argument("--seconds", type=float, required=True, help="Capture duration")
    ap.add_argument("--outfile", required=True, help="Log file to write")
    ap.add_argument("--timestamp", action="store_true",
                    help="Prefix each line with seconds since capture start")
    args = ap.parse_args()

    outdir = os.path.dirname(os.path.abspath(args.outfile))
    if outdir:
        os.makedirs(outdir, exist_ok=True)

    ser = serial.Serial()
    ser.port = args.port
    ser.baudrate = args.baud
    ser.timeout = 0.05          # short, so the deadline is always honoured
    ser.dtr = False             # IO0 high -- never the ROM bootloader
    ser.rts = False
    ser.open()

    # Pulse EN low then high: RTS is the reset line on the standard
    # DTR/RTS auto-reset circuit. DTR stays de-asserted throughout.
    ser.dtr = False
    ser.rts = True
    time.sleep(0.1)
    ser.reset_input_buffer()
    ser.rts = False
    # Bytes the firmware emitted before the reset can still be sitting in the
    # USB pipeline. The chip cannot have produced anything yet at this point,
    # so one more flush drops the stale tail without eating the boot banner.
    ser.reset_input_buffer()

    start = time.monotonic()
    deadline = start + args.seconds
    buf = bytearray()
    lines = 0

    def emit(fh, raw):
        nonlocal lines
        text = raw.decode("utf-8", errors="replace").rstrip("\r")
        if args.timestamp:
            text = "[%7.3f] %s" % (time.monotonic() - start, text)
        fh.write(text + "\n")
        fh.flush()
        os.fsync(fh.fileno())
        lines += 1
        # The pre-app ROM boot preamble is sent at 74880 baud and read here at
        # the app's rate, so it decodes as garbage; the console's codec (not
        # always UTF-8, e.g. Windows cp1252) can then reject characters that
        # utf-8 "replace" above let through. The file (already flushed) must
        # never be lost over a console-only encoding error.
        try:
            print(text)
        except UnicodeEncodeError:
            print(text.encode(sys.stdout.encoding or "ascii", errors="replace")
                      .decode(sys.stdout.encoding or "ascii"))
        sys.stdout.flush()

    try:
        with open(args.outfile, "w", encoding="utf-8", newline="") as fh:
            while time.monotonic() < deadline:
                chunk = ser.read(max(1, ser.in_waiting))
                if not chunk:
                    continue
                buf.extend(chunk)
                while b"\n" in buf:
                    raw, _, rest = buf.partition(b"\n")
                    buf = bytearray(rest)
                    emit(fh, raw)
            if buf:                      # trailing partial line, no newline
                emit(fh, bytes(buf))
    except KeyboardInterrupt:
        print("[capture] interrupted, partial log kept", file=sys.stderr)
    finally:
        ser.close()

    print("[capture] %d lines -> %s" % (lines, args.outfile), file=sys.stderr)


if __name__ == "__main__":
    main()
