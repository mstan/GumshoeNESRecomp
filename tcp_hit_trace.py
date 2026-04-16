#!/usr/bin/env python3
"""
tcp_hit_trace.py — capture the full $24/$25/$C5/$2C story around a zapper fire.

Usage:
  1. Launch GumshoeRecomp.exe (debug.ini already present → TCP auto-on).
  2. Press START, reach gameplay.
  3. Run this script:    python tcp_hit_trace.py
  4. Follow prompts: fire at a target, press Enter.

Output:
  - follow_history for $24, $25, $C5, $2C: every write + calling function
  - frame_timeseries for the same addresses around the fire window
  - summary of which dispatch path consumed $C5
"""
import socket, json, sys, time

PORT = 4372                         # Gumshoe native port
WATCHES = {0x24: "$24 ($24 dispatch)",
           0x25: "$25 (mode)",
           0xC5: "$C5 (detection counter)",
           0x2C: "$2C (mode timer)"}


def cmd(c):
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(5)
    s.connect(("127.0.0.1", PORT))
    s.sendall((json.dumps(c) + "\n").encode())
    buf = b""
    while b"\n" not in buf:
        chunk = s.recv(8192)
        if not chunk:
            break
        buf += chunk
    s.close()
    return json.loads(buf.decode().strip())


def current_frame():
    r = cmd({"cmd": "frame"})
    return r.get("frame", 0)


def main():
    # Liveness
    try:
        r = cmd({"cmd": "ping"})
    except Exception as e:
        print(f"TCP server not responding on :{PORT}. Is the game running?")
        print(f"  error: {e}")
        sys.exit(1)
    print(f"[ok] TCP alive. {r}")

    # Baseline
    f0 = current_frame()
    print(f"[baseline] frame={f0}")
    print("[baseline] RAM:", end=" ")
    for addr, label in WATCHES.items():
        r = cmd({"cmd": "read_ram", "addr": f"{addr:04X}", "len": 1})
        val = r.get("data", ["??"])[0] if r.get("ok") else "??"
        print(f"${addr:02X}={val}", end=" ")
    print()

    # Install follows
    for addr, label in WATCHES.items():
        r = cmd({"cmd": "follow", "addr": f"{addr:04X}"})
        ok = r.get("ok", False)
        print(f"[follow] {label:30s} {'ok' if ok else 'FAIL: ' + str(r)}")

    print()
    print("-" * 60)
    print("NOW: aim at a target and click to fire.")
    print("     After firing, press Enter here to capture.")
    print("-" * 60)
    input()

    f1 = current_frame()
    print(f"[captured] frame={f1} (window: {f0}..{f1})")

    # Write history per address
    print()
    for addr, label in WATCHES.items():
        r = cmd({"cmd": "follow_history", "addr": f"{addr:04X}", "limit": 40})
        print(f"\n===== {label} =====")
        if not r.get("ok"):
            print(f"  ERR: {r}")
            continue
        events = r.get("events", r.get("history", []))
        if not events:
            print("  (no writes observed)")
            continue
        for e in events:
            # Common fields: frame, val (or value), fn (function name), pc
            fr = e.get("frame", "?")
            v = e.get("val", e.get("value", "?"))
            fn = e.get("fn", e.get("function", "?"))
            pc = e.get("pc", "?")
            print(f"  frame={fr:>6}  val={v}  fn={fn}  pc={pc}")

    # Timeseries in the fire window (last ~120 frames before capture)
    start = max(0, f1 - 120)
    print()
    print(f"\n===== timeseries frames {start}..{f1} =====")
    r = cmd({"cmd": "frame_timeseries",
             "start": start, "end": f1,
             "fields": ["ram:24", "ram:25", "ram:C5", "ram:2C"]})
    if r.get("ok"):
        # format depends on implementation — just dump compactly
        series = r.get("series", r)
        print(json.dumps(series, indent=2)[:4000])
    else:
        print(f"  frame_timeseries not supported or errored: {r}")

    print()
    print("=" * 60)
    print("Interpretation hints:")
    print("  - If $C5 was written 00→01 then 01→00 by func_808C_b6 area ($80CC/$80D2):")
    print("      the $24-dispatch path consumed it → went to $883E reset.")
    print("  - If $C5 was cleared by func_8132_b6 ($813F):")
    print("      the $25=$11 path consumed it → should have called func_AB56_b6.")
    print("  - If $25 transitioned to $12 at any point: hit was properly processed.")
    print("  - If no such transition: trace which dispatch ran and why the outcome was wrong.")


if __name__ == "__main__":
    main()
