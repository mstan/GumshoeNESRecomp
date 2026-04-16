#!/usr/bin/env python3
"""Retroactive hit-detection trace — pulls the past ~60 seconds of $24/$25/$C5/$2C
from the ring buffer and finds $C5 transitions."""
import socket, json, sys

PORT = 4372
WATCHES = [0x24, 0x25, 0xC5, 0x2C]


def cmd(c):
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(10)
    s.connect(("127.0.0.1", PORT))
    s.sendall((json.dumps(c) + "\n").encode())
    buf = b""
    while b"\n" not in buf:
        chunk = s.recv(65536)
        if not chunk:
            break
        buf += chunk
    s.close()
    return json.loads(buf.decode().strip())


def main():
    try:
        r = cmd({"cmd": "ping"})
        print(f"[ok] {r}")
    except Exception as e:
        print(f"TCP dead on :{PORT}: {e}")
        sys.exit(1)

    f_now = cmd({"cmd": "frame"}).get("frame", 0)
    start = max(0, f_now - 2000)   # last ~33s of play
    print(f"[query] frame_timeseries {start}..{f_now} in 200-frame chunks")

    # Server caps at 200 frames per request — chunk it
    merged = None
    a_start = start
    while a_start < f_now:
        a_end = min(a_start + 199, f_now)
        r = cmd({"cmd": "frame_timeseries",
                 "start": a_start, "end": a_end,
                 "fields": [f"ram:{a:02X}" for a in WATCHES]})
        if not r.get("ok"):
            print(f"[err] chunk {a_start}..{a_end} failed: {r}")
            sys.exit(2)
        chunk = r.get("series", r.get("data", r))
        if merged is None:
            merged = {}
            if isinstance(chunk, dict):
                for k, v in chunk.items():
                    if isinstance(v, list):
                        merged[k] = list(v)
                    else:
                        merged[k] = v
            else:
                merged = {"series": list(chunk) if isinstance(chunk, list) else []}
        else:
            if isinstance(chunk, dict):
                for k, v in chunk.items():
                    if isinstance(v, list) and k in merged and isinstance(merged[k], list):
                        merged[k].extend(v)
            elif isinstance(chunk, list):
                merged.setdefault("series", []).extend(chunk)
        a_start = a_end + 1

    r = {"ok": True, "series": merged}

    # Shape of reply varies — print the structure, then parse
    series = r.get("series", r.get("data", r))
    # Try to extract per-address lists
    # Common shape: {"frames":[...], "ram:24":[...], "ram:25":[...]} or
    # {"series":[{"frame":N, "ram:24":v, ...}, ...]}
    by_addr = {a: [] for a in WATCHES}
    frames = []

    if isinstance(series, dict) and "frames" in series:
        frames = series["frames"]
        for a in WATCHES:
            by_addr[a] = series.get(f"ram:{a:02X}", []) or series.get(f"ram:{a:02x}", [])
    elif isinstance(series, list):
        for entry in series:
            frames.append(entry.get("frame"))
            for a in WATCHES:
                v = entry.get(f"ram:{a:02X}", entry.get(f"ram:{a:02x}"))
                by_addr[a].append(v)
    else:
        print("[warn] unexpected shape; raw dump follows")
        print(json.dumps(r, indent=2)[:3000])
        return

    if not frames or not by_addr[0xC5]:
        print("[err] no data in series")
        print(json.dumps(r, indent=2)[:2000])
        return

    print(f"[series] {len(frames)} frames loaded")

    # Find $C5 transitions (0 → non-zero and non-zero → 0)
    def to_int(v):
        if v is None:
            return None
        if isinstance(v, int):
            return v
        if isinstance(v, str):
            try:
                return int(v, 16)
            except ValueError:
                return None
        return None

    c5 = [to_int(v) for v in by_addr[0xC5]]
    d24 = [to_int(v) for v in by_addr[0x24]]
    d25 = [to_int(v) for v in by_addr[0x25]]
    d2c = [to_int(v) for v in by_addr[0x2C]]

    transitions = []
    for i in range(1, len(c5)):
        if c5[i] != c5[i - 1] and c5[i] is not None and c5[i - 1] is not None:
            transitions.append(i)

    print(f"\n[$C5 transitions] found {len(transitions)} in window")
    for idx in transitions[:30]:
        fr = frames[idx]
        print(f"  frame={fr:>6}  $C5: {c5[idx-1]:02X} -> {c5[idx]:02X}   "
              f"$24={d24[idx]:02X} $25={d25[idx]:02X} $2C={d2c[idx]:02X}")

    # Also find $25 transitions (mode changes — especially any to $12)
    print(f"\n[$25 transitions]")
    count = 0
    for i in range(1, len(d25)):
        if d25[i] != d25[i - 1] and d25[i] is not None:
            fr = frames[i]
            arrow = " <-- HIT!" if d25[i] == 0x12 else ""
            print(f"  frame={fr:>6}  $25: {d25[i-1]:02X} -> {d25[i]:02X}"
                  f"  $24={d24[i]:02X}{arrow}")
            count += 1
            if count > 40:
                print(f"  ... ({len(d25)} more)")
                break

    # Snapshot around each $C5 transition: show 3 frames before and after
    print(f"\n[windowed context around $C5 transitions]")
    for idx in transitions[:6]:
        fr = frames[idx]
        print(f"\n  -- frame {fr} --")
        lo = max(0, idx - 3)
        hi = min(len(frames), idx + 4)
        print(f"     {'frame':>6}  $24 $25 $C5 $2C")
        for j in range(lo, hi):
            mark = " <<" if j == idx else ""
            print(f"     {frames[j]:>6}   {d24[j]:02X}  {d25[j]:02X}  {c5[j]:02X}  {d2c[j]:02X}{mark}")

    print()
    print("Look for:")
    print("  - $C5: 00 -> 01 (detection fired)")
    print("  - $C5: 01 -> 00 (consumed) — note $24 at that frame")
    print("       $24 != 0 → consumed by the $80CC path ($883E reset)")
    print("       $24 == 0 AND $25 == 11 → consumed by func_8132_b6 (should call AB56)")
    print("  - $25: 11 -> 12 means HIT was properly processed by func_AB56")
    print("  - No $25 -> 12 despite $C5 consumption = hit was eaten by wrong path")


if __name__ == "__main__":
    main()
