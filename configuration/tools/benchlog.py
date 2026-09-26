#!/usr/bin/env python
"""Capture EVERY line from the node and hub serial ports into one session
directory, each line stamped with host wall time and a monotonic offset.

    python tools/benchlog.py [--node COM6] [--hub COM9] [--tag mac1]

Nothing is filtered: analysis happens afterwards, on the files, as often as
needed. The ports are opened with DTR/RTS held low so that OPENING does not
reset the board. CLOSING can still reset an ESP32 through the auto-reset
circuit, so leave this running for the whole stimulus sequence and stop it
last (Ctrl+C, or `stop` file in the session dir).

Output (logs/<timestamp>_<tag>/):
    node.log  hub.log   one line per serial line: "<wall> <t+sec> <text>"
    merged.log          both streams interleaved in arrival order, prefixed N|/H|
    notes.txt           append-only: `python tools/benchlog.py --note "pressed X"`
    `--reset node|hub`  pulse EN via RTS on the port the logger already holds
"""
import argparse, datetime, os, sys, threading, time

import serial

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "logs")
ANSI = b"\x1b["


def reader(name, port, baud, outdir, merged, mlock, t0, stop):
    tag = name[0].upper()
    s = serial.Serial()
    s.port, s.baudrate, s.timeout = port, baud, 0.2
    s.dtr = False
    s.rts = False
    s.open()
    buf = b""
    trigger = os.path.join(outdir, f"reset_{name}")
    with open(os.path.join(outdir, f"{name}.log"), "ab", buffering=0) as f:
        while not stop.is_set():
            # `python tools/benchlog.py --reset node` drops this file: pulse EN via
            # RTS on the port WE ALREADY HOLD, so the boot is captured without
            # closing the port (closing is what resets an ESP32 uncontrollably).
            if os.path.exists(trigger):
                os.remove(trigger)
                s.dtr = False
                s.rts = True
                time.sleep(0.15)
                s.rts = False
                f.write(f"{datetime.datetime.now():%H:%M:%S.%f}"[:-3].encode() + b" ---- benchlog: RTS reset pulsed ----\n")
            data = s.read(4096)
            if not data:
                continue
            buf += data
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                line = line.rstrip(b"\r")
                now = datetime.datetime.now().strftime("%H:%M:%S.%f")[:-3]
                out = f"{now} {time.monotonic() - t0:9.3f} ".encode() + line + b"\n"
                f.write(out)
                with mlock:
                    merged.write(f"{tag}|".encode() + out)
    s.close()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--node", default="COM6")
    ap.add_argument("--hub", default="COM9")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--tag", default="session")
    ap.add_argument("--note")
    ap.add_argument("--reset", choices=["node", "hub"], help="pulse EN on that port, in the newest session")
    ap.add_argument("--dir", help="session dir for --note (default: newest)")
    a = ap.parse_args()

    if a.reset:
        d = a.dir or max((os.path.join(ROOT, x) for x in os.listdir(ROOT)), key=os.path.getmtime)
        open(os.path.join(d, f"reset_{a.reset}"), "w").close()
        with open(os.path.join(d, "notes.txt"), "a") as f:
            f.write(f"{datetime.datetime.now():%H:%M:%S.%f}"[:-3] + f" reset requested: {a.reset}\n")
        return

    if a.note:
        d = a.dir or max((os.path.join(ROOT, x) for x in os.listdir(ROOT)), key=os.path.getmtime)
        with open(os.path.join(d, "notes.txt"), "a") as f:
            f.write(f"{datetime.datetime.now():%H:%M:%S.%f}"[:-3] + f" {a.note}\n")
        return

    d = os.path.join(ROOT, datetime.datetime.now().strftime("%Y%m%d_%H%M%S_") + a.tag)
    os.makedirs(d)
    print("session dir:", os.path.abspath(d), flush=True)
    stop = threading.Event()
    mlock = threading.Lock()
    t0 = time.monotonic()
    with open(os.path.join(d, "merged.log"), "ab", buffering=0) as merged:
        ts = [threading.Thread(target=reader, args=(n, p, a.baud, d, merged, mlock, t0, stop), daemon=True)
              for n, p in (("node", a.node), ("hub", a.hub)) if p]
        for t in ts:
            t.start()
        try:
            while not os.path.exists(os.path.join(d, "stop")) and all(t.is_alive() for t in ts):
                time.sleep(0.5)
        except KeyboardInterrupt:
            pass
        stop.set()
        for t in ts:
            t.join(2)


if __name__ == "__main__":
    main()
