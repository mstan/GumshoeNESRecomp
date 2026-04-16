# Codex Debug Summary

Date: 2026-04-14
Project: `GumshoeNESRecomp`

This document contains only verifiable facts gathered during debugging. It does not propose a fix.

## Scope Inspected

- `GumshoeNESRecomp`
- nested `nesrecomp` checkout inside this repo
- `../DuckHunt` repo for comparison
- generated output in `generated/gumshoe_full.c`
- runner TCP/debug server
- local Ghidra project presence

## Repository Facts

- `nesrecomp.pin` in Gumshoe and Duck Hunt both pin the same commit: `56d85de`.
- The top-level build wiring between Gumshoe and Duck Hunt is broadly similar.
- Gumshoe's `game.toml` differs from Duck Hunt's:
  - Gumshoe has extra `inline_dispatch` sites.
  - Gumshoe has `merge_func` entries.
  - Gumshoe has additional manual bank-function seeds.
- Gumshoe's nested `nesrecomp` worktree is not pristine.
  - During inspection, `git -C nesrecomp status` showed dirty changes in the nested runner area.
- Gumshoe's local nested runner code differs from Duck Hunt's local nested runner code.

## Ghidra / MCP Facts

- A local Ghidra project exists under the repo.
- The configured Ghidra MCP bridge was not usable from this environment.
- MCP resource listing returned no usable resources.
- Direct probes to the expected local MCP/SSE endpoints returned generic HTML rather than consumable protocol data.

## Existing Gumshoe Hook Facts Before Instrumentation

- `extras.c` already contained zapper setup and a small `game_info` TCP extension.
- Before instrumentation:
  - `game_dispatch_override` was effectively unused.
  - `game_ram_read_hook` was effectively unused.
  - `game_fill_frame_record` was effectively a no-op.

## Debug Tooling Added

### Runner changes

In `nesrecomp/runner/include/debug_server.h` and `nesrecomp/runner/src/debug_server.c`:

- Added:
  - `const NESFrameRecord *debug_server_get_frame_record(uint32_t frame);`
- Increased follower capacity:
  - `MAX_FOLLOWERS` from `8` to `32`

### Gumshoe extras changes

In `extras.c`:

- Added packed per-frame `game_data` in `game_fill_frame_record`
- Added broader default follower setup in `game_on_init`
- Added decoded Gumshoe TCP commands:
  - `gumshoe_state`
  - `gumshoe_entities`
  - `gumshoe_entity`
  - `gumshoe_frame`
  - `gumshoe_window`
  - `gumshoe_oam`
  - `gumshoe_entities_frame`

### What `gumshoe_window` exposes

For a historical frame window, it now includes:

- key RAM bytes for gameplay/shot state
- shot-related bytes like `$C5`, `$C7`
- mode/state bytes including `$24`, `$25`, `$26`, `$2C`
- scratch/selection bytes:
  - `$00`
  - `$01`
  - `$63`
  - `$64`
- decoded selected entity if `$64` points into `0x0600`
- key PPU state
- palette bytes
- compact entity summaries

### What `gumshoe_oam` exposes

- Historical OAM decode for an exact frame, used to inspect whether highlight/rectangle sprites exist on the blackout frame.

### What `gumshoe_entities_frame` exposes

- Full entity-table dump for a historical frame.
- For each slot it reports:
  - `slot`
  - `base`
  - `active`
  - `b0`
  - `b1`
  - `b2`
  - `b6`
  - `b7`
  - `b8`
  - `b9`

## Verified Build / Runner Facts

- The project was rebuilt successfully multiple times in `Release`.
- When needed, the running `GumshoeRecomp.exe` process had to be killed before relinking.
- Launching the rebuilt game directly in the executable directory worked.
- The TCP server on port `4372` was used successfully.

## Verified Generic TCP Facts

The following existing generic commands were confirmed usable:

- `frame`
- `get_registers`
- `read_ram`
- `history`
- `ppu_state`
- `follow_history`
- `get_frame`
- `frame_diff`
- `memory_diff`
- `restore_frame`

## Manual Repro Facts

Multiple manual shot reproductions were collected from the user with historical polling after the fact.

The user reported these gameplay symptoms consistently:

- clicking fires
- Gumshoe does not jump when shot
- the expected zapper white rectangle around Gumshoe is not visibly present

## Shot Detection Facts

On failed gameplay shots:

- shot-related counters changed
- `$C5` pulsed
- `$C7` pulsed

This proves the game sees shot activity on those frames.

Example failed-shot behavior observed repeatedly:

- `$C5: 0x00 -> 0x01`
- `$C7: 0x00 -> 0x02`

## Blackout / Zapper Presentation Facts

On failed gameplay shot frames:

- the zapper blackout frame does occur internally
- palette switched into the blackout pattern
- `spr0_active` became active on the shot frame

Observed palette pattern on shot frames:

- `3F00..3F03 = 0F 0F 0F 0F`
- `3F11..3F13 = 30 30 30`

This proves the shot presentation pass is occurring. The failure is not "the game never enters the blackout frame."

## OAM / White Rectangle Facts

Using `gumshoe_oam` on exact gameplay blackout frames:

- no white rectangle sprite set for Gumshoe was present in OAM
- on the inspected blackout frames, OAM contained only one visible sprite:
  - `slot 0: y=E8 tile=FE attr=22 x=B0`

Immediately before the shot frame, normal gameplay sprites were present in OAM.
On the blackout frame, those were gone and the expected Gumshoe highlight rectangle was not present.

This verifies:

- blackout frame exists
- white rectangle does not appear on the gameplay shot frame that was inspected

## State Machine Facts

Failed gameplay shots did not enter the same higher-level transition path as a known successful historical transition.

On failed gameplay shots:

- `$24` stayed unchanged
- `$25` stayed `0x04`
- `$26` stayed `0x04`

Earlier in the same run, a contrasting successful transition was observed:

- `$25: 0x11 -> 0x12`
- `$1E: 0x00 -> 0x01`
- `$2C: 0x7E -> 0x01`
- bank was `6`

The failed gameplay shot path differed:

- bank was `0`
- `$25` stayed `0x04`
- `$1E` stayed `0x02`
- `$2C` stayed `0x00`

## Call Path Facts

Failed gameplay shot path:

- `func_C57F_b1`
- `func_828E_b0`
- `func_8087_b0`

Earlier successful transition path:

- `func_98D2_b6`
- `func_8132_b6`
- `func_AB56_b6`

This confirms the user-facing failed gameplay shots are going through a different logic path than the earlier successful transition.

## `$64` Target Selection Facts

During failed gameplay shots, the selector scratch byte `$64` remained invalid.

Observed values:

- `$64 = 0xF4`
- `$64 = 0xFC`

These are not valid `0x0600` entity slot offsets.

The historical decoded `selected` entity was therefore invalid on those failed shot frames.

This proves the gameplay shot path did not resolve a valid selected target before trying to use it.

## Sentinel Writer Fact

In bank 1, the pre-shot path explicitly seeds `$64` to an invalid sentinel.

Verified in generated code near `func_C57F_b1` / `func_C6FB_b1` support flow:

- `$64 = 0xF4` is written before the later selector scan is expected to replace it.

So the invalid value is intentional as a starting state. The failure is that later code never replaces it with a valid slot on failed gameplay shots.

## Selector Acceptance Facts

The selector in `func_C6FB_b1` accepts an entity slot only if all of these are true:

- `b0 != 0`
- `b7 & 0x01 != 0`
- `b7 & 0x80 == 0`
- `b2 != 0`

This was verified directly from the generated code in the `C71F..C737` region.

## Historical Entity Table Facts

On failed gameplay shot frames, valid active entities existed in the entity table.

Examples observed:

- frame `6630`: slot `0` active
- frame `6654`: slot `0` active
- frame `6690`: slots `0` and `1` active

Therefore the failure was not "there were no entities to scan."

## Exact Rejection Facts

The live entities on failed gameplay shot frames were rejected specifically by the `b7` tests.

Observed examples:

- frame `6630`, slot `0`:
  - `b0=09`
  - `b2=01`
  - `b7=94`
  - rejected because `b7 & 0x01 == 0`
  - also rejected because `b7 & 0x80 != 0`
- frame `6654`, slot `0`:
  - `b0=09`
  - `b2=01`
  - `b7=94`
  - same rejection
- frame `6690`, slot `0`:
  - `b0=09`
  - `b2=01`
  - `b7=14`
  - rejected because `b7 & 0x01 == 0`
- frame `6690`, slot `1`:
  - `b0=09`
  - `b2=01`
  - `b7=94`
  - same rejection

This proves the selector fails on `b7`, not on entity existence.

## `b7` Flag Facts

Verified direct writes and transitions:

- `0x94` is the same low bits as `0x14`, with bit `7` set
- multiple routines toggle bit `7` with `b7 ^= 0x80`
- this explains `0x14 <-> 0x94`
- this does not change bit `0`

Therefore:

- the high-bit toggle is not the root cause
- the low bit never becoming `1` is the important fact for selector failure

## Verified `b7` Writer Facts

Direct writes of non-shootable values were verified in generated code.

Examples:

- `b7 = 0x10`
- `b7 = 0x14`
- `b7 = 0x94`

For the `b0=09` bank-4 ordinary live-state handler:

- `func_9C42_b4` with `b2=0` initializes `b7 = 0x94`
- later `b2=2` path writes `b7 = 0x10`
- other related bank-4 handlers write `0x14`, `0x18`, `0x19`, etc. depending on state

## `b0=09` Dispatch Facts

In bank 4 entity dispatch:

- `b0 = 09` dispatches to `func_9C42_b4`

The failed-shot entities observed in history had:

- `b0 = 09`
- `b2 = 01`
- `b7 = 0x14` or `0x94`

This is consistent with the ordinary live `func_9C42_b4` state being present on those frames.

## Spawn / Helper Facts

Verified helper/spawn routines exist that create additional entities from the `b0=09` state machine.

Examples:

- `func_9E14_b4`
- `func_9E44_b4`

These helper routines were verified to create entities with:

- `b2 = 0x01`
- `b7 = 0x10`

These helper entities are also non-shootable by the selector's acceptance rule because bit `0` is still clear.

## Shootable-Flag Facts

Separate code paths do write shootable-looking values with bit `0` set, such as:

- `0x11`
- `0x19`
- `0x9F`

These were verified in generated code, but they are distinct from the ordinary `b0=09` live-state writes observed on the failed gameplay shot frames.

This means:

- shootable entity/flag states do exist in the codebase
- the failed gameplay shot frames inspected so far did not have those states in the scanned active entity slots

## Duck Hunt Comparison Facts

- Duck Hunt works in the user-facing sense.
- Duck Hunt's gameplay highlight path is structurally different.
- Duck Hunt stages through different entity/state structures and is not a direct one-to-one oracle for Gumshoe's failing bank-0 gameplay shot path.

## Current Verified Narrowing

The following is established by instrumentation and code inspection:

1. The failed user gameplay shots are seen by the game.
2. The blackout/zapper presentation frame occurs.
3. The expected Gumshoe white rectangle is not present in OAM on the inspected failed gameplay shot frames.
4. The gameplay path does not transition into the later hit/jump state.
5. The gameplay target selector fails because `$64` remains invalid.
6. The selector fails specifically because the active scanned entities have non-shootable `b7` values.
7. The observed active entities on those failed frames were `b0=09` entities in non-shootable states.
8. Separate shootable flag values exist elsewhere in code, but those states were not present in the scanned live entities on the failed gameplay shot frames.

## Not Yet Proven

The following has not been established yet:

- which exact gameplay actor on screen corresponds to Gumshoe in every failing shot capture
- whether a separate shootable helper entity is missing entirely on those frames, versus a required transition into a shootable state failing to occur
- the precise single defect that causes the player-hit path to remain on non-shootable entity states

## Practical Command Inventory

The following Gumshoe-specific debug commands are available now:

```json
{"cmd":"gumshoe_state"}
{"cmd":"gumshoe_entities"}
{"cmd":"gumshoe_entity","slot":0}
{"cmd":"gumshoe_frame","frame":60}
{"cmd":"gumshoe_window","frame":60,"before":2,"after":2}
{"cmd":"gumshoe_oam","frame":60}
{"cmd":"gumshoe_entities_frame","frame":60}
```

Generic useful commands:

```json
{"cmd":"follow_history","addr":"00C5","limit":40}
{"cmd":"follow_history","addr":"00C7","limit":40}
{"cmd":"follow_history","addr":"0025","limit":40}
{"cmd":"get_frame","frame":60}
{"cmd":"frame_diff","from":60,"to":61}
{"cmd":"memory_diff","from":60,"to":61}
{"cmd":"restore_frame","frame":60}
```

