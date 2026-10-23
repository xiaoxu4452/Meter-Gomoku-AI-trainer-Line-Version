# Meter Gomoku — Line Version

**English** | [Chinese](README_zh.md)

A Windows desktop assistant for Gomoku / Renju. It **reads the board you are already playing on** — any third-party client, a web board or an emulator — keeps a mirror of the position, asks the bundled Rapfi engine for the best move, and can play it for you. This repository is the **Line (screen-recognition) version**: screen capture + always-on-top overlay + simulated clicks. It is not a stand-alone trainer.

<p align="center"><img src="docs/images/main.png" width="780" alt="Main window"></p>

> 📄 **Printable bilingual manual (PDF, 48 pages):** [docs/Meter-Gomoku-Manual.pdf](docs/Meter-Gomoku-Manual.pdf)

Current version: **4.43** · Engine: **Rapfi 0.44.1** (mix9 NNUE)

---

## 1. What makes it different

| | |
|---|---|
| **Works with any client** | No hooks, no DLL injection, no memory reading. It only takes screenshots and simulates clicks, so it never touches the other program's internals. |
| **Locked-geometry recognition** | The 15×15 grid is located once and then **locked**; afterwards only changed intersections are re-read (a still picture costs almost nothing, one stone costs one cell). |
| **Always-on-top link window** | A fixed 304×712 window that never gets covered by the game, with its own control panel, status line and mirror board. |
| **Real engine** | Rapfi (GPL-3.0), shipped as separate processes in four instruction-set variants; the main lane owns the whole transposition table. |
| **Official balance tools** | Balance search uses the engine's native paired `YXBALANCETWO` search, and swap openings come from the official opening book — not from a hand-made table. |
| **Nothing to install at runtime** | No Python, no Node, no extra DLLs. WebView2 is the only runtime dependency (built into Windows 11). |

---

## 2. Quick start

1. Run `Release/Meter Gomoku_Setup4.43.exe` and finish the installer.
2. Start the app, then open your Gomoku client (or a web board) and get to the game screen.
3. In the main window click **Link** in the top bar — the small link window appears.
4. Click **Frame Board** and drag a rectangle around the board *including its outer border*.
5. Choose **AI Black / AI White** (the side the engine plays), the rule and the thinking time, then switch on **Auto Drop**.

The mirror board in the link window starts following the game immediately. If your client ignores simulated clicks, turn on **Real Click**.

---

## 3. The main window

The window is split into four areas:

| Area | What is there |
|---|---|
| **Top bar** | **Link** (opens the link window) · **Image** (screen recognition) · **Review** · **History** · **About** · language toggle (EN / CN) · **Colors** |
| **Left dock** | Engine dashboard: depth / speed / nodes / time / eval / best move, the evaluation curve, and Black/White score & win-rate rows |
| **Board** | 15×15 board with coordinates on all four sides; analysis marks are drawn straight on it |
| **Right dock** | Cards: **Game setup**, **Analysis**, **Engine**. Drag by the title bar to reorder, fold with the dash, close with ✕; the layout is remembered |
| **Bottom bar** | Live analysis · ◀ ▶ (step moves) · Restart · Save position · undo / redo / swap colours · AI side · Settings (add a card) |

### 3.1 Engine dashboard

<table>
<tr>
<td width="34%" valign="top"><img src="docs/images/engine.png" width="300" alt="Engine dashboard"></td>
<td valign="top">
The quickest way to see whether the engine is actually working — every value updates while any calculation runs.
<ul>
<li><b>Depth</b> — current search depth.</li>
<li><b>Speed</b> — nodes per second.</li>
<li><b>Nodes</b> — nodes searched this turn.</li>
<li><b>Time</b> — elapsed search time (accumulates in live analysis).</li>
<li><b>Eval</b> — position score; positive means good for the side to move.</li>
<li><b>Best</b> — the engine's chosen move.</li>
<li><b>Eval curve</b> — score over depth; the vertical axis auto-scales.</li>
<li><b>Black / White — Score · Win rate</b> — each side's score and implied win rate.</li>
</ul>
</td>
</tr>
</table>

### 3.2 Analysis and calculation

<table>
<tr>
<td width="34%" valign="top"><img src="docs/images/analysis.png" width="300" alt="Analysis card"></td>
<td valign="top">
<ul>
<li><b>Compute</b> — one deep search of the current position; the best point is marked on the board. The button toggles to <b>Stop</b>.</li>
<li><b>Multi-point</b> — returns <i>N</i> candidates (2–8, default 4); each ring carries its rank and score.</li>
<li><b>Balance picks</b> with <b>Points</b> — the engine's native paired balance search; returns points where neither side has a clear edge (<code>|eval| ≤ 50</code> counts as balanced). <b>Points</b> sets how many to return (1–6); an empty board is fixed at 3.</li>
<li><b>Defend scan</b> — scores every empty point in a 9×9 neighbourhood as <b>W</b> (moves to win) / <b>L</b> (moves to lose); re-scans after each move.</li>
<li><b>Look-ahead · VCF · VCT</b> — forced-line tools. Look-ahead plays the position forward until five in a row actually appears; VCF uses consecutive fours only; VCT adds live threes.</li>
<li><b>Black / White + Commit</b> — the colour and button used to commit a suggested line.</li>
<li><b>Coach view</b> — four-colour square hints while it is your turn (best / good / fair / poor); they vanish when you move.</li>
<li><b>Clear calc</b> — erases this card's marks without touching the position.</li>
</ul>
</td>
</tr>
</table>

### 3.3 Game setup

<table>
<tr>
<td width="30%" valign="top"><img src="docs/images/setup.png" width="285" alt="Game setup card"></td>
<td valign="top">
<ul>
<li><b>Mode</b> — <i>Play</i> · <i>Free</i> (place stones) · <i>Endgame</i> (mate problem).</li>
<li><b>AI plays</b> — Black / White toggles; both off means you play both colours.</li>
<li><b>Think time</b> — 0.2 s to 300 s. Shared with the link window; the two are always in step.</li>
<li><b>Background thinking</b> — keeps searching during your turn (hot transposition table), so the AI replies faster.</li>
<li><b>Rules</b> — Freestyle · Overline · Renju.</li>
<li><b>Engine</b> — <b>Threads</b> (maximum chosen from the real topology: physical cores minus two) and <b>Hash</b> (memory-adaptive). Both are persisted.</li>
<li><b>Board display</b> — <b>Move numbers</b> on stones, <b>Hover box</b> preview.</li>
<li><b>Position</b> — copy / paste / load the whole position as one string.</li>
</ul>
</td>
</tr>
</table>

---

## 4. Screen recognition

<table>
<tr>
<td width="36%" valign="top"><img src="docs/images/vision.png" width="340" alt="Recognition window"></td>
<td valign="top">
Open it from <b>Image</b> in the top bar. It turns a screenshot into a position code.
<ol>
<li><b>Upload images</b> (multi-select) or <b>Screenshot</b> a region.</li>
<li><b>Crop</b> — the box snaps to the board and keeps about 1/30 of the board as margin, so the outermost stone stays complete.</li>
<li><b>Re-recognize</b> — the result becomes a position code you can copy, or load straight into practice with <b>Load to practice</b>.</li>
<li><b>Edit</b> — swap Black and White, delete stones, add stones; <code>Ctrl+Z</code> undoes.</li>
<li><b>VC Mode</b> — for mate problems: the two stone counts may differ, adding is unlimited, and loading switches to Endgame mode with the attacker preset.</li>
<li><b>Board code</b> + <b>Copy</b> — the position as a compact string.</li>
</ol>
<b>Recog params</b> exposes six thresholds that apply live while you drag:
<ul>
<li><b>Sync interval (ms)</b> — how often stones are re-read; smaller is more responsive but costs more CPU.</li>
<li><b>Grey offset</b> — the grey threshold for "there is a stone here".</li>
<li><b>Black offset / Black percent</b> — grey threshold and in-cell coverage for Black.</li>
<li><b>White offset / White percent</b> — the same two values for White.</li>
</ul>
</td>
</tr>
</table>

### How the recognition works

* **Two-stage fusion** — the first frame is pinned down by the app's own detector, which also reports the exact position of all 15 horizontal and 15 vertical lines. Once locked, the synchronous stone reader takes over and the window fingerprint is handed across, so the full search happens exactly once.
* **Cell-by-cell change detection** — every intersection keeps a 6×6 sample signature, so a still picture re-reads zero cells and a single stone re-reads exactly one.
* **Image tolerance** — if the whole board reads empty, the thresholds are relaxed once against this frame's own peak; if still empty, a local-contrast reader takes over (it compares each intersection's centre patch with the diagonal patches around it). A grid that does not line up with the lines is re-checked by an older projector-based recogniser, and a whole row of 15 stones — impossible in Gomoku — is treated as a one-cell misalignment and re-read after shifting the grid.
* **Capture protection** — every mark the app draws is excluded from screen capture, so it can never misread its own overlay as stones.

---

## 5. Link mode

<table>
<tr>
<td width="30%" valign="top"><img src="docs/images/link.png" width="290" alt="Link window"></td>
<td valign="top">
The link window is a separate top-level window, <b>304×712</b>, always on top. It does <i>all</i> of the recognition and all of the AI work; the main window only mirrors the position.
<ul>
<li><b>Freestyle / Overline / Renju</b> — rule for the current game.</li>
<li><b>Frame Board</b> — drag a rectangle over the board on screen; recognition starts once the frame is set.</li>
<li><b>Recog Params</b> — swaps the button area for the six thresholds, applied live (§6).</li>
<li><b>Real Click</b> — uses the physical mouse cursor. <b>Off by default</b>; only needed for clients that accept nothing but real mouse input.</li>
<li><b>Auto Drop</b> — thinks as soon as the opponent's move is seen and plays for you.</li>
<li><b>×1 / ×2 / ×3</b> — clicks per move.</li>
<li><b>AI Black / AI White</b> — mutually exclusive; the choice is mirrored into the main window immediately.</li>
<li><b>Think Time</b> — 0.2–300 s, one shared value.</li>
<li><b>Swap Balance</b> — opening balance points (see below).</li>
<li><b>Swap Eval</b> — scores the current swap position with the playing engine, to help you decide whether swapping is worth it.</li>
<li><b>Cancel</b> — stops an in-flight balance / evaluation and clears the marks already drawn.</li>
</ul>
</td>
</tr>
</table>

### Status area

`Depth` · `Speed` · `Nodes` · `Eval` update live; below them a fixed-height dashed box explains what is happening (recognising / thinking / points marked …). The mirror board shows the recognised position and the stone count (**Total / Black / White**) and keeps the last good position when a frame reads nothing, instead of clearing itself.

### Auto Drop: the three-stage pipeline

| Stage | Method | When |
|---|---|---|
| 1 | `PostMessage` straight to the target window | Default: lightest, never disturbs the physical mouse |
| 2 | `SendInput` | Escalated automatically when the next frame says stage 1 failed |
| 3 | `SetCursorPos` + `mouse_event` (a real click) | Last resort; once it works it is pinned and reused |

Every stage is judged by the following frame. If all three fail, a whole round restarts automatically — up to three rounds, then it stops and tells you. A finished game switches Auto Drop off, so the next game starts clean.

### Swap balance

On an **empty board** the app uses the official opening book: the engine returns a three-stone opening for the current rule (freestyle / Renju book), which is expanded through the board's eight symmetries and de-duplicated. The balance engine scores the pair, and that score is the number you can check.

| Stones on board | Points | Colours | Notes |
|---|---|---|---|
| 0 (empty) | 3 | 2 Black, 1 White | Black 1 fixed at the centre; White 2 / Black 3 from the official book |
| 3 stones (5 moves) | 2 | 1 Black, 1 White | One **paired** balance search returns both points together |
| More than 5 stones | — | — | Past the swap window; the app says so |

Marks live inside the recognition frame: blue/purple rings with a number for the order of play, and they disappear once you have played those stones. Pressing **Swap Balance** cancels any selected AI side (they would interfere); if Auto Drop is on, the points are played in order automatically.

---

## 6. Recognition parameters in the link window

<table>
<tr>
<td width="30%" valign="top"><img src="docs/images/link-params.png" width="290" alt="Recognition parameters"></td>
<td valign="top">
One click replaces the button area with the six thresholds; the statistics row folds away and a text box explains each parameter.
<ul>
<li><b>Interval (ms)</b> — 200 by default. Smaller is faster but costs more CPU.</li>
<li><b>Gray offset</b> — larger is easier to detect; smaller is stricter.</li>
<li><b>Black offset / Black percent</b> — the grey threshold and in-cell coverage for Black stones.</li>
<li><b>White offset / White percent</b> — the same for White stones.</li>
<li>Dragging or typing applies the value <b>immediately</b> and also saves it.</li>
<li><b>General</b> — restores defaults (200 / 50 / 96 / 33 / 96 / 33).</li>
<li><b>Save</b> / <b>Load</b> — store the six values under a name (up to 50 presets).</li>
<li><b>Apply</b> — saves and folds the panel; <b>Cancel</b> restores the last applied values, in the page <i>and</i> in the host.</li>
</ul>
</td>
</tr>
</table>

---

## 7. Rules and forbidden moves

| Rule | Five in a row | Forbidden |
|---|---|---|
| Freestyle | Wins, overline included | none |
| Overline | Wins; **six or more in a row does not win** | none |
| Renju | Wins | overline, double-four, double-three (four-three is legal) |

---

## 8. Engine update: Rapfi 0.44.1

The bundled engine has been moved to the newer **Rapfi 0.44.1** (upstream master, mix9 NNUE) and is built as four instruction-set variants with clang + LTO. Compared with the previous mix8 engine this is a large strength step:

| Area | What changed |
|---|---|
| **NNUE** | New **mix9** architecture. Upstream VLTC: **+29 Elo** (freestyle-15), **+21 Elo** (freestyle-20), **+34 Elo** (standard-15), **+56 Elo** (renju-15) versus mix8. |
| **Instruction sets** | New **AVX-VNNI** and **AVX512-VNNI** builds; the installer ships SSE / AVX2 / AVX512 / AVX-VNNI and picks the best one for your CPU. |
| **Search** | Optimised search and better scaling on multi-core machines. |
| **Protocol** | `SWAPABLE` flag and the native `YXBALANCEONE` / `YXBALANCETWO` balance commands, which this app uses directly for swap balance. |
| **Startup** | Four NNUE networks (~38 MB) are decompressed at launch, so the engine answers its first command after 300–800 ms; the host waits for that before sending the first position. |

The `rapfi/` folder in this repository tracks this newer upstream source. Rapfi is distributed under **GPL-3.0** — see `rapfi/` for the licence text and full source.

---

## 9. Performance notes

* The engine runs as a **separate process** and is never pinned to specific cores — pinning it costs about 5× the nodes per second on modern hybrid CPUs.
* Recognition gets its own share of the cores and runs at high priority; it is intermittent (cell-by-cell change detection), so it uses a small fraction of one core.
* The main engine lane owns the whole transposition table; helper lanes drop to a floor value, so total hash usage equals the size you selected.
* First launch defaults to the top of the thread list (12 threads / 512 MB on a 16-thread machine).

---

## 10. Frequently asked questions

| Symptom | Cause | Fix |
|---|---|---|
| The board is found but no stones appear | Frame is too tight, or the stones have very low contrast | Frame the board **with its border**, then try lower black/white offsets |
| Too many or too few stones of one colour | Thresholds not tuned for this theme | Use Recog Params: set the black/white offset for the grey threshold, percent for the in-cell coverage |
| Automatic dropping does nothing | The client only accepts real mouse input | Turn on **Real Click**; keep the window position fixed (the frame is geometry-locked) |
| The window is blank on start | Security software quarantined the UI package | Whitelist the install folder and reinstall |
| Antivirus flags the installer | Unsigned executable that captures the screen | Allow it, or sign the build |
| The board mirror stops following | The board moved, or the window was resized | Click **Frame Board** again |

---

## 11. Repository layout

```
host/      Win32 C++ host: board scan, recognition IPC, engine processes,
           overlay window, simulated clicks (host.cpp, engine_host.*, appver.*)
ui/        WebView2 front-end: calc.html / calc.css / calc.js / bridge.js
vision/    screen board & stone recognition (statically linked OpenCV)
core/      legacy shared engine glue, kept for reference (not loaded by 4.43)
rapfi/     Rapfi 0.44.1 search core + per-ISA build presets
           (upstream: dhbloo/rapfi, GPL-3.0)
Release/   pre-built installer (Meter Gomoku_Setup4.43.exe)
docs/      screenshots used by this document
```

> The `ui/` folder is a **reading snapshot** of the shipped 4.43 front-end, not a complete build tree (the engine adapter and a few helper modules are not included). Build scripts, the UI packing tool and intermediate artefacts are intentionally not part of this repository.

The installer ships the UI as a single packed container (`calc/ui.pak`, binary-encrypted), which the host decrypts in memory before handing it to WebView2 — there is no plain HTML on disk.

---

## 12. Requirements

* Windows 10 / 11 (x64)
* [WebView2 Runtime](https://developer.microsoft.com/microsoft-edge/webview2/) (already present on Windows 11)
* No Python, no Node, no separate engine install — the installer bundles the Rapfi variants (SSE / AVX2 / AVX512 / AVX-VNNI) and the network weights

---

## 13. Building from source (reference)

Toolchain: MSVC 2022 (v143), Windows 10/11 SDK, WebView2 SDK, statically linked OpenCV.

* `host/` — Win32 host project (manifest, `appver.rc`, icon resources).
* `vision/` — builds a DLL against static OpenCV; the host calls its exported functions.
* `rapfi/` — built with the CMake presets in `rapfi/`, one binary per instruction set.
* `ui/` — plain sources; the shipped build packs and encrypts them into `calc/ui.pak` with a local tool that is not part of this repository.

---

## 14. Credits & licence

* Engine: [Rapfi](https://github.com/dhbloo/rapfi) by dhbloo and contributors — the world-class Gomoku/Renju engine behind the search, the evaluation and the `YXBALANCE` opening commands. Distributed under **GPL-3.0**; see `rapfi/` for the licence text and the source.
* Vision pipeline built on **OpenCV** (Apache-2.0), statically linked.
* Document fonts: Source Han Serif / Source Han Sans, **SIL OFL 1.1**.
* Application code: provided for study and reference. Use the assistant in line with the rules of the platform you play on.
