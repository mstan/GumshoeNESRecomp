# Gumshoe / Duck Hunt — title auto-advances with no input — FIXED

Status: **RESOLVED** (2026-06-29, branch `fix/zapper-games-menu`, commit
`6325b8c`). Root cause was a `$4017` Zapper **trigger-bit polarity** bug in
the shared runner — NOT recompiler function fragmentation, NOT recursion, and
NOT "the zapper is idle" (the earlier handoff misread the idle `$4017` value).

## Symptom

On the Mesen oracle (no input) Gumshoe holds on its title (an attract-mode
countdown `$2C` ticks `0x20`→`0x1f`→… and the title eventually self-advances
to a demo around f363). The recomp instead auto-advanced title → phase intro
→ gameplay within ~60 frames with no input.

## Root cause (measured, not theorized)

`runner/src/runtime.c` returned the `$4017` Zapper **trigger** bit (`$10`)
with **inverted polarity**:

```c
if (!g_zapper_trigger) val |= 0x10;   /* WRONG: bit set while NOT pulled */
```

The NES Zapper trigger is **active-high**: `$4017` bit 4 = 1 *while pulled*,
0 at idle (Nestopia `NstInpZapper.cpp:174`; confirmed against the Mesen RAM
oracle). The inversion reported a phantom trigger **press every idle frame**.

The Gumshoe/Duck Hunt light-gun engine polls the trigger in `func_98D2`
(`$98D2`): `LDA $4017; AND #$10; BNE …` → a "press" path that `INC`s the
debounce flag `$C5`. A nonzero `$C5` makes the master dispatcher `func_808C`
(`$808C`) take `$80CC: LDA $C5/ BNE $80E3`, which clears the **mode selector
`$24`** (`$80E5: STA $24` with A=0) and drops out of the `$26` attract
dispatch into the `$25` title state machine — which then auto-advances.

### How it was found (first-divergence RAM diff vs the Mesen oracle)

1. Per-frame WRAM-delta traces from recomp (`NESRECOMP_WRAM_TRACE`) and the
   Mesen oracle (`nesref` `NESREF_TRACE_FILE`), no input.
2. First clean divergence in a tracked state byte: recomp wrote `$25` =
   `0x11`/`0x12` (values the oracle never produces) starting f19.
3. New always-on env-gated **write-attribution tap** (`NESRECOMP_WRITE_WATCH`
   / `…_WATCH_FILE`, logs writer function + GxROM bank per watched RAM addr)
   walked the chain: `$25` mirrors `$1E` (via `func_AB4D`); the derail is
   gated by `$24`; `$24` is cleared by `func_808C` when `$C5 != 0`; `$C5` is
   set by `func_98D2` from the `$4017 & $10` trigger read.
4. The oracle's `$C5` stays 0 at idle ⇒ Mesen idle `$4017 & $10 == 0` ⇒ the
   recomp's idle `$10`-set was the bug.

## The fix

`runner/src/runtime.c`, `$4017` Zapper read:

```c
if (g_zapper_trigger) val |= 0x10;    /* active-high: set only when pulled */
```

Scoped inside `if (g_zapper_enabled)`, so only the two zapper games are
affected; the other seven `_acc` games are behaviorally unchanged (runner
change, no regen).

## Validation (vs Mesen RAM oracle, no input)

| var  | Gumshoe recomp (fixed) | Gumshoe oracle |
|------|------------------------|----------------|
| `$24`| `0x01`, held           | `0x01`, held   |
| `$1E`| `0x00`, held           | `0x00`, held   |
| `$25`| `0x00`→`0x02`@f361→`0x03`@f370 | `0x00`→`0x02`@f363→`0x03`@f375 |
| `$2C`| `0x20`@f6, −1 every 11f | `0x20`@f8, −1 every 11f |
| `$26`| `0x01`→`0x02`@f360→`0x03`@f361 | `0x01`→`0x02`@f362→`0x03`@f363 |

Frame-accurate within a constant ~2-frame priming offset. The recomp now
renders and **holds** the Gumshoe title; an actual scripted trigger pull sets
`$C5` and advances `$24`→0 (dual-validated: idle waits, real press fires).

**Duck Hunt** (stood up as the 2nd `_acc` zapper game, `_acc/DuckHunt`):
idle holds on the GAME A/B/C menu (`$24`/`$26` held, 0 dispatch misses),
a scripted trigger pull clears `$24` and advances — matching its Mesen
oracle. This **refutes** the prior `runtime.c` comment's claim that the
inverted polarity was needed for Duck Hunt (the auto-fire merely masked it).

Note: `nesref`+Mesen's *video* capture renders green for these games (a known
nesref quirk); the RAM oracle is valid and was used for all comparisons. The
recomp's own render is the visual ground truth (correct title art for both).

## Out of scope / still open

- Fire-hang (~1s on each shot) — owner-flagged, likely pre-existing, not
  investigated here.
- The `merge_range`/`span` function-fragmentation work (the earlier — wrong —
  root-cause theory) is shelved; it was never the cause.
