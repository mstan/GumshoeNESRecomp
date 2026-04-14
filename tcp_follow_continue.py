#!/usr/bin/env python3
"""Follow-history dump with no pause — assumes already following."""
import socket, json, time


def cmd(c):
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(10)
    s.connect(("127.0.0.1", 4372))
    s.sendall((json.dumps(c) + "\n").encode())
    buf = b""
    while b"\n" not in buf:
        chunk = s.recv(65536)
        if not chunk: break
        buf += chunk
    s.close()
    return json.loads(buf.decode().strip())


r = cmd({"cmd": "frame"})
print(f"[at] frame {r.get('frame')}")
for a in (0x24, 0x25, 0x26, 0xC5):
    r = cmd({"cmd": "follow_history", "addr": f"{a:04X}", "limit": 120})
    print(f"\n===== ${a:02X} (total={r.get('total')}) =====")
    for e in r.get("entries", []):
        print(f"  frame={e.get('f'):>6}  {e.get('old')}->{e.get('new')}  stack={e.get('stack')}")
