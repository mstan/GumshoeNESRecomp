#!/usr/bin/env python3
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


LABELS = {0x24: "$24", 0x25: "$25", 0xC5: "$C5", 0x2C: "$2C"}
print("now:", cmd({"cmd": "frame"}))
for a, label in LABELS.items():
    print(f"\n===== {label} =====")
    r = cmd({"cmd": "follow_history", "addr": f"{a:04X}", "limit": 80})
    if not r.get("ok"):
        print("  ERR:", r); continue
    for e in r.get("entries", []):
        print(f"  frame={e.get('f'):>6}  {e.get('old')}->{e.get('new')}  stack={e.get('stack')}")
