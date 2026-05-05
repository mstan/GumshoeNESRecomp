# GumshoeNESRecomp

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

This clones [nesrecomp](https://github.com/mstan/nesrecomp) at the exact
version pinned in `nesrecomp.pin` and links the Nestopia oracle core.

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
