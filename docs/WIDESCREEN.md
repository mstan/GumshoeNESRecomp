# Widescreen (experimental)

Enable **Widescreen (Experimental)** in the launcher's **Mods** screen, or in
the in-game menu's **Mods** section. The package is off by default; with it
off the game runs exactly as without it. It targets Gumshoe (USA, Europe),
PRG+CHR payload CRC32 `BEB8AB01`, on the cycle backend, and follows the
custom-renderer pattern of Super Mario Bros. 2 (Japan), Super Mario Bros. 2
and Kirby's Adventure.

## Presentation

Choose **Fit window**, **16:9**, **21:9** or **32:9** (square pixels: 426,
560 and 854 x 240; Fit follows the window between 256 and 854).

- **Camera: Stay inside stage edges** (default) starts each stage with the
  native screen at the canvas's left edge, as nothing exists left of a
  stage's first column, and centers it once the stage has scrolled far
  enough; at a stage's last screen the view stops at its end. **Keep native
  view centered** never moves the native screen.
- **Timer and shots: Screen edge** (default) draws the timer and the shot
  counter at the canvas's left edge; **Original position** leaves them in
  the native screen.

Title, letter, phase cards and other screens that are not a stage keep the
centered native picture.

### The Zapper

The mouse aims at what the window shows. The native screen sits wherever the
camera placed it, and the aim maps through that position (the compositor
declares it to the engine: `cyc_render_set_native_origin`). The margins are
off the console's screen: a shot there sees no light, exactly like aiming
away from a TV, so the game's own hit detection decides every shot and only
targets in the native screen can be hit. The crosshair follows the mouse
across the whole picture. During a shot's black frames the game blacks out
its background palettes, and the margins, drawn with the same palettes, go
black with it.

## How it works

Nothing below changes the machine. With the mod on, every frame's
full-machine hash equals stock (`tests/widescreen_probe.py`, check `stock`).

**Stage map** (`src/gumshoe_ws_world.c`). Gumshoe streams one 16-pixel
column at a time into its two nametables, about 36 pixels ahead of the
screen, and never scrolls back. The mod keeps the stage in world columns
(8-pixel tile columns, a ring of 256, tagged):

- every CPU write to a nametable or attribute table (the engine's PPU write
  callback, bank independent, tile edits included) is stored at its world
  column, and the columns the frame showed are re-read from CIRAM;
- columns ahead come from the game's own column builder (bank 0 `$8435`,
  bank 1 `$8378`, bank 2 `$8807` - the three stage banks carry the same
  engine at different addresses), run again and again from the live stream
  state on an isolated machine (`runner/cyc/cyc_mod.h`): tiles from
  `$0532`/`$0552`, attributes from the upload list at `$0180` after each odd
  column. Everything else the builder does - the objects its special
  metatile IDs spawn, its stage-loop decisions - is discarded with the
  scope. It runs as far as the widest view of the current camera reaches.
  When the game later writes a column for real, the prediction is checked
  against it.

The attribute list matters: the game's attribute routine writes eleven list
entries where NMI uploads eight, and the eleventh lands on `$01A0[0]` after
the first entry copied it. `$01A0` therefore no longer holds the top row's
attribute; the list does. (Stages with a sky in that row showed it.)

World column 0 is the stage's first: a stage starts (`$25` = 3) from scroll 0
with 18 columns built. A death restarts at the beginning of the current
four-screen group through the same setup. Special screen IDs `$08`/`$18`
make the builder stop after their 16 columns (`$4E`), which is the stage's
end.

**Compositor** (`src/gumshoe_ws.c`). The native picture stays authoritative
in its 256 columns; the stage map fills the rest at the same world offset,
with the frame's pattern table and palettes. A picture is extended only
when the stage map agrees with the native background over the native
columns (coverage, independent of the palette); transitions and text
screens fall back to the centered picture. Native columns 0-7, where the
game clips sprites, get the frame's sprites again.

**Objects** (`src/gumshoe_ws_objects.c`). An object keeps its screen X in
eight bits plus a flag (byte 7 bit 7) that means off the screen on either
side. The game spawns objects about 36 pixels past the right edge, does not
draw them until they enter, and drops them at the left edge. At each NMI
(hook site `gumshoe.widescreen.nmi`, `$8031` in banks 0-2, content-keyed)
the game's own drawer (bank 0 `$964A`, bank 1 `$9600`, bank 2 `$99F0`) runs
on an isolated machine for every object near or past an edge with its X
moved on screen; the sprites, moved back, are drawn in the margins. An
object the game drops at the left edge continues as a resident for the
picture: its bytes are stepped by the game's own animation and type
routines (`$94CB`/`$9495`, `$94F9`/`$94C3`, `$98AF`/`$9861`) at X 128 on an
isolated machine and moved against the camera as the scroll routine moves
every object, until it leaves the view or its routine ends it. Objects keep
the game's spawning, movement, collisions and Zapper targets.

**Save states** carry the stage map and the object packets and residents
as mod records (`gumshoe.widescreen.world`, `gumshoe.widescreen.objects`,
versioned). Settings belong to the Mods selection; loading a state never
changes them. A state saved without the mod loads with an empty map, which
the next frame fills from the nametables (the left margin then fills as the
stage scrolls).

## Command line

```powershell
.\GumshoeRecomp.exe "Gumshoe (USA, Europe).nes" --widescreen fit|16:9|21:9|32:9|off
.\GumshoeRecomp.exe "Gumshoe (USA, Europe).nes" --widescreen 32:9 --widescreen-camera edges|centered
.\GumshoeRecomp.exe "Gumshoe (USA, Europe).nes" --widescreen 32:9 --widescreen-hud edges|center
```

These override the saved Mods selection. Diagnostics: `--widescreen-log
FILE` (one JSON line per frame), `--widescreen-lookahead off`,
`--widescreen-ring 128` (wraparound on short routes), TCP `gumshoe_ws_state`.
Test fixtures, never player features: `--dev-hold ADDR:VALUE` and
`--dev-autofire N` (a Zapper autopilot that shoots Stevenson when the
game's collision columns show a hole or wall ahead).

## Validation

```powershell
python tests/widescreen_probe.py --exe build_ws/Release/GumshoeRecomp.exe `
    --baseline <untouched stock build>/GumshoeRecomp.exe --out <new directory>
```

The probe's docstring lists its checks. Results are recorded in the central
Beads issue `beads-2dw.16.2`.

## Limits

- Objects use the game's own activation: nothing spawns earlier for the
  wider view, and the right margin shows objects only once the game has
  spawned them (about 36 pixels past the native edge).
- Residents (objects past the left edge) are presentation only; they do not
  collide or score, and a full object table holds them for a frame.
- Long-stage validation used a test autopilot with the game's invincibility
  timer held; screen loops and stage ends are covered by the code paths
  above and by owner playtesting, not by an automated route to every end.
