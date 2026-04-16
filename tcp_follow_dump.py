#!/usr/bin/env python3
"""Dump follow_history for $24/$25/$C5/$2C after a fire."""
import socket, json


def cmd(c):
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(10)
    s.connect(("127.0.0.1", 4372))
    s.sendall((json.dumps(c) + "\n").encode())
    buf = b""
    while b"\n" not in buf:
        chunk = s.recv(65536)
        if not chunk:
            break
        buf += chunk
    s.close()
    return json.loads(buf.decode().strip())


LABELS = {0x24: "$24 (dispatch select)",
          0x25: "$25 (mode)",
          0xC5: "$C5 (detection counter)",
          0x2C: "$2C (mode timer)"}

print("now:", cmd({"cmd": "frame"}))
for a, label in LABELS.items():
    print(f"\n===== {label} =====")
    r = cmd({"cmd": "follow_history", "addr": f"{a:04X}", "limit": 80})
    if not r.get("ok"):
        print("  ERR:", r)
        continue
    events = r.get("events", r.get("history", []))
    if not events:
        print("  (no writes observed)")
        continue
    for e in events:
        f  = e.get("frame", "?")
        v  = e.get("val", e.get("value", "?"))
        fn = e.get("fn", e.get("function", e.get("caller", "?")))
        pc = e.get("pc", "?")
        print(f"  frame={f:>6}  val={v}  fn={fn}  pc={pc}")
