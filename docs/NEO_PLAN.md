# NEO-DX / ASTRODSA — implementation plan

Working plan for the data + DSA + query layer built inside SOL SYSTEM SIM.
Companion to `docs/NEO_DSA_Context.md` (the project context document); where the
two disagree, this file wins, because it records decisions taken after review.

Updated at the end of every stage: decisions, complexity notes, and what shipped.

---

## 1. Scope and division of responsibility

The graded core is the data layer, the data structures, and the query engine.
The existing renderer and HUD are the presentation layer and stay thin.

```
JPL SBDB + CAD (JSON over HTTPS)
  -> tools/neo_ingest         download, page, cache raw responses, validate
  -> src/neo/ingest           parse + validate into domain types
  -> src/neo/storage          neo.db (SQLite), versioned schema
  -> src/neo/dsa              my own hash map, merge sort, heap, AVL, buckets
  -> src/neo/query            filters, ranges, top-K, planner
  -> src/neo/sim              records -> scene (orbits, encounter markers)
  -> src/hud                  NEO QUERY panel: asks the query layer, draws results
```

Hard rules:

- The UI never parses JSON and never touches the network. It calls the query layer.
- One master `std::vector<AsteroidRecord>`. Every index stores `std::uint32_t`
  positions into it, never copies of records.
- Unknown is not zero. Optional physical values are `std::optional<double>`, and
  each query states explicitly how unknowns are treated.
- No network access during the build or during tests. Tests read saved fixtures.

## 2. Build integration

New static library `solsim_neo` (no OpenGL, no ImGui, no GLM), mirroring how
`solsim_sim` is already separated, so the CLI tools and the tests link it
without creating a window.

| Target | Contents |
|---|---|
| `solsim_neo` | `src/neo/{model,ingest,storage,dsa,query,sim}` — parsing, storage, DSA, queries |
| `solsim_neo_net` | WinHTTP implementation of `IHttpClient`; only `neo_ingest` links it |
| `tools/neo_ingest` | download + validate + write `neo.db` |
| `tools/neo_bench` | benchmarks -> CSV |
| `tests/neo_tests` | ctest target; fixtures only, no network |

Toolchain: C++17, warning-free under GCC `-Wall -Wextra -Wpedantic`. MSVC is not
installed on this machine, so MSVC cleanliness is kept by construction: no GCC
extensions, no variable-length arrays, explicit casts, no anonymous structs.

### Dependencies (approved, stage 1)

| Dependency | Version | How | Licence |
|---|---|---|---|
| nlohmann/json | 3.12.0 | vendored single header, `external/json/json.hpp` | MIT |
| SQLite amalgamation | 3.53.4 | vendored `external/sqlite/sqlite3.c/.h`, built as a silenced target | public domain |
| WinHTTP | Windows SDK | system library, behind `IHttpClient` | system component |

Hashes verified against the publishers and recorded in `THIRD_PARTY.md`. Nothing
is downloaded at build time.

## 3. Data model

`src/neo/model`, following the context document, with the review fixes applied.

```
DateTime          Julian Date (TDB) as a double + display formatting
PhysicalProperties  optional: diameterKm (measured), diameterSigmaKm, H, albedo,
                    rotationPeriodHours; plus estimatedDiameterKm (derived from H)
OrbitalProperties   orbitId, epochJd, e, a_AU, q_AU, i, om, w, ma, n, period, moid_AU
ObjectClassification kind (asteroid/comet), isNEO, isPHA, orbitClass
Asteroid          pdes (canonical), spkid, fullname, physical, orbital, classification
CloseApproach     objectIndex, orbitId, jdTdb, distAU, distMinAU, distMaxAU,
                  vRelKms, vInfKms, optional H, optional diameter
AsteroidRecord    Asteroid + [firstApproach, approachCount) into the flat approach vector
```

### Identifiers and the join

- **Canonical key: `pdes`** (primary designation). SBDB `pdes` and CAD `des` use
  the same designation string, so the join is direct. `spkid` is kept as a
  secondary index for cross-checks and for Sentry later.
- CAD rows whose `des` matches no object are counted, sampled into the validation
  report, and dropped; they are never silently ignored.

### Diameter policy

`measured` and `estimated` are separate fields and never merged:

```
D_km(estimated) = 1329 / sqrt(p) * 10^(-H/5),  p = 0.14 when albedo is unknown
```

Queries carry a policy flag: measured only, measured + estimated, and whether
unknowns are excluded (the default) or kept.

### Dates

Stored as Julian Date (TDB) doubles. Display goes through the existing
`sim::calendarFromJulianDate`. Simulation time is days since J2000, so
`t_days = jd - 2451545.0`.

### Orbit propagation (reuses the existing solver, `src/sim` untouched)

SBDB publishes elements at each object's **own epoch**; `sim::propagate` expects
elements at J2000 in the `(a, e, i, Omega, varpi, L)` form. The conversion is
exact for two-body motion:

```
Omega = om
varpi = om + w
M(J2000) = ma - n * (epoch_jd - 2451545.0)      n = SBDB mean motion, deg/day
L = varpi + M(J2000)
```

`n` is requested from SBDB rather than derived from `a`, so the published value
is used as-is. Two-body propagation far from the epoch ignores planetary
perturbations, non-gravitational forces and close-encounter deflection: the
drawn orbit is a visual aid, not an ephemeris. Objects with `e >= 1` are stored
but flagged unsupported for propagation (the Newton solver assumes `0 <= e < 1`).

## 4. Ingestion

`tools/neo_ingest`, sequential and polite:

- SBDB: `sb-group=neo`, explicit field list, paged with `limit` / `limit-from`.
- CAD: explicit `date-min`, `date-max`, `dist-max`, `diameter=true`,
  `fullname=true`, paged the same way.
- **v1 defaults: dates 1950-01-01 .. 2150-01-01, `dist-max` 0.05 AU.** The wider
  1900-01-01 .. 2200-01-01 and 0.2 AU stay available as flags.
- A count-first request runs before downloading; above a row threshold the tool
  stops unless `--yes` is passed.
- Every raw response is cached on disk keyed by its request, so re-runs parse
  from the cache instead of re-downloading.
- `signature.version` is checked in every response; an unknown version fails loudly.
- A short delay between pages.
- Validation report: per-field null counts, row counts, unmatched CAD
  designations, and rejected rows with reasons.

## 5. DSA layer

Each structure is mine, header + `.cpp`, with the `std::` equivalent behind the
same interface so `neo_bench` can compare them. Complexity is documented in each
header (and repeated here for the report).

| Structure | Operations | Time | Space | Why it is here |
|---|---|---|---|---|
| `HashMap<K,V>` (open addressing, Robin Hood, backward-shift delete) | insert / find / erase | O(1) average, O(n) worst | O(capacity) | `pdes -> index` exact lookup; open addressing is cache-friendly and Robin Hood bounds probe-length variance; backward-shift deletion avoids tombstone build-up |
| merge sort (stable) + own `lowerBound` / `upperBound` | sort / bound search | O(n log n) / O(log n) | O(n) scratch | sorted views by diameter, H, MOID, a, e, i and by approach date, distance, velocity; stability keeps ties in designation order so results are reproducible |
| `BinaryHeap<T, Compare>` | push / pop / top-K | O(log k) per element, O(n log k) total | O(k) | top-K closest / fastest / largest without sorting the whole candidate set |
| AVL tree | insert / range query | O(log n) / O(log n + m) | O(n) | ordered index over approach dates. **Chosen over an interval tree** because CAD encounters are instants, not intervals: a date window is two bound searches plus an in-order walk. An interval tree only earns its extra complexity when records have duration and can overlap |
| Bucket index | bucket lookup | O(1) bucket, O(m) scan | O(n) | application-defined size classes (<10 m, 10–50, 50–140, 140–1000, >1 km) and year buckets for approaches; matches the UI filters directly |

Baselines kept for comparison: `std::unordered_map`, `std::sort`,
`std::priority_queue`, `std::map`.

## 6. Query engine

- `Query`: kind, NEO/PHA flags, orbit class, diameter range + unknown policy, H
  range, MOID range, approach date window, distance max, velocity range, sort
  field, top-K.
- Per-field histograms are built at load and used to estimate selectivity.
- The planner applies the most selective indexed filter first, then scans the
  candidates.
- A `naive` path (full linear scan) exists for tests and benchmarks.
- Results: record indices + matching approach indices, plus stats (candidates
  examined, elapsed time, plan chosen).

## 7. Tests (ctest, no network)

- Parsing against saved fixtures: a small real SBDB page and a small real CAD page.
- Missing-field cases: null diameter, missing albedo, a comet with no H.
- Oracle tests: randomised queries with a fixed seed; every indexed path must
  return exactly what the linear scan returns.
- DSA unit tests: hash map resize/erase behaviour, heap ordering, AVL balance
  invariants, sort stability.

## 8. Benchmarks

`tools/neo_bench` writes CSV. Sizes 1k, 10k, all real NEOs, plus synthetic 100k
and 500k generated from the real field distributions and clearly labelled
synthetic. Each timing repeated at least 5 times, median reported,
`std::chrono::steady_clock`.

Experiments: exact lookup (linear vs mine vs `unordered_map`), range query
(linear vs sorted view + binary search vs AVL), top-K (full sort vs my heap vs
`priority_queue`), combined query (fixed order vs planner), ingestion time by
page size, and memory for the master vector and each index.

## 9. UI hook (last, deliberately thin)

- `neo.db` is loaded on a worker thread at startup; the top status strip shows
  `NEO DB: N objects / M approaches`.
- A `NEO QUERY` panel in the current theme: filter widgets, a results table
  (designation, D, H, next approach, min distance, v_rel) and the query stats.
- Selecting a result propagates its orbit with the existing Kepler solver from
  its own epoch, draws a dim orbit plus a marker, and fills TARGET · OBSERVATION.
- Close approaches inside the window appear as timeline markers.

## 10. Stage status

| Stage | Goal | Status |
|---|---|---|
| 1 | Plan + dependencies + licences | **done** |
| 2 | Model + JSON parsing from fixtures + tests | not started |
| 3 | `neo_ingest`: real download, paging, cache, validation report | not started |
| 4 | SQLite schema + save/load + tests | not started |
| 5 | DSA structures + unit tests | not started |
| 6 | Query engine + planner + oracle tests | not started |
| 7 | `neo_bench` + CSV + results summary | not started |
| 8 | UI hook | not started |

### Stage 1 — decisions recorded

1. Canonical key `pdes`; `spkid` secondary; unmatched CAD rows counted and reported.
2. Ordered index is an AVL tree, justified above against an interval tree.
3. `HashMap` uses open addressing with Robin Hood probing and backward-shift
   deletion; FNV-1a over the designation; resize at load factor 0.75.
4. Measured and H-estimated diameters stay separate; every query carries a policy.
5. Approaches live in one flat vector; each record keeps a `[first, first+count)`
   range, so date indexes can point straight at approaches.
6. SBDB `n` is requested and used for the epoch shift instead of deriving it from `a`.
7. CAD v1 window 1950–2150 at 0.05 AU, with the wider range behind flags.
8. Dependencies pinned: nlohmann/json 3.12.0, SQLite 3.53.4, WinHTTP.
