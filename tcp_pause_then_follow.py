#!/usr/bin/env python3
"""Launch-race strategy:
  1. Poll TCP until up
  2. Immediately send `pause`
  3. Install follows on $24/$25/$26/$C5
  4. `continue` — let script run
  5. Poll frame until > 1400 (divergence was at 1289)
  6. Dump follow_history
"""
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


# Busy-wait for TCP up, then pause ASAP
for _ in range(200):
    try:
        r = cmd({"cmd": "pause"})
        print(f"[paused] {r}")
        break
    except Exception:
        time.sleep(0.01)
else:
    print("TCP never up")
    raise SystemExit(1)

r = cmd({"cmd": "frame"})
print(f"[at] frame {r.get('frame')}")

for a in (0x24, 0x25, 0x26, 0xC5):
    r = cmd({"cmd": "follow", "addr": f"{a:04X}"})
    print(f"follow ${a:02X}: {r.get('ok')} slot={r.get('slot')}")

cmd({"cmd": "continue"})
print("[resumed]")

target = 1500
while True:
    r = cmd({"cmd": "frame"})
    f = r.get("frame", 0)
    if f >= target:
        print(f"[reached] frame {f}")
        break
    time.sleep(0.2)

for a in (0x24, 0x25, 0x26, 0xC5):
    r = cmd({"cmd": "follow_history", "addr": f"{a:04X}", "limit": 60})
    print(f"\n===== ${a:02X} (total={r.get('total')}) =====")
    for e in r.get("entries", []):
        print(f"  frame={e.get('f'):>6}  {e.get('old')}->{e.get('new')}  stack={e.get('stack')}")
