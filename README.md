# GumshoeNESRecomp

> _This recompilation is a **byproduct of developing
> [nesrecomp](https://github.com/mstan/nesrecomp)** — the games are the proving ground, the framework is the goal.
> **These are in-development previews, not finished ports — expect rough
> edges**, and depth will keep landing over months, not days. My time for any
> one title is limited, so I ask for your patience. Contributions are welcome —
> testing, issues, and PRs to the game or framework all help and will
> accelerate this game's polish. More on the why at:
> [Recomp + AI: 5 Months Later »](https://1379.tech/recomp-ai-5-months-later/)_

Static recompilation of Gumshoe (NES) for native PC.
Built with the [NESRecomp](https://github.com/mstan/nesrecomp) framework.

> **Status: Playable end-to-end with one known cosmetic bug.** As of 2026-05-04 the game boots past the title screen, gameplay (Mr. Stevenson, balloons, scoring, Zapper input) works, but the top-left HUD elements (timer and shot counter) do not render — see [ISSUES.md ISSUE-000](ISSUES.md). Gameplay itself is unaffected. If you find another bug, please open an issue.

## About

Gumshoe is a side-scrolling action game for the NES that uses the Zapper light gun. Players shoot obstacles and enemies to help Mr. Stevenson navigate through levels, collecting diamonds along the way.

## Special Feature: Mouse-as-Zapper

Gumshoe requires the NES Zapper light gun. This recompilation maps your **mouse** to the Zapper:

- **Move mouse** — aim the Zapper
- **Left click** — pull the trigger
- A **crosshair** is drawn at the aim point (white normally, red when firing)
- The OS cursor is hidden while in the game window

## Widescreen (experimental)

An opt-in Mods package extends the stage across 16:9, 21:9, 32:9 or a Fit
window, from the game's own nametable writes and column builder, and draws
objects crossing the screen edges in full. The Zapper still aims at the
native picture: the margins are off the console's screen. Enable
**Widescreen (Experimental)** in the launcher's or in-game Mods screen; see
[docs/WIDESCREEN.md](docs/WIDESCREEN.md).

## What Works

- Full gameplay with Zapper-as-mouse input
- Gumshoe jumping, obstacle shooting, enemy hit detection
- Bottle targets and entity scanning
- Round progression and scoring
- Save states (F6/F7)

## Quick Start

1. Download `GumshoeNESRecomp-windows-x64.zip` from [Releases](../../releases)
2. Extract and run `GumshoeRecomp.exe`
3. Select your `Gumshoe (USA, Europe).nes` ROM when prompted — the path is saved for future launches

## Controls

| NES Button | Keyboard       |
|------------|----------------|
| D-Pad      | Arrow keys     |
| A          | Z              |
| B          | X              |
| Start      | Enter          |
| Select     | Tab            |

| Action          | Input             |
|-----------------|-------------------|
| Aim Zapper      | Mouse movement    |
| Fire Zapper     | Left mouse button |

| Hotkey | Action |
|--------|--------|
| F5     | Toggle turbo (fast-forward) |
| F6     | Save state |
| F7     | Load state |

## Building from Source

Requires Visual Studio 2022 and CMake 3.20+.

```bash
git clone https://github.com/mstan/GumshoeNESRecomp
cd GumshoeNESRecomp

# Windows
setup.bat

# Linux / macOS
chmod +x setup.sh && ./setup.sh
```

This initializes the pinned [nesrecomp](https://github.com/mstan/nesrecomp)
submodule and links the Nestopia oracle core.

Then build:

```bash
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

Place your `Gumshoe (USA, Europe).nes` ROM in the build directory or select it at runtime.

## Architecture

This is a **static recompiler**, not an emulator. The original 6502 machine code is translated to C at build time, then compiled to native x64. The NES PPU, APU, and mapper are simulated by the runner library.

- `game.toml` — recompiler configuration
- `extras.c` — game-specific hooks (Zapper init, debug server)
- `generated/` — auto-generated C code (do not edit manually)
- `nesrecomp/` — framework submodule (recompiler + runner)

## Known Limitations

- Audio is basic (APU register writes are captured but full audio mixing is work-in-progress)

## License

PolyForm Noncommercial 1.0.0 — see [`LICENSE`](LICENSE). Third-party
components retain their own licenses.

---

<p align="center">
  <sub><b>R.A.I.D. — Retro AI Development</b> · a Discord for AI-assisted retro reverse-engineering, decomp &amp; recomp</sub>
</p>

<p align="center">
  <a href="https://discord.gg/Ad9BwSzctP"><img src=".github/raid-discord.png" alt="Join the Retro AI Development (R.A.I.D.) Discord" width="200"></a>
</p>

## Cycle backend migration

Default cycle controls: arrows move, Z is A, X is B, Enter is Start and
Backslash is Select. Escape opens the menu, Tab fast-forwards, and F8/F9
save/load the cycle state. Use Controls to remap inputs in `config.ini`.
Older `keybinds.ini` and F5/F6/F7 instructions below apply to the legacy host.

This branch defaults to the cycle CPU backend. The existing legacy build is
available with `-DNESRECOMP_BACKEND=legacy`. Initialize the pinned engine and
recomp-ui submodules, then build from your original ROM:

```powershell
cmake -S . -B build-cycle -DNESRECOMP_ROM="C:/path/to/Gumshoe (USA, Europe).nes"
cmake --build build-cycle --config Release
```

`NESRECOMP_ROOT` and `NESRECOMP_RECOMP_UI` can select other checkouts;
`NESRECOMP_HOST_COMPILER` can select a prebuilt NESRecomp compiler. The supplied
`cyc_seeds.txt` covers the tested title and opening gameplay route. Unseen code
continues on the cycle interpreter, so the profile is a speed aid rather than
a restriction on play. This build uses NTSC timing for the supplied ROM.

Mouse movement aims the Zapper; left click fires. Aim mapping follows the
displayed picture through resizing and treats black bars as offscreen.
Mouse aiming and Crosshair are available in the launcher and Escape menu.
Their settings are saved in `config.ini [Zapper]`; existing `keybinds.ini`
choices are imported without changing the legacy file. The crosshair is a
presentation overlay and cannot count as light detected by the gun.

F8 saves and F9 loads the cycle slot under `saves/*.cycstate`. These states
include trigger, aim and sensor history. Legacy `.state` files belong to the
legacy backend; retain them for that build.

Validation on this branch: 1,800-frame native/interpreter parity at all four
CPU/PPU clock alignments, exact save-state continuation, independent TriCNES
comparison with the gun detached, actual aimed-hit and offscreen-miss routes,
window/menu/crosshair/letterbox/settings checks, and both backend builds.
TriCNES does not implement the Zapper, so gun routes use the cycle interpreter
comparison plus real gameplay outcomes. Owner confirmed crosshairs and firing pass in both titles; no merge
has been performed.

Cycle Windows builds use `tools/build.ps1 -Rom <original-ROM>`. Create a ROM-free
ZIP with `tools/make_release.ps1 -Rom <original-ROM>`; `-SkipBuild -BuildDir
build-cycle` packages an existing production cycle build. Legacy C remains
available through explicit CMake selection.
