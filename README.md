# Meter Gomoku AI Trainer — Line Version

A Windows desktop assistant for Gomoku (Five-in-a-Row). It visually reads a live
Gomoku board on screen (any third-party client or web board), syncs the position
into an internal board, thinks with the Rapfi engine and can auto-play the move.
This repository is the **Line (on-screen) edition** — board capture + overlay +
click injection, not the standalone trainer.

## Features

- **Automatic board capture** — finds the 15×15 grid on screen, locks it after
  first successful detection and only re-scans when the user re-identifies.
- **Real-time stone recognition** — per-frame difference detection on the locked
  grid (fast, ~50 ms stone-change latency); red / blue / green selection marks
  on stones are recognised as "not a stone" and ignored.
- **Live engine dashboard** — depth, speed (nps), nodes, eval and best line,
  refreshed continuously; single `analyze` channel used exclusively so the
  engine always spends its full budget on one search.
- **Rules** — freestyle (no forbidden), freestyle with long-line-not-a-win, and
  Renju (forbidden). Rule / time / threads / hash changes restart the engine and
  re-evaluate from the current position.
- **Balanced opening tools** — balance-point search and swap-balance evaluation,
  tuned separately for each rule (no shared forbidden/freestyle opening table);
  results are drawn as half-ring markers (upper half = move order, lower half =
  score) on the board, and the balance engine stops itself as soon as it answers
  so the main engine resumes immediately.
- **Threads / hash persistence** — user-selected thread count and hash size are
  saved and restored on the next launch, auto-scaled to the machine.

## Repository layout

```
host/      Win32 C++ host: board scanning, recognition IPC, engine process,
           overlay windows, click injection (host.cpp, engine_host.*, appver.*)
ui/        WebView2 front-end (bridge.js, calc.js, calc.html, calc.css, engine-ai.js)
vision/    On-screen board / stone recognition module (OpenCV-based)
core/      Shared JS engine-core glue
rapfi/     Rapfi search core source + the bundled vendored engine build
           (see rapfi/CMakeLists.txt; official upstream: dhbloo/rapfi)
Release/   Prebuilt installer (Meter Gomoku_Setup4.42.exe)
```

Build scripts, UI-encryption tooling and intermediate build output are **not**
included by design. The UI shipped in the installer is encrypted (`*.enc`); the
plain-text sources under `ui/` are kept for readability.

## Requirements

- Windows 10 / 11 (x64)
- [WebView2 Runtime](https://developer.microsoft.com/microsoft-edge/webview2/) (preinstalled on Win11)
- The installer ships the Rapfi engine variants (SSE / AVX2 / AVX512 / AVX-VNNI)
  and the network model; no Python or Node runtime is needed to run.

## Build from source (reference)

Toolchain used: MSVC 2022 (v143), Windows 10/11 SDK, WebView2 SDK, static OpenCV.
- `host/` — Win32 host project (manifest, appver.rc, Calculator.ico resources).
- `vision/` — DLL built against the static OpenCV libs; exports used by the host.
- `rapfi/` — build with CMake presets (`cmake --preset ...`); pick the variant
  matching the target CPU.
- The `ui/` sources are encrypted into `*.enc` at packaging time with a local
  tool (not shipped here); the host decrypts and feeds them to WebView2.

## Usage (quick)

1. Run `Meter Gomoku_Setup4.42.exe` and install.
2. Open your Gomoku client / web board.
3. Press **Identify board** (Auto-fit will expand a too-small selection toward the
   real board); once locked, the internal mini-board follows every move.
4. Choose your colour, rule and thinking time on the dashboard; enable
   **Auto-play** to let the engine respond.
5. Use **Balance points** / **Swap balance** for opening study — markers clear
   automatically after the chosen number of moves are played.

## Acknowledgements

- Engine: [Rapfi](https://github.com/dhbloo/rapfi) by dhbloo and contributors —
  the world-championship-grade Gomoku engine used for search, evaluation and
  the balanced-opening (YXBALANCE) commands.
- Vision pipeline built with OpenCV.

## License

Source released for reference; the bundled Rapfi core follows the Rapfi
license (see `rapfi/`).
