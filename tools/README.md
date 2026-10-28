# Meter Gomoku - Tools & Environment

Helper scripts and build/runtime environment notes.

## Environment Overview

| Component | Type | Requirements |
| --- | --- | --- |
| `Desktop GomokuTrainer.exe` | Released app | None - fully packaged (self-contained) |
| `Vision/GomokuVision.dll` | Vision module | None at runtime; OpenCV 4.x + MSVC to rebuild |
| `rapfi-native/` | Engine bundle | None at runtime; CMake + MSVC to rebuild engine |
| `ui/ calc.html` | Web UI | Any modern browser (plain HTML/JS/CSS, no build step) |
| `core/engine-core.js` | Pure JS core | None - dependency-free UMD module |

Runtime is zero-dependency: every binary ships inside the app folder.
The tools below only matter when rebuilding components from source.

## Scripts

| Script | Purpose |
| --- | --- |
| `check-env.ps1` | Probe the local toolchain (Node, Python, Git, CMake, MSVC, OpenCV) and report PASS/FAIL per item |
| `pack-rapfi-native.ps1` | Verify the 5 engine variants in `../rapfi-native` against `engines.json` SHA-256 hashes and print a manifest |

## Build Notes (short)

- `rapfi/` engine: CMake preset build, see `rapfi/CMakePresets.json`.
- `host/` + `vision/`: MSVC 2022 (v143), Windows SDK 10, OpenCV 4.x; set `OpenCV_DIR` before building.
- `ui/` + `core/`: no toolchain - open `ui/calc.html` directly in a browser.

## Upstream

Rapfi engine: https://github.com/dhbloo/rapfi (GPLv3, see `../rapfi-native/NOTICE.txt`).
