# GumshoeNESRecomp — Open Issues

## ISSUE-001: Missing HUD elements (timer + shot counter)
**Status:** Open
**Priority:** P1 (cosmetic but obvious)
**First observed:** 2026-05-04, after the GxROM dispatch + NMI vector fix
landed and the game became playable past the title screen.

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
