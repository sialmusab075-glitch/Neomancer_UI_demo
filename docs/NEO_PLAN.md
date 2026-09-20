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
| nlohmann/json | 3.12.0 | vendored single header, `external/json/nlohmann/json.hpp` | MIT |
| SQLite amalgamation | 3.53.4 | vendored `external/sqlite/sqlite3.c/.h`, built as a silenced target | public domain |
| WinHTTP | Windows SDK | system library, behind `IHttpClient` | system component |

Hashes verified against the publishers and recorded in `THIRD_PARTY.md`. Nothing
is downloaded at build time.

## 3. Data model

`src/neo/model`, as implemented in stage 2.

```
Asteroid              pdes (canonical key), spkid, name, fullName,
                      PhysicalProperties, OrbitalProperties, ObjectClassification
PhysicalProperties    all optional: diameterKm (measured only), diameterSigmaKm,
                      absoluteMagnitudeH, albedo, rotationPeriodHours
                      + estimatedDiameterKm() and bestDiameterKm(), both derived
OrbitalProperties     orbitId, epochJdTdb, eccentricity, semiMajorAxisAU,
                      perihelionAU, inclinationDeg, ascendingNodeDeg,
                      argPerihelionDeg, meanAnomalyDeg, meanMotionDegPerDay,
                      optional periodDays / moidAU / conditionCode,
                      propagationSupported()
ObjectClassification  kind (Asteroid/Comet/Unknown), numbered,
                      isNEO / isPHA as std::optional<bool>, orbitClass
CloseApproach         objectIndex, jdTdb, distanceAU, distanceMinAU, distanceMaxAU,
                      relVelocityKms, vInfinityKms, optional H / diameter / sigma
ParsedApproach        designation + orbitId + CloseApproach, before the join
AsteroidRecord        Asteroid + [firstApproach, firstApproach + approachCount)
Dataset               records() + approaches() + find(pdes) / findBySpkId(spkid)
                      + approachesOf(index) -> ApproachSpan
```

`CloseApproach` is 104 bytes on this toolchain (measured in `neo_tests`) and
trivially copyable, with no strings: there can be millions of them, and every
index built later stores `uint32_t` offsets into the one flat vector. At ~104 B
per row, 1M approaches cost ~104 MB, which is the figure the stage 7 memory
experiment reports; the three `std::optional<double>` members are 48 B of that
and are the first thing to pack if it ever matters.

### Field mapping: SBDB Query API -> C++

Requested with `full-prec=1`. Everything arrives as a JSON string or null,
except `spkid`, which is a JSON number.

| JPL field | C++ member | Unit / type | Notes |
|---|---|---|---|
| `pdes` | `Asteroid::pdes` | string | canonical key; a row without it is rejected |
| `spkid` | `Asteroid::spkid` | string | secondary index; numeric in the payload |
| `name` | `Asteroid::name` | string | empty when unnamed (18 of 23 fixture objects) |
| `full_name` | `Asteroid::fullName` | string | kept verbatim, leading spaces included |
| `kind` | `classification.kind`, `.numbered` | enum + bool | `an` / `au` / `cn` / `cu` |
| `neo` | `classification.isNEO` | `optional<bool>` | `Y`/`N`/null; null stays empty |
| `pha` | `classification.isPHA` | `optional<bool>` | null for comets |
| `class` | `classification.orbitClass` | string | `APO`, `ATE`, `AMO`, `HYA`, `ETc`, ... |
| `orbit_id` | `orbital.orbitId` | string | solution id (`659`, `JPL 16`) |
| `epoch` | `orbital.epochJdTdb` | JD (TDB) | epoch of osculation |
| `e` | `orbital.eccentricity` | — | `>= 1` means no propagation |
| `a` | `orbital.semiMajorAxisAU` | AU | negative for hyperbolic orbits |
| `q` | `orbital.perihelionAU` | AU | |
| `i` | `orbital.inclinationDeg` | deg | to the ecliptic |
| `om` | `orbital.ascendingNodeDeg` | deg | Omega |
| `w` | `orbital.argPerihelionDeg` | deg | omega |
| `ma` | `orbital.meanAnomalyDeg` | deg | at `epoch`, not at J2000 |
| `n` | `orbital.meanMotionDegPerDay` | deg/day | used as published for the epoch shift |
| `per` | `orbital.periodDays` | days, optional | null for hyperbolic orbits |
| `moid` | `orbital.moidAU` | AU, optional | Earth MOID |
| `condition_code` | `orbital.conditionCode` | int 0–9, optional | 0 = best determined |
| `H` | `physical.absoluteMagnitudeH` | mag, optional | comets usually have none |
| `diameter` | `physical.diameterKm` | km, optional | **measured only** |
| `diameter_sigma` | `physical.diameterSigmaKm` | km, optional | |
| `albedo` | `physical.albedo` | —, optional | used by the estimate when present |
| `rot_per` | `physical.rotationPeriodHours` | h, optional | |

A row is rejected (counted, reason recorded, never guessed at) when `pdes` is
missing or when any of `epoch, e, a, q, i, om, w, ma, n` is missing or
non-numeric, because such a row can be neither keyed nor propagated.

### Field mapping: Close Approach Data API -> C++

Requested with `diameter=true&fullname=true`.

| JPL field | C++ member | Unit / type | Notes |
|---|---|---|---|
| `des` | `ParsedApproach::designation` | string | joins to `pdes`, then dropped |
| `orbit_id` | `ParsedApproach::orbitId` | string | dropped after the join (see below) |
| `jd` | `CloseApproach::jdTdb` | JD (TDB) | required |
| `cd` | — | — | not stored; formatted from `jd` on display |
| `dist` | `distanceAU` | AU | required |
| `dist_min` | `distanceMinAU` | AU | falls back to `dist` if absent |
| `dist_max` | `distanceMaxAU` | AU | falls back to `dist` if absent |
| `v_rel` | `relVelocityKms` | km/s | relative to the approach body |
| `v_inf` | `vInfinityKms` | km/s | falls back to `v_rel` if absent |
| `t_sigma_f` | — | — | not stored in v1 |
| `h` | `absoluteMagnitudeH` | mag, optional | |
| `diameter`, `diameter_sigma` | `diameterKm`, `diameterSigmaKm` | km, optional | null when unknown |
| `fullname` | — | — | not stored; SBDB `full_name` is authoritative |

CAD's `orbit_id` is dropped after the join: the object already stores the
current solution id, and a string per approach would cost ~32 bytes times
millions of rows for information no query uses.

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
| 2 | Model + JSON parsing from fixtures + tests | **done** (97 checks in `neo_tests`) |
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

### Stage 2 — decisions recorded

1. **Signature versions** are per API and compiled in: SBDB `1.0`, CAD `1.5`.
   A different version fails the whole payload with an error naming both
   versions and the API doc URL. The check is overridable in `ParseOptions` so
   a test can prove the rejection comes from the check, not from bad data.
2. **Parsing never throws across its boundary.** A bad payload is an expected
   outcome (truncated download, error document, changed version), so it returns
   a `ParseStatus`. Row-level defects are counted in a `ValidationReport`
   (per-column null counts, rejection reasons, capped samples) and never abort
   a page.
3. **nlohmann/json is confined** to `src/neo/ingest/detail/JsonTable.*`, which
   only `.cpp` files include. No public header exposes it.
4. **Absent is not zero, but missing optional CAD columns get a documented
   fallback:** `dist_min`/`dist_max` fall back to `dist`, `v_inf` to `v_rel`,
   so a range query can never match a spurious 0 AU or 0 km/s. Physical values
   (diameter, H, albedo) never get a fallback; they stay empty.
5. **The join** resolves designations, then sorts the matched rows once by
   `(objectIndex, jdTdb)`. That single O(n log n) sort produces both the
   contiguous per-record range and chronological order inside it. Unmatched
   rows are dropped and reported with per-designation counts (sample capped at
   20, true total kept).
6. **Estimated diameter** uses a measured albedo when SBDB has one, otherwise
   `p = 0.14`. `bestDiameterKm()` prefers the measured value; the two are never
   merged in storage.
7. `Dataset::find()` currently wraps `std::unordered_map`. Stage 5 swaps in
   `neo::HashMap` behind the same signature; callers only ever see indices.

### JPL API behaviour found while building the fixtures

- A **top-level `"OR"`** in `sb-cdata` returns HTTP 502. The documented nested
  form, `{"AND":[{"OR":[...]}]}`, works.
- `EQ` **rejects spaces** in an argument, so designations such as `2020 AN3`
  must be matched with `RE` (`pdes|RE|^2020.AN3$`).
- `spkid` is a **JSON number**; every other SBDB value is a string or null.
- Broad filters (`H|ND` over the whole NEO group) and even some ordinary window
  queries return **intermittent 502s**. The ingest tool must retry with backoff
  and cache successful responses.

### Fixtures (`tests/fixtures/`, each with a `.meta.json`)

| File | Rows | What it covers |
|---|---|---|
| `sbdb_neo_page.json` | 23 objects | PHAs, measured diameter (433, 2P), null diameter (15), null H and comet (2P), hyperbolic with negative `a` and null `per` (2017 U1), null NEO/PHA flags |
| `cad_pha_window.json` | 22 approaches | a realistic one-year PHA window; 3 designations deliberately absent from the SBDB fixture |
| `cad_apophis.json` | 28 approaches | one object with many encounters, including 2029-04-13 at 0.000254 au |
| `sbdb_bad_signature.json` | 2 objects | synthetic: `signature.version` changed to `9.9`, must be refused |
