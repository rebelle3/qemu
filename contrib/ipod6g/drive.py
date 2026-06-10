#!/usr/bin/env python3
"""Drive a running ipod6g machine over QMP: inject click-wheel keys and
take screendumps (PNG written next to the requested name).

Usage: drive.py <qmp-socket> [cmd...]
  cmds: key names (ret, up, down, left, right, bracket_left,
        bracket_right), 'sleep:<secs>', 'shot:<name>'
Key mapping inside the machine: up=MENU down=PLAY left/right=LEFT/RIGHT
ret=SELECT bracket_right/left=wheel scroll.
"""
import json, socket, struct, sys, time, zlib


class QMP:
    def __init__(self, path):
        self.s = socket.socket(socket.AF_UNIX)
        self.s.connect(path)
        self.f = self.s.makefile("rw")
        self.f.readline()
        self.cmd("qmp_capabilities")

    def cmd(self, c, **a):
        self.f.write(json.dumps({"execute": c, "arguments": a}) + "\n")
        self.f.flush()
        while True:
            r = json.loads(self.f.readline())
            if "return" in r or "error" in r:
                if "error" in r:
                    print("QMP error:", r["error"], file=sys.stderr)
                return r

    def key(self, k):
        for down in (True, False):
            self.cmd("input-send-event", events=[{
                "type": "key",
                "data": {"down": down, "key": {"type": "qcode", "data": k}}}])
            time.sleep(0.15)
        time.sleep(0.5)

    def shot(self, name):
        ppm = "/tmp/%s.ppm" % name
        self.cmd("screendump", filename=ppm)
        data = open(ppm, "rb").read()
        parts = data.split(b"\n", 3)
        w, h = map(int, parts[1].split())
        raw = parts[3][-(w * h * 3):]
        rows = b"".join(b"\x00" + raw[y * w * 3:(y + 1) * w * 3]
                        for y in range(h))
        def chunk(t, d):
            return (struct.pack(">I", len(d)) + t + d +
                    struct.pack(">I", zlib.crc32(t + d) & 0xffffffff))
        png = (b"\x89PNG\r\n\x1a\n" +
               chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) +
               chunk(b"IDAT", zlib.compress(rows, 6)) + chunk(b"IEND", b""))
        out = "%s.png" % name
        open(out, "wb").write(png)
        print("wrote", out)


def main():
    q = QMP(sys.argv[1])
    for c in sys.argv[2:]:
        if c.startswith("shot:"):
            q.shot(c[5:])
        elif c.startswith("sleep:"):
            time.sleep(float(c[6:]))
        else:
            q.key(c)


if __name__ == "__main__":
    main()
