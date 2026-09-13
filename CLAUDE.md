# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project state

This is a PlatformIO/Arduino firmware project for an ESP32 board (`nodemcu-32s`). It is currently just the
default PlatformIO scaffold — `src/main.cpp` contains only the generated stub (`setup()`/`loop()` plus a
placeholder `myFunction`), and `include/`, `lib/`, and `test/` are empty aside from PlatformIO's own README
placeholders. There is no application logic, custom library, or test suite implemented yet. This is not a
git repository.

## Commands

This project uses PlatformIO. If the `pio` CLI is not on `PATH` in the shell (it isn't in this environment
by default), invoke it via the PlatformIO Core install, typically at `~/.platformio/penv/bin/pio`, or use
the PlatformIO IDE extension in VS Code.

- Build: `pio run`
- Build for a specific environment: `pio run -e nodemcu-32s`
- Upload to the connected board: `pio run -t upload`
- Serial monitor: `pio device monitor`
- Clean build artifacts: `pio run -t clean`
- Run unit tests (once tests exist under `test/`): `pio test`
- Run a single test file: `pio test -f <test_name>`

## Architecture

- `platformio.ini` defines a single build environment, `[env:nodemcu-32s]`, targeting the `espressif32`
  platform with the Arduino framework. Additional environments (e.g. for a different board or a
  native/test environment) would be added here as new `[env:...]` sections.
- `src/main.cpp` is the firmware entry point (`setup()` / `loop()`), following standard Arduino structure.
- `include/` is for project header files shared across `src/` files.
- `lib/` is for private, project-specific libraries — each in its own subdirectory (e.g. `lib/Foo/Foo.h`,
  `lib/Foo/Foo.cpp`), auto-discovered and linked by PlatformIO's Library Dependency Finder based on what
  `src/` includes.
- `test/` is for PlatformIO Unit Testing (Unity-based on-device or native tests).
