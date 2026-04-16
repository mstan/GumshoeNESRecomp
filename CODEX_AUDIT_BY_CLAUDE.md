# Claude Audit of Codex's Gumshoe Debugging

Date: 2026-04-14
Subject: Verifying and extending Codex's findings in `CODEX_SUMMARY.md`. The scope of this document is findings, not fixes.

## TL;DR

The recompiled disassembly is sound. The bug is in the **PPU runner's sprite-0-hit emulation during zapper blackout frames**, not in the translated 6502 code. Every byte of 6502 logic from zapper poll through selector acceptance is correct. The hit-detection loop at `$C627-$C635` polls `$2002 bit 6` (sprite-0-hit) and times out because the runner's PPU never asserts that bit when sprite 0 overlaps the background during a blackout frame.

## What Codex Got Right

- Zapper trigger registers: `$C5`, `$C7` pulse correctly.
- Blackout frame occurs; palette switches; `spr0_active` becomes true on shot frames.
- Gumshoe/bottle highlight rectangle does not appear in OAM.
- State machine bytes (`$24`, `$25`, `$26`, `$2C`) never advance past the pre-hit values.
- Bank-4 spawner `func_95E9_b4` signature (takes `X` as b0 value to spawn, find-free-slot allocator, zeroes b1/b2/b4/b5, loads b8 from zp `$0D`).
- Acceptance criteria for the selector at `$C6FB_b1` (b0!=0, b7&1!=0, b7&0x80==0, b2!=0).
- Shootable b7 values in code include `0x11`, `0x19`, `0x9F`.
- Ghidra MCP is not usable from the automated harness; manual Ghidra operation was required for this audit.

## What Codex Got Wrong

### 1. "The shootable helper is `b0=0x06`, `0x0A`, or `0x12`"

Wrong. Mesen oracle capture (CSV of 2441 frames including a confirmed successful shot at f1034) shows the shootable target is **`b0=0x01`, b7=0x1F, b2=0x01**. `b0=0x12` is a *different* auxiliary actor in slot 4/8 whose `b7` is `0x84` — not shootable.

User clarified that `b0=0x01` is in fact the **thrown bottle object**, not a Gumshoe peek-out entity. Gumshoe's own on-screen character uses a different mechanism entirely.

### 2. "`$64` stays at sentinel = selector fails"

Wrong. `$64` is a general **entity-iteration scratch variable**, used by many functions (`func_9495_b0`, `func_9452_b0`, `func_91E2_b0`, etc.). Our recomp showed **1608 transitions** on `$64`, including the expected `F4 → 0C → 18 → 24 → ...` stride-12 walks. Sampling `$64` at an arbitrary frame just catches whatever value the iterator parked at. The `F4/FC` sentinel Codex observed is simply what `$64` reverts to when the entity-walk hits end-of-table — not a selector failure signal.

### 3. "The failed gameplay shot path never enters the blackout/selector path"

Misleading. The code path `func_C57F_b1 → func_828E_b0 → func_8087_b0` is the correct gameplay-tick path every frame. The zapper polling, blackout, and hit-test body **do all run** — this Claude session verified via follower instrumentation that `func_C67F_b1` (hit-test cleanup that clears `$C5`) runs on **every** shot frame (104 times in our capture).

### 4. "`$53` alt-path is the peek-out/spawn trigger"

Partially wrong. `$53` pulses correctly (`00 → 01 → 00` every ~17 frames via `func_8900_b0`), and the alt path at `func_8435_b0 → func_84C7_b0 → func_A00C_b0 → func_A086_b0 → func_922B_b0` DOES reach entity code — but that chain is the **thug's bottle-throw animation scripter**, not Gumshoe's peek-out. The actual `b0=0x01` entity (bottle) gets written by that chain, confirmed by our follower on `$0624` (slot 3 b0) capturing a `0x00 → 0x01` write at frame 6414 via exactly this stack.

## New Findings From This Session

### Full zapper pipeline (bank 1)

```
func_C57F_b1   ($C57F-$C5BB)   Poll $4017 trigger, pulse $C5/$C7
func_C5BC_b1   ($C5BC-$C6AB)   Validate + inlined hit-test body (MERGED)
├─ $C5FC       fall-through hit-test body entry
├─ $C627-$C635 PPU sprite-0-hit poll loop (← exits here on failure)
├─ $C640       JSR func_C6FB_b1 (selector)
├─ $C675       STA #$02 to slot's b2 on hit
└─ $C67F       cleanup (clear $C5)
func_C6FB_b1   ($C6FB-$C758)   Entity-table selector
```

Two copies of the hit-test body exist in generated code:
- Inlined inside `func_C5BC_b1` (reached via fall-through from `$C5F5`)
- Separate `func_C5FC_b1` (reached via trampoline from `$C5D4` when `$24 != 0`)

Both copies behave identically. **This is not a split-function bug.**

### Validation gates at shot time — all pass

Captured at live shot frames (42110, 42120, 42132):

| Byte | Value | Gate at `$C5BC_b1` | Result |
|---|---|---|---|
| `$C5` | 0x01 | `BEQ exit` | pass |
| `$4F` | 0x00 | `AND #$F; CMP #1; BEQ exit` | pass |
| `$24` | 0x00 | branch to path2 | pass |
| `$8B` | 0x00 | `BNE skip_53_check` | goes to `$53` check |
| `$53` | 0x00 | `BNE exit` | pass |
| `$4E` | 0x00 | path to `$C5ED` | pass |
| `$84` | 0x00 | `CMP #$0B; BEQ exit` | pass |
| `$C9` | 0x01 | `BNE $C5FC` | **proceeds to hit test** |

`$D3=5, $D2=1` (entity count non-zero) — selector should run.

### The smoking gun — `$0626` slot 3 b2 never written

With slot 3 containing the valid shootable bottle (`b0=0x01, b2=0x01, b7=0x1F`) for thousands of frames, and with confirmed shot pulses on it:

- `$0626` follower: **0 transitions across 96k+ frames**
- `$CD` follower: no writes from hit-test path (only from unrelated `func_88BD_b0` state chain)

The write at `$C679` (`LDA #$02; STA $0602,X` after selector accept) **never fires**. That can only be true if the hit-test loop exits via `$C635` (`JMP $C67F`) on every attempt.

### Why the loop exits at `$C635`

```
$C627: LDA $2002
$C62A: AND #$40       ; isolate sprite-0-hit flag
$C62C: BNE $C638      ; if set → enter processing (selector)
$C62E: LDA $4017
$C631: AND #$08       ; zapper light-sensor
$C633: BNE $C627      ; if still sees light, keep polling
$C635: JMP $C67F      ; timeout: light disappeared before sprite 0 hit
```

The loop is: "wait until either PPU sprite-0-hit fires (success) OR the zapper stops seeing light (timeout/miss)." Our runner reaches the timeout branch every time, which means **PPU `$2002 bit 6` (sprite-0-hit) is never being asserted** during Gumshoe's zapper blackout frame.

## Root Cause

**PPU sprite-0-hit emulation is incorrect for Gumshoe's zapper blackout pattern.**

On real hardware:
1. Game enters blackout frame: all-black BG except a white highlight rect rendered over the target.
2. Game draws sprite 0 at the highlight rect coordinates.
3. PPU sets `$2002 bit 6` when sprite 0's rendered pixel overlaps a non-transparent BG pixel.
4. Game reads `$2002 & $40` → sees bit 6 set → enters selector → marks hit.

In our runner:
- `spr0_active` becomes true (per Codex).
- Palette switches to blackout pattern (per Codex).
- But `$2002 bit 6` is never asserted at a cycle where the game reads it.

The prior fix documented in memory (`project_gumshoe_gxrom_boot.md` — "removed spr0 gate + added OAM stale re-render, Duck Hunt verified") is insufficient for Gumshoe. Duck Hunt uses sprite-0-hit in a different, less-timing-sensitive way.

## What to Stop Investigating

- Dispatch misses (verified zero; logger wiring validated in `runtime.c:994`).
- Split functions in the `func_828E_b0` chain (audited all 6 direct children byte-for-byte against Ghidra — clean).
- Selector logic (`func_C6FB_b1`) — identical to original, would accept slot 3 if reached.
- Peek-out spawn logic — works correctly (confirmed via slot 3 spawn at f6414).
- `$53` pulse / alt-path machinery — fires correctly.
- Validation gates in `func_C5BC_b1` — all pass at shot time.
- Backward tail calls / merge_func candidates — not relevant to this bug.

## Where to Investigate Next

`nesrecomp/runner/src/ppu_renderer.c` (or wherever sprite-0-hit is asserted in the runner):

1. Find the code that sets `$2002 bit 6` (typically on each frame's render pass).
2. Verify it fires during the zapper blackout frame specifically — palette `3F00..3F03 = 0F 0F 0F 0F`, `3F11..3F13 = 30 30 30`.
3. Verify the cycle-timing: when the game reads `$2002` at `$C627`, the flag must already be set for that frame's sprite-0 overlap.
4. Cross-reference Gumshoe's OAM at a blackout frame — sprite 0 is likely the highlight rectangle's top-left tile, at the same coords as the target's b8/b9.
5. Compare runner behavior against Mesen's behavior on the same capture frame (we have a CSV + Lua dumper ready at `mesen-emulator/gumshoe_dump.lua`).

## Reproduction Assets

- Mesen oracle: `F:/Projects/nesrecomp-release/mesen-emulator/` with `gumshoe_dump.lua` (dumps zero-page + full entity table per frame, flags `BECAME SHOOTABLE` transitions).
- Oracle CSV showing successful hit at f1034: `C:/Users/Matthew/Documents/Mesen2/gumshoe_dump.csv`.
- Recomp TCP followers (runtime-added, not persisted): `$0626`, `$0624`, `$0624`, `$0032` via `add_slot3b2.py`.
- Analysis scripts in repo root: `analyze_mesen.py`, `analyze_hit.py`, `analyze_53.py`, `check_gates.py`, `pull_slot3b2.py`.

## Frames of Interest

- f1033-1034 (Mesen): bottle spawn + activation; b7 `10 → 1F` transition (BECAME SHOOTABLE).
- f534 (recomp): only captured `$D4` transition, misleadingly suggesting hit-test body ran once (actually runs on every shot, but `$D4=$D3=0x05` stable so writes don't transition).
- f6414 (recomp): slot 3 `b0 = 0x00 → 0x01` written via `func_922B_b0 ← func_A086_b0 ← func_A00C_b0 ← func_84C7_b0 ← func_8435_b0 ← func_828E_b0 ← func_8087_b0`.
- f42110-42132 (recomp): example shot frames with all validation gates passing yet `$0626` unchanged.
