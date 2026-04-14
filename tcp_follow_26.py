#!/usr/bin/env python3
"""Install follow on $26, wait for script to run past frame 1289, dump history."""
import socket, json, time


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


# wait for TCP server to come up
for i in range(30):
    try:
        r = cmd({"cmd": "ping"})
        print(f"[ok] {r}")
        break
    except Exception:
        time.sleep(0.2)
else:
    print("TCP never came up")
    raise SystemExit(1)

for a in (0x24, 0x25, 0x26, 0xC5):
    r = cmd({"cmd": "follow", "addr": f"{a:04X}"})
    print(f"follow ${a:02X}: slot {r.get('slot')}")

# Wait until game passes frame 1400 (divergence is at 1289)
while True:
    r = cmd({"cmd": "frame"})
    f = r.get("frame", 0)
    if f > 1400:
        print(f"[reached] frame {f}")
        break
    time.sleep(0.1)

for a in (0x24, 0x25, 0x26, 0xC5):
    r = cmd({"cmd": "follow_history", "addr": f"{a:04X}", "limit": 80})
    print(f"\n===== ${a:02X} =====")
    for e in r.get("entries", []):
        f = e.get("f"); old = e.get("old"); new = e.get("new"); stack = e.get("stack")
        print(f"  frame={f:>6}  {old}->{new}  stack={stack}")
