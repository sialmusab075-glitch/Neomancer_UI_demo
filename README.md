# SOL SYSTEM SIM

Real-time Solar System simulator with a sci-fi "mission control" telemetry HUD.
C++17 · OpenGL 3.3 core · GLFW · Dear ImGui (docking) · ImPlot · GLM. Windows 10/11 x64.

**Status: Milestone 3.** Sun and all eight planets under Kepler motion; compressed and true
display scales; starfield, a polar ecliptic grid with a "gravity well", and a particle-shell Sun;
click-picking, double-click tracking, and tag labels. A docked telemetry HUD has:
- a status bar and mission controls;
- the selected body's hero distance, readout bars, state-vector grid and ring gauges;
- an r(t) history plot with an orbit-phase histogram;
- an event log. Perihelion, aphelion, opposition and conjunction are detected by sign changes,
  then refined by bisection on the analytic orbit.

## Requirements

- Visual Studio 2022 with the **Desktop development with C++** workload
  (it includes MSVC, CMake ≥ 3.20, Ninja and Git).
- An internet connection for the first configure. GLFW, GLM, Dear ImGui and ImPlot
  are fetched by CMake `FetchContent` at pinned tags. Nothing needs downloading by hand.
- A GPU/driver with OpenGL 3.3.

## Build and run

### Command line (Visual Studio generator)

From any terminal where `cmake` is on PATH (e.g. *Developer PowerShell for VS 2022*):

```bat
cmake -B build
cmake --build build --config Release
build\Release\SolSystemSim.exe
```

Debug build (keeps a console window for logging):

```bat
cmake --build build --config Debug
build\Debug\SolSystemSim.exe
```

`cmake -B build` picks the "Visual Studio 17 2022" generator and x64 automatically when
VS 2022 is installed. To be explicit: `cmake -B build -G "Visual Studio 17 2022" -A x64`.

### Command line (Ninja, from a VS Developer Prompt)

Open **x64 Native Tools Command Prompt for VS 2022**, then:

```bat
cmake -B build-ninja -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-ninja
build-ninja\SolSystemSim.exe
```

### MinGW-w64 GCC (no Visual Studio, everything on D:)

Needs a 64-bit **WinLibs** GCC 13+ (UCRT, POSIX threads) and CMake ≥ 3.20.
In a plain Command Prompt:

```bat
set PATH=D:\tools\mingw64\bin;D:\tools\cmake\bin;%PATH%
cmake -B build-gcc -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++
cmake --build build-gcc
ctest --test-dir build-gcc --output-on-failure
build-gcc\SolSystemSim.exe
```

Put the new toolchain first on PATH so an old 32-bit `C:\MinGW` is not picked up.
The C++ runtime is linked statically, so the exe needs no extra DLLs.

### Visual Studio IDE

Either:

1. **Solution:** run `cmake -B build` once, open `build\SolSystemSim.sln`, pick
   *Release* or *Debug*, and press **F5**. `solsim` is already the startup project.
2. **Open Folder:** *File → Open → Folder…*, select the repository root, wait for CMake
   to configure, choose **SolSystemSim.exe** in the startup-item dropdown, and press **F5**.

Assets are copied next to the executable after every build, and all paths are resolved
from the executable's directory. So the exe also runs when double-clicked in Explorer.

### Tests

```bat
cmake --build build --config Release --target sim_tests
ctest --test-dir build -C Release --output-on-failure
```

`sim_tests` also checks the event detector against published dates, 2000–2005:
- Earth perihelia and aphelion (±2.5 d; the elements describe the Earth–Moon barycentre).
- Mars oppositions, including the 2003 close approach (0.373 AU), and a Mars conjunction.
- Jupiter oppositions.
- The same events are found at 0.5 d steps, at 6 d steps (365 d/s at 60 fps) and in reverse.

`render_tests` checks the scale mapping (log formula, axis handedness, true-scale ratios),
picking (ray–sphere, pick ray through a projected point), and the starfield / shell / grid
generators. It also checks the reactor structure: counts, outward ticks (30° ones twice as
long), planar and bounded geometry, the edge fade, staggered captions, seed determinism, and
ramp indices within [0,1].

`sim_tests` covers the Kepler solver (residual < 1e-9, ≤ 15 iterations up to e = 0.97),
vis-viva vs. propagated velocity, periodicity, and Earth's J2000 position against the
ephemeris. It also runs the Milestone 1 acceptance check: it drives the same
clock → propagate loop the app runs, at +10, +365 and −120 d/s, and measures one Earth
revolution geometrically (**365.258 sim-days**).

## Controls

| Input | Action |
|---|---|
| Left-drag (on the 3D view) | Orbit the camera (pitch clamped to ±89°) |
| Right-drag | Pan (detaches "follow") |
| Mouse wheel | Exponential zoom, clamped |
| Click a body | Select it (small bodies have a 12 px minimum click radius) |
| Double-click a body / F | Track it: the camera glides onto it and follows |
| Esc | Stop tracking · Home: reset the view |
| Space | Pause / resume |
| MISSION CONTROL panel | HOLD/PLAY, REV, ±1 D (while held), EPOCH, log-scale time slider (−365…+365 d/s), COMPRESSED / TRUE SCALE, target list + TRACK, layers (orbits, grid, stars, labels; moons in M4), RESET LAYOUT |
| HISTORY panel | 90 D / 1 YR / 5 YR window chips |
| Panels | Drag a panel's tab to re-dock or float it (hover a panel's top-left corner triangle to show its tab). The arrangement is saved in `imgui.ini` next to the exe; RESET LAYOUT restores the default |

Mouse input over ImGui windows never reaches the camera. A drag that starts on a panel
stays with the panel.

## Layout

```
CMakeLists.txt            top-level build; warnings, WIN32 subsystem, asset copy, tests
cmake/Dependencies.cmake  FetchContent pins + imgui / implot static targets
external/glad/            pre-generated GL 3.3 core loader (tools/gen_glad.mjs)
assets/fonts/             JetBrains Mono Regular (OFL) + licence
assets/shaders/           body, line (grid), orbit (+ geometry shader), points (stars, Sun shell)
src/main.cpp
src/app/                  Application, Window, Input, Paths, Log, DPI manifest
src/sim/                  Vec3, Constants, OrbitalElements, Body, BodyTable, KeplerSolver,
                          SolarSystem, SimClock, OrbitProbe, EventDetector   (no GL / ImGui / GLM)
src/render/               SceneRenderer, Shader, SphereMesh, OrbitRenderer, Camera, ScaleMapper,
                          Picking, Starfield, EclipticGrid, PointCloud, LineMesh, GlColor
src/hud/                  Theme (style + TagChip, ReadoutBar, RingGauge, CornerBrackets, HeroNumber,
                          panel chrome), HudLayout (dockspace + default layout), HudState,
                          TitleBar, Controls, PlanetPanel, DataGrid, HistoryPlot, LogPanel,
                          SceneOverlay (labels, selection brackets)
tests/sim_tests.cpp       simulation checks
tests/render_tests.cpp    render geometry checks (no GPU)
tools/gen_glad.mjs        regenerates external/glad from Khronos gl.xml (not run by the build)
```

## Design notes

- **Physics in real units, doubles throughout.** Positions in AU, velocities in km/s,
  time in days since J2000.0. Rendering maps these through `ScaleMapper`. The
  subtraction `body − camera target` happens in double; only the small result
  becomes float, so following a body does not jitter.
- **Kepler pipeline:** M = L − ϖ + n·t → Newton on E − e·sinE = M (tol 1e-10, ≤ 15 iter,
  Danby start) → true anomaly → rotate by ω, i, Ω into the ecliptic. Speed from the analytic
  derivative, cross-checked against vis-viva in tests and in the debug panel.
- **μ = G·M_sun** uses the IAU nominal GM (1.32712440018e20 m³/s²). M_sun is derived
  from it, because GM is known about five orders of magnitude more precisely than G.
- **Compressed scale:** `d_render = 10 · log10(1 + 10 · d_AU)`, applied radially.
  Planet radii are `0.25 · (R/R⊕)^0.45` and the Sun is capped.
- **True scale:** 1 AU = 10 render units and real radii, so the Sun is 0.0465 units and
  Earth 0.00043. Every body keeps a 3.5 px minimum on-screen radius (Sun 6 px), so it stays
  visible and clickable.
- **Orbit lines** are widened in screen space by a geometry shader, because the core
  profile doesn't guarantee `glLineWidth > 1`. The selected orbit is 2.6 px, the rest 1.2 px.
- **Starfield:** 5,000 deterministic (PCG32) points at infinity, with 35% concentrated in a
  band tilted like the Milky Way.
- **DPI:** a manifest declares PerMonitorV2. The HUD style is rebuilt from an
  unscaled base whenever `glfwGetWindowContentScale` changes (`ScaleAllSizes` plus
  `FontScaleDpi`). ImGui 1.92 rasterises fonts at the final size, so text stays sharp.

## HUD notes

- **Layout:** a full-window dockspace with a pass-through central node. The 3D scene is
  rendered only into that node's rectangle, so panels never cover a planet, and picking and
  projection use the same rectangle.
- **Readout bars** are scaled to the range across the eight planets. Mass, radius, distance
  and period use a log scale, because they span orders of magnitude. The Sun's values fall
  outside that range and are shown in amber.
- **Event log:** 500-line cap, amber timestamps, alignment events in cyan. It auto-scrolls
  unless you scroll up. Selection, tracking and pause changes are logged whatever caused them:
  mouse, keyboard or panel.
- **Type scale:** 10.5 / 12 / 15 / 18 / 28 / 46 px. They are all rendered from the two
  JetBrains Mono faces (15 px and 28 px), because ImGui 1.92 rasterises any size on demand.
- The window opens maximised.

## Visual design

- **Palette** (`src/style/Palette.h`, the only place colours are defined). There are two
  accents:
  - WORLD gold `#C9A45C` (dim `#7A6A45`) for things in space: grid, Sun particles, scene
    labels and leader lines, log timestamps, histogram bars.
  - UI teal `#4FD1C5` for the instrument: panel chrome, buttons, and the selected target
    only (its orbit, rim light, brackets and label).

  Unselected orbits and planet rims are neutral blue-grey `#6F8499`. The 3D view `#070B10`
  is darker than the panels `#0B1520` at 92% alpha. Text has four levels: hero, value,
  label and micro.
- **HDR pipeline** (`render/PostProcess`):
  1. The 3D view renders into a 4× MSAA RGBA16F framebuffer.
  2. A blit resolves the multisampling.
  3. A bright pass downsamples to half resolution.
  4. Three rounds of separable 9-tap Gaussian blur.
  5. A composite adds the bloom, applies a soft shoulder, and optionally a vignette plus
     ~1.5% animated noise.

  The window's own framebuffer is single-sampled; ImGui antialiases its own shapes. If
  the driver rejects the offscreen targets, the view falls back to direct rendering
  without bloom.
- **Depth:** grid, orbits and Sun particles fade with distance from the camera.
  - Orbit trails are bright at the planet and dim ~300° behind it. The trail flips
    when time runs backwards.
  - The grid's gravity well (`k/sqrt(r²+s²)`) and Jupiter's dip are computed in
    `line.vert`.
  - When the Sun is large on screen it is a particle volume (~4,200 points). Density
    is biased toward the core, far-side particles are dimmer, and each particle flickers
    slightly.
- **Glow:** orbits and Sun particles blend additively. The selected orbit is a thin HDR core
  over a wide soft halo (screen-space quads from `orbit.geom`). The selection brackets and
  label leader lines are drawn additively through an ImGui draw callback.
- **Warm grade:** the composite also applies a colour grade after the shoulder and before
  the vignette. It lifts shadows toward the theme's shadow tint, pulls highlights toward
  near-white, and desaturates red-dominant colour above a limit, so REACTOR stays warm
  without going red.
- **Toggles** (MISSION CONTROL → LAYERS): BLOOM (bloom pass), FINISH (vignette + noise),
  and the WARM slider (grade strength 0–1, default 0.60). GRID controls the gravity well
  and the whole reactor structure together: rings, ticks, spokes, fragments, markers and
  ring captions.

## HUD themes

Every HUD colour, size, font and component choice is a token in one struct,
`hud::HudTheme` (`src/hud/HudTheme.h/.cpp`); nothing else in `src/hud` defines a colour.
Switch themes with **F2** or MISSION CONTROL → HUD THEME.

- **REACTOR** (default): warm monochrome on near-black.
  - Colours: orange accent, red reserved for alarms, peach values, rust 1 px lines.
  - Type: tracked titles (a second JetBrains Mono instance with
    `ImFontConfig::GlyphExtraAdvanceX`, rebuilt when the DPI changes because ImGui applies
    that advance unscaled).
  - Components:
    - Inverted status chips; flat outline buttons; square checkboxes; 3 px scrollbars.
    - A dashed callout around the TARGET distance.
    - STATE VECTOR as a dense table with PERI/APO alarm chips.
    - Tall gradient bar gauges for the orbit gauges and the orbit phase.
    - Thin meters on the TARGET tiles; a dotted plot grid.
    - Event-log alarm markers.
  - Chrome: full-width top and bottom status strips, and a faint scanline texture over the panels.
- **OBSERVATORY**: the previous teal/gold look.

### 3D scene

The scene draws from `style::SceneStyle` (`src/style/SceneStyle.h`), which each theme
carries as `HudTheme::scene`. The render code has no colours of its own; planets keep
their natural colours.

| Token | REACTOR | OBSERVATORY | Used for |
|---|---|---|---|
| bgCenter / bgMid / bgEdge | `#14100D` / `#0B0908` / `#060504` | `#0C141C` / `#070B10` / `#04070A` | radial background + vignette |
| gridDim | `#3A2E27` | world gold | gravity-well grid (secondary) |
| ringDim / ringMid / structure | `#3A2E27` / `#6A5344` / `#C8955E` | `#1E3238` / `#2F5A60` / `#4FA8A0` | minor rings / major rings, plain orbits / ticks, crown, markers, captions |
| coreHot / coreMid / coreFalloff | `#FFF1DC` / `#FFC27A` / `#E8894A` | `#FFF6E0` / `#FFD9A0` / world gold | Sun heat ramp |
| accent | `#FF6A3D` | UI teal | selected orbit, brackets, target label, data tag, PERI/APO |
| alert | `#E4281A` | warn | events only |
| starCool / starMid / starWarm | `#D9DCE2` / `#EDE3D6` / `#F4D6B4` | cool / white / warm | star temperature ramp |
| labelLine / labelBorder / labelText / labelBox | amber / `#6A5344` / HUD value / bg | gold / gold / gold / bg | scene labels |
| gradeShadow / gradeHighlight / gradeRedLimit | `#2A1A10` / `#FFF4E6` / 0.55 | `#0A1A22` / `#F0F8FF` / 0.80 | composite grade |

Alphas (rings, ticks, spokes, fragments, markers, halo, plain orbits) are tokens too.

- **Reactor structure** (`render/ReactorStructure`, a pure generator; static VBOs rebuilt
  only when the scale mode changes):
  - A range ring at each planet's semi-major axis, plus faint intermediate rings and an
    outer rim.
  - Ticks every 10° (every 30° twice as long) pointing outward.
  - 12 dashed radial spokes, plus 40 seeded fragments and 60 dot markers.
  - Everything fades with depth and over the outer 15%.
  - Ring captions ("1.00 AU") are staggered in angle. A caption that would touch a body
    label, the brackets, the data tag or another caption is skipped.
- **Sun as a reactor core:**
  - Particles take their colour from the core ramp.
  - Three billboarded halo rings pulse ±10% over 6 s.
  - A crown of short ticks rotates slowly around the core.
- **Orbits:** unselected orbits are drawn in ringMid at low alpha, with the trail kept. The
  selected orbit is drawn in the accent (core + halo), with PERI/APO diamonds and micro labels.
- **Target:** accent brackets and label, plus a leader line to a data tag
  (`r 1.0165 AU · v 29.3 km/s`). The tag flips to the left at the view's right edge.

## Dev hooks (automated screenshots)

Set environment variables before launching to script a view and capture it. These are
used to check visuals without a person at the screen:

| Variable | Effect |
|---|---|
| `SOLSIM_SCREENSHOT=out.bmp` | Save the frame after `SOLSIM_SCREENSHOT_FRAMES` (default 90) frames, then exit |
| `SOLSIM_TRUE_SCALE=1` | Start in true scale |
| `SOLSIM_SELECT=SATURN` | Select a body; `SOLSIM_TRACK=1` also tracks it |
| `SOLSIM_TIME_SCALE=365` | Initial time scale (days/s) |
| `SOLSIM_CAMERA=dist,yawDeg,pitchDeg` | Camera placement |
| `SOLSIM_THEME=OBSERVATORY` | Start with the original teal HUD |
| `SOLSIM_EARTH=1` | Start in the Earth view (screenshots wait for the first NEO query result) |
| `SOLSIM_NEO_TOPK=n`, `SOLSIM_NEO_MAXLD=ld`, `SOLSIM_NEO_PHA=1`, `SOLSIM_NEO_FROM/TO=YYYY-MM-DD` | The NEO FILTER of that first query |
| `SOLSIM_NEO_SELECT=i`, `SOLSIM_NEO_HOVER=i` or `any` | Select / show as hovered result `i` |
| `SOLSIM_CLICKS="frame:x:y[:shift\|ctrl];..."` | Inject left clicks at window pixels on given frames, to drive the panels in scripted runs |
| `SOLSIM_EARTH_DISPLAY=swarm\|paths` | The Earth view's display: `swarm` (default, all flybys at once) or `paths` (real dates) |
| `SOLSIM_NEO_CHECKS=none`, `first:N`, `every:N` | Start with only some NEO RESULTS rows checked (drawn) |
| `SOLSIM_EARTH_ENTER=n`, `SOLSIM_EARTH_LEAVE=n` | Enter / leave the Earth view at frame `n` (with `SOLSIM_SELECT=EARTH`) |
| `SOLSIM_NEOS=pha\|1000\|5000\|20000\|all\|result` | Turn the NEOS layer on with that preset; `SOLSIM_NEOS_LEGEND=distance\|pha\|diameter\|approach`, `SOLSIM_NEOS_SELECT=i`, `SOLSIM_NEOS_DIRECT=n` (propagate directly up to n objects; 0 = always on the worker) |
| `SOLSIM_NEO_DB=path` | The NEO database (default: `data/neo.db` found above the executable) |
| `SOLSIM_VSYNC=0` | Vsync off, for measuring what a frame costs |
