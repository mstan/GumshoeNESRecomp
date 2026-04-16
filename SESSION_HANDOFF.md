# Session Handoff — Gumshoe Hit Detection (2026-04-13 → 2026-04-14)

## Mantra
**Oracle-first. One divergence. No speculation.**

## Where we ended

Hit detection still doesn't register in Gumshoe. Game runs cleanly, no regressions
from the cadence fix or the `origin/master` merge. The underlying bug is an
**architectural deadlock in the game's state machine**, not a codegen fault we've
found yet.

## What was fixed this session

1. **Framework cadence bug** (`nesrecomp`, commit `1d05f3e` on `feature/m66`):
   `maybe_trigger_vblank` used to silently consume frame budgets during PPU-off
   windows (NMI disabled) by gating `nes_vblank_callback` on PPUCTRL bit 7. The
   consequence was that native's main loop could accumulate 3–5 frame-budgets
   of game-state progression inside a single perceived frame, making the mode
   state machine race ahead of real-NES cadence.

   Fix: `nes_vblank_callback` fires every budget exhaustion regardless of NMI
   state; `game_run_nmi` is called every wall-clock frame; `func_NMI` gating
   moves to verify_mode; added 1.5×-budget safety cap on deferred VBlank in
   case no backward-branch arrives. See commit message for details.

2. **Merged `origin/master` into `feature/m66`** (commit `2b5817c`): brings in
   MegaMan3 MMC3 / PHA-PHA-JMP(ind) / all-CPU-regs-across-NMI work. Conflicts
   resolved in merge commit body. No regressions.

3. **`$D920` misidentification corrected.** The saved-memory note
   `"$D920 response handler next"` was wrong. `$D920` is a `WaitForVBlank`
   utility. The real hit qualifier is `func_98D2_b6` (inline inside
   `func_808C_b6`'s `$80BF` path). Memory file updated.

## The outstanding bug — deadlock after first hit

Fully traced via TCP `follow` on `$24/$25/$26/$C5`. After the FIRST successful hit:

- `$24` clears to 0 and **stays 0 permanently** — no code path re-arms it.
- `$25` enters a closed cycle `$12→$01→$12→$02→$03→$04→$07→$12→$10→$12→…`
  that never reaches `$0C` (would call `func_861A_b6` → `$24=1`) or `$00`
  (would call `func_80EA_b6` → `$25=$11` hit-listen).
- Both hit-detection readers are dead: `$80CC` needs `$24=1`; `func_8132_b6`
  needs `$25=$11`.
- Bank-0 `func_C57F_b1` keeps setting `$C5=1` on every trigger pull; `func_C67F_b1`
  clears it next frame. 40+ cycles observed during the user's playtest.
  Zero readers consume.

Full map in `memory/project_zapper_detection_chain.md`.

## Three hypotheses for the missing re-arm

1. **`$883E` reset routine in the recomp should also set `$24=1`**, but it
   only writes zeros. If the original ROM's `$883E` sets `$24=1` somewhere
   and our generated version doesn't, that's a codegen bug — possibly an
   unrecognized PHA/RTS pattern terminating the function early.

2. **Bank-0 code should write `$25=$0C`** after a balloon is destroyed. We
   haven't located this path. Candidates to audit: `func_894A_b0`, `func_D72F_b1`,
   `func_D726_b1` — all seen writing `$25` to other values. None seen writing
   `$0C`.

3. **A bank-6 function is supposed to transition `$25 → $0C` post-`$AB56`**
   but doesn't, due to codegen mis-handling its control flow.

## Next steps for the next session

### Before anything
Read `memory/project_zapper_detection_chain.md` in full — it has the complete
dispatch map, observed state transitions with frame numbers, and the TCP-script
reproduction recipe.

### Then pick one of:

**A.** Grep the raw ROM bank bytes for `A9 0C 85 25` (LDA #$0C / STA $25)
and `A9 01 85 24` (LDA #$01 / STA $24). For each hit, check whether the
containing function is reachable in our dispatch graph and whether our
generated code emits the same logic. If a write exists in the ROM but is
unreachable in our generated code, that's the missing path.

**B.** Use Ghidra to disassemble `func_883E_b6` ($883E in bank 6) and compare
against `generated/gumshoe_full.c:82791`. If the original has a `STA $24` our
generated version dropped, that's the fix.

**C.** Force `$24=1` via TCP `write_ram` during live gameplay to verify the
theory: if re-arming `$24` restores multi-hit detection, the bug is isolated.
Then dig for the missing re-arm write.

### Build / branch state

**`nesrecomp/feature/m66`**:
- Tip: `2b5817c` (merge of origin/master)
- `1d05f3e` cadence fix; `2b5817c` merge. NOT pushed to origin.
- Backups: `backup/2026-04-13` (pre-session), `backup/pre-master-merge`.

**`GumshoeNESRecomp/master`**:
- 2 new commits this session (verify_mode + TCP scripts). NOT pushed.
- Backup: `backup/2026-04-13`.

### Key files

- `verify_mode.c` — extended with `$24` logging; `mode_trace.csv` now has
  `nat_24`/`emu_24` columns alongside `$25/$26/$2C`.
- `tcp_follow_*.py` in repo root — all working against port 4372.
- `tcp_pause_then_follow.py` — the race-win strategy (pause ASAP, install,
  continue). **Use non-turbo mode** (`verify_slow.txt` script) so TCP comes
  up before divergence at frame 1289.
- `build/Release/debug.ini` — TCP auto-enable marker, keep present.

### Reproducing the deadlock

```
cd F:/Projects/nesrecomp-release/GumshoeNESRecomp/build/Release
./GumshoeRecomp.exe "../../Gumshoe (USA, Europe).nes" --script verify_slow.txt

# In another terminal:
cd F:/Projects/nesrecomp-release/GumshoeNESRecomp
python tcp_pause_then_follow.py
```
Pauses early, installs follows on `$24/$25/$26/$C5`, resumes, dumps
`follow_history` once past frame 1500.

### What DOES work
- One hit registers via the `$24=1 → $98D2` path on the first fire (confirmed
  at frame 1743 in a non-turbo verify run).
- Player no longer dies on trigger (solved in a previous session).
- No dispatch misses. No regressions from cadence fix or merge.
- Duck Hunt verified unaffected (DH never exceeds 1× budget so the cap we
  added never fires).

### What DOESN'T work
- Hit detection after the first hit.
- No visible flash on targets during trigger press after the first hit
  (because `$98D2`, which does the PPU-off detection flash via the `$80BF`
  path, stops running once `$24=0`).

## Caveats

- **"Working hit" at frame 1743 was verified via code inspection of
  `func_98D2_b6`** (reads `$4017 & 0x10` zapper trigger, sets `$C5=1` on qualified
  hit). That's zapper logic, not collision logic. But I did NOT visually
  confirm a balloon popped at frame 1743. To be 100% sure, save-state before/after
  and diff OAM staging (`$600+`) or entity state. The user raised this —
  acknowledge it as an uncertainty.

- **Turbo mode can produce different `$26` trajectories** than non-turbo.
  Use non-turbo (`verify_slow.txt`) for consistent traces. The "frame 1289
  `$26 03→04` divergence" I initially reported was a turbo-mode artifact;
  in non-turbo, `$26` never reaches 4 at all.

## Skill state

`/recomp-debug` was invoked. Phase 2 (characterize failure) done. Phase 3
classification: NOT Class 1 (dispatch), NOT Class 2 (missing function),
**possibly Class 3** (unrecognized control flow in whatever writes `$24=1`
or `$25=$0C`), unlikely Class 4 or 5.

Everything committed. Nothing pushed. Rollback points intact.
