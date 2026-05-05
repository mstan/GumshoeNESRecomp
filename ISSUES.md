# GumshoeNESRecomp — Open Issues

## ISSUE-001: Missing HUD elements (timer + shot counter)
**Status:** Resolved 2026-05-04
**Priority:** P1 (cosmetic but obvious)
**First observed:** 2026-05-04, after the GxROM dispatch + NMI vector fix
landed and the game became playable past the title screen.

### Resolution
GxROM static-JSR bank-pairing bug in
`nesrecomp/recompiler/src/code_generator.c:1127`.  For JSR targets at
`$C000+`, the recompiler unconditionally passed `fixed_bank` as the
source bank to `emit_call_target`, which then went through
`format_func_name`'s "fixed-bank rule" (`bank == fixed_bank && addr >= $C000`
strips the `_b` suffix) and emitted `func_<addr>()` resolving to the
last bank's body.

For traditional fixed-bank mappers (UxROM/MMC1/MMC3/NROM) this is
correct — `$C000+` *is* always served by the fixed bank.  For GxROM
(mapper 66, full 32K window switch) both halves switch together and
`$C000+` is the source's `bank | 1`, NOT a fixed bank.  Mirrors the
`call_by_address` GxROM pairing fix in 49f9fe3.

The Gumshoe symptom was that `bank00:$82B5: JSR $D158` (the master
per-frame render dispatcher's HUD-render call) emitted `func_D158()`
which resolved to bank 7's body at $D158 — a button-state EOR diff
loop — instead of bank 1's HUD render at $D158 (`JSR $D27A; JSR $D2B5;
LDY #$10; LDA $ED; ...` writing timer/shot-counter sprites to OAM
slots 54-63 at $02D8-$02FF).  HUD render simply never ran.

The bank-1 HUD render writes 6 sprites for the timer (slots 54-59,
X = $20-$48 with sprites 54+55 force-hidden as anchor placeholders
and 56-59 as "M:SS" digits + colon at Y = $10) and 4 sprites for the
shot counter (slots 60-63, X = $30-$48 at Y = $18, "=NNN" pattern).

### Fix
Gate the `fixed_bank` lookup behind `rom_mapper_full_32k_switch(rom)`:
GxROM uses the source bank (`gxrom_paired_bank` resolves to `bank | 1`
for `$C000+`); other mappers keep the fixed_bank lookup unchanged.
Also force `force_dynamic` for SRAM-sourced GxROM JSRs since the
source bank is unknown at compile time there.

### Validation
- Visual: gameplay screenshot shows "5:57" timer + "=050" shot counter
  + portrait icon at top-left.  Sprites 54-63 raw OAM Y stable at
  $10/$18 across 15 frame samples (was $F4 hidden before).
- Smoke matrix: regenerated Faxanadu, Zelda, Metroid, Yoshi, Duck Hunt;
  `*_full.c` SHA-256 byte-identical pre/post-fix (none are mapper 66,
  so the fix's `else` branch produces the original behavior).
- `dispatch_miss_info` reports zero misses during gameplay.

### Observed behavior
- The top-left HUD is absent during gameplay:
  - Timer display (`5:44` style countdown clock with the small clock icon)
  - Shot counter (`=097` with the bullet icon)
- Gameplay itself works correctly — Mr. Stevenson, balloons, level
  geometry, scoring, and Zapper input all function.
- Side-by-side comparison vs YouTube playthrough confirms only the
  upper-left HUD region is missing.  The portrait icon (top-right
  corner) was reported by the player as also missing in their build,
  though this was not verified at investigation time.

### What's been ruled out
- ❌ Not a dispatch-miss bug.  After the `gumshoe/init-flag-investigation`
  branch fixes (07d1130), `dispatch_miss_info` reports zero misses
  during gameplay.  The recompiler-side dispatch is fully correct.
- ❌ Not an inline-dispatch table truncation.  The 24-entry $D476 table
  at $80A4 (and similar tables at $C768, $D6AD, $A89D) are now
  emitted at full size after the GxROM-aware dispatch-table size
  scan fix.  No `INLINE MISS @$XXXX` log lines during gameplay.
- ❌ Not a sprite-DMA / runner bug.  Shadow OAM at `$0200-$024F` and
  internal `g_ppu_oam` agree byte-for-byte (verified via TCP debug
  server `read_ram` + `read_oam`).  Both contain only gameplay
  sprites (Mr. Stevenson, balloons, ground objects) at Y=64-216.
  No HUD-region sprites at Y=8-32 in either.
- ❌ Not a CHR / palette / nametable issue.  All three are populated
  with real, varying content during gameplay.

### Diagnosis
The HUD-render code path **isn't executing**.  Sprites for the
timer / shot-counter / portrait are never written to shadow OAM
in the first place.  This is a game-state issue: some flag or
state-machine condition that gates the HUD-render routine is not
being met.  Recompiler / runner are doing their job correctly.

### Next investigation steps
- Connect Ghidra to the Gumshoe ROM (per Rule 0 in nesrecomp/CLAUDE.md).
- Find the HUD-render routine (likely a JSR somewhere in the NMI
  handler chain that writes digit tiles to shadow OAM at low Y).
- Trace back to the gating flag and find why it's never set during
  gameplay.  Suspect candidates: a "title-screen-done" or
  "level-init-complete" flag that the recomp leaves unset because
  some prior code path (probably state $25 transition handler in
  the $D476-dispatched code) doesn't fully execute.
- Compare RAM byte-by-byte against a Mesen-2 oracle running the
  same ROM at the same frame to find the disagreeing flag.

### References
- TCP debug server commands used: `dispatch_miss_info`,
  `read_palette`, `read_oam`, `read_ram` (for shadow OAM at $0200),
  `dump_nametables`, `memory_diff`.  See the global memory note
  `feedback_query_debug_server_first.md` for the playbook.
