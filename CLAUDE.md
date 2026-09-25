# SOL SYSTEM SIM: working notes

C++17, OpenGL 3.3 core, GLFW, GLM, Dear ImGui (docking) + ImPlot, Windows only.
A semester DSA project (NEO-DX / ASTRODSA) lives inside a solar-system simulator.
The graded part is the data + DSA + query layer (src/neo); the UI is secondary.

## Read first
- docs/NEO_PLAN.md  (design, decisions, measured numbers; keep it updated)
- docs/NEO_DSA_Context.md, docs/DSA_NOTES.md

## Build and test
Target toolchains: MSVC (primary) and GCC via WinLibs + Ninja.
  cmake -B build-gcc -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++
  cmake --build build-gcc
  ctest --test-dir build-gcc        # 9 suites, must all pass before every commit
Must be warning-free under GCC -Wall -Wextra -Wpedantic and MSVC-clean by construction.
If SolSystemSim.exe is locked (the app is running), never kill it: link a
separate exe name for testing instead.

## Architecture rules
- src/sim: pure C++ (Kepler solver, clock). src/neo: pure C++ (model, ingest, storage,
  DSA, query engine, Earth flybys, NEOS swarm). Neither may include OpenGL/ImGui/GLM.
- src/render, src/hud, src/app: rendering, HUD panels, application glue.
- SQLite is storage only: queries run on the in-memory DSA structures.
- The UI never parses JSON, uses SQL or touches the network. It only calls
  QueryEngine::run (through NeoService, on a worker thread).
- Logic that decides WHERE/WHAT (mappings, selection, propagation) goes in pure
  neo/sim code with tests; the renderer and HUD only draw it.

## Workflow
- Conventional commits, one concern each. Tests for new logic (compare against
  sim::propagate or a closed form where possible).
- No network in build or tests. No new dependency without asking (licence check).
- data/ (neo.db, cache) is NOT in git. Regenerate with the neo_ingest tool
  (polite paging, resumable) or copy the file.
- Keep the repo PRIVATE. Check before pushing.
- Update docs/NEO_PLAN.md after each feature, including real vs schematic.
- UI changes: verify by screenshot using the SOLSIM_* dev hooks listed in README.md
  (SOLSIM_SCREENSHOT, SOLSIM_EARTH=1, SOLSIM_CLICKS, ...).

## Current state (done)
Ingest -> SQLite -> DSA -> query engine/planner -> Earth view (schematic flybys,
NEO FILTER, results table with checkboxes, PATHS/SWARM display) -> NEOS layer in
the solar view (presets, worker-thread propagation, shared selection).

## Known open items
- Stage 7 not started: neo_bench + CSV + results summary, including the experiment
  of a HashMap keyed by record index (memory comparison).
- Earth view has no legend/colour modes yet (the solar NEOS layer does).
- Real mouse interaction (shift/ctrl clicks in the results table) was only tested
  through scripted clicks.
- Double-click to track an asteroid with the camera is not done.
