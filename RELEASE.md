# Gumshoe — macOS (Apple Silicon) build

Native arm64 macOS build of Gumshoe, attached to release **v1.2** as
`GumshoeNESRecomp-macos-arm64.zip`.

## What this is
- The original game statically recompiled to native arm64 (no emulator core shipped).
- Self-contained `.app`: SDL2 bundled via `@executable_path`, ad-hoc codesigned.
- Verified by manual play on Apple Silicon (looks/sounds correct on the golden path).

## Status
Light-gun (Zapper) game: aim with the mouse, left-click to shoot (enabled by default).


## Install
1. Download `GumshoeNESRecomp-macos-arm64.zip` from the **v1.2** release and unzip.
2. First launch: right-click `Gumshoe.app` -> Open (ad-hoc signed), or
   `xattr -dr com.apple.quarantine "Gumshoe.app"`.
3. ROM not included — supply your own dump: Gumshoe (USA) .nes dump
4. Run: `"Gumshoe.app/Contents/MacOS/Gumshoe" /path/to/rom`

## Build it yourself
`scripts/release-mac.sh` reproduces this artifact (build -> .app -> zip);
`scripts/release-mac.sh --publish` re-attaches it to the latest release.
Requires: `brew install cmake ninja sdl2 dylibbundler` on Apple Silicon.
