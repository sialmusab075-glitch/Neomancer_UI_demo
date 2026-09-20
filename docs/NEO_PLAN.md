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

- **SQLite is storage only.** `neo.db` is how the dataset survives between runs,
  and nothing more. Every query in stage 6 runs on the in-memory DSA structures;
  no part of the query path issues SQL. SQLite may reappear in `neo_bench` as a
  baseline to measure my structures against, and nowhere else. The schema
  therefore carries only the indexes integrity needs (primary keys and the
  UNIQUE designations) and no query indexes.
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

`CloseApproach` is 120 bytes on this toolchain (measured in `neo_tests`) and
trivially copyable, with no strings: there can be millions of them, and every
index built later stores `uint32_t` offsets into the one flat vector. At 120 B
per row, 1M approaches cost ~120 MB, which is the figure the stage 7 memory
experiment reports; the four `std::optional<double>` members are 64 B of that
(16 B each for 8 B of payload) and are the first thing to pack if it matters.

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
| `dist_min` | `distanceMinAU` | AU | falls back to `dist`, row flagged `distRangeDerived` |
| `dist_max` | `distanceMaxAU` | AU | same fallback and flag |
| `v_rel` | `relVelocityKms` | km/s | relative to the approach body |
| `v_inf` | `vInfinityKms` | km/s, optional | **never** filled in from `v_rel`: different quantity (`v_inf < v_rel`) |
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

## 4. Ingestion (stage 3, implemented)

`tools/neo_ingest` is a command-line tool; the UI never runs it. Pipeline:
`IHttpClient` -> `Fetcher` (politeness, retry, cache) -> `Ingestor` (paging,
windows, progress) -> parsers -> `Dataset` -> `IngestReport`.

### Politeness

- Strictly sequential requests, at least **1 s apart** (`--min-interval`).
- A `User-Agent` naming the project, and 15 s connect / 60 s receive timeouts.
- **Retries only on 5xx and timeouts**, at most 5 attempts (`--max-attempts`),
  with exponential backoff (1, 2, 4, 8 s, capped at 30 s) plus up to 50% seeded
  jitter. A 4xx is the request's own fault and is never retried.
- A page that exhausts its attempts fails the run with the URL in the message.

### Cache and offline

Every raw response is stored as `<fnv1a64(url)>.json` plus a
`.meta.json` holding the exact URL, status, byte count and fetch time. The URL
is verified on load, so a hand-edited or colliding entry is ignored rather than
served as the wrong answer. `--offline` runs entirely from the cache (a miss is
an error); `--refresh` ignores what is cached and downloads again.

### Resume

A progress file (`data/ingest_progress.json`) records the next SBDB record
offset and the next CAD window start, keyed by a signature of the run
parameters; a file from different parameters is ignored. Re-running after an
interruption replays the completed pages from the cache (no network) and
continues from the mark. In `--refresh` mode the mark also means "this run
already refreshed these pages", so a resumed refresh does not download them
twice.

### SBDB

`sb-group=neo`, the explicit field list (including `epoch` and `n`),
`full-prec=1`, `sort=spkid` for deterministic paging, and `limit` /
`limit-from`. A count-first request (no `fields`) prints the total before any
bulk download.

### CAD

Defaults `1950-01-01 .. 2150-01-01`, `dist-max=0.05`, `neo=true`,
`diameter=true`, `fullname=true`. The wider 1900..2200 at 0.2 AU is available
through the flags.

**Time windows instead of offset paging.** CAD documents `limit-from` as
1-based while SBDB documents it as 0-based, so offset paging there is a
correctness risk. Instead the range is cut into `--window-years` windows
(default 5) and each window is fetched in one request with `limit`. The
response reports `total`; when `total` exceeds the rows returned, the window is
too wide and is **split in half** and retried. The API itself therefore decides
the granularity, and no offset arithmetic is involved.

Window edges are inclusive at both ends, so a row landing exactly on a boundary
can come back from two neighbouring windows. Exact repeats (same designation,
same instant) are dropped once, and counted in the report.

A count-first `total-only=true` request runs before any window; above
`--threshold` rows (default 500,000) the run stops unless `--yes` is given.

### Report

`data/ingest_report.json` and `data/ingest_report.txt`: run mode and timing,
the request parameters, per-API signature versions, rows seen/accepted/rejected
with reasons, null counts per field, derived distance ranges, window and split
counts, duplicates dropped, the full join result with unmatched designations,
dataset composition, and fetch statistics (requests, cache hits, retries,
failures, bytes, network time, polite waiting time).

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
| 3 | `neo_ingest`: real download, paging, cache, validation report | **done** (85 checks in `neo_ingest_tests`; one real run recorded below) |
| 4 | SQLite schema + save/load + tests | **done** (79 checks in `neo_storage_tests`) |
| 5 | DSA structures + unit tests | **done** (133 checks in `neo_dsa_tests`; study notes in `docs/DSA_NOTES.md`) |
| 6 | Query engine + planner + oracle tests | **done** (298 checks in `neo_query_tests`; 12,200 random queries, 0 mismatches) |
| 7 | `neo_bench` + CSV + results summary | not started |
| 8 | UI hook | **done**: Earth view (section 14) and the solar-view NEOS layer (section 15) |

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
4. **Absent is not zero.** One fallback survives, and it is recorded: when
   `dist_min`/`dist_max` are missing the nominal `dist` stands in, the row sets
   `distRangeDerived` and the `ValidationReport` counts it, so a distance range
   query cannot match a spurious 0 AU and the substitution is never invisible.
   `v_inf` gets no fallback: it is a different quantity from `v_rel` (it
   excludes the Earth-s gravitational focusing, so `v_inf < v_rel`), so it is
   `std::optional` and stays empty. Physical values (diameter, H, albedo) stay
   empty too.
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

### JPL API behaviour found against the live API

- A **top-level `"OR"`** in `sb-cdata` returns HTTP 502. The documented nested
  form, `{"AND":[{"OR":[...]}]}`, works.
- `EQ` **rejects spaces** in an argument, so designations such as `2020 AN3`
  must be matched with `RE` (`pdes|RE|^2020.AN3$`).
- `spkid` is a **JSON number**; every other SBDB value is a string or null.
- Broad filters (`H|ND` over the whole NEO group) and even ordinary window
  queries return **intermittent 502s**. The real ingest run needed 29 retries
  for 69 requests, and one window exhausted 5 attempts before succeeding on a
  later run: retry with backoff plus the response cache is not optional.
- **`total-only=true` puts the number in `total` and sets `count` to 0**, unlike
  an ordinary response where `count` is the row count. Reading `count` there
  silently reports zero rows (found and fixed during the first real run).
- SBDB's `sb-group=neo` **excludes hyperbolic objects**: the live run returned 0
  objects with `e >= 1`. 1I/'Oumuamua is only in the fixture because it was
  requested by designation. The propagation guard still matters for comets and
  for any future widening of the query.
- CAD `neo=true` selects exactly the population `sb-group=neo` returns: the real
  run joined **42,819 of 42,819 rows with zero unmatched**.

### Fixtures (`tests/fixtures/`, each with a `.meta.json`)

| File | Rows | What it covers |
|---|---|---|
| `sbdb_neo_page.json` | 23 objects | PHAs, measured diameter (433, 2P), null diameter (15), null H and comet (2P), hyperbolic with negative `a` and null `per` (2017 U1), null NEO/PHA flags |
| `cad_pha_window.json` | 22 approaches | a realistic one-year PHA window; 3 designations deliberately absent from the SBDB fixture |
| `cad_apophis.json` | 28 approaches | one object with many encounters, including 2029-04-13 at 0.000254 au |
| `sbdb_bad_signature.json` | 2 objects | synthetic: `signature.version` changed to `9.9`, must be refused |

### Stage 3 — decisions recorded

1. **Networking is isolated.** `IHttpClient` is the only door to the network and
   `WinHttpClient` lives in its own target (`solsim_neo_net`) that only
   `tools/neo_ingest` links. The tests and the application link `solsim_neo`,
   which contains no HTTP implementation at all, so no test can reach the
   network even by mistake.
2. **CAD is paged by time, not by offset** (see above): the window splits itself
   in half whenever the response says it carries more rows than it returned.
3. **Retry only what deserves it:** 5xx and timeouts, never 4xx.
4. **`--skip-sbdb` keeps the dataset's existing objects.** Replacing them with
   an empty vector would make every CAD row unmatched; this is what lets stage 4
   load objects from `neo.db` and fetch only new approaches. (Caught by the
   window-split test.)
5. The `Fetcher`'s sleep is injectable, so tests exercise the real backoff logic
   without spending wall-clock time.

### Stage 3 — the real run (2026-09-20)

`neo_ingest --data-dir data`, defaults (1950-01-01 .. 2150-01-01, 0.05 AU):

| Measure | Value |
|---|---|
| SBDB objects | 42,666 reported, 42,666 accepted, 0 rejected (9 pages) |
| CAD approaches | 42,819 reported, 42,819 accepted, 0 rejected (41 windows, 1 split) |
| Join | 42,819 matched, **0 unmatched**, 19,655 objects with approaches |
| Objects with no approach in the window | 23,011 |
| Diameters | 1,264 measured, 41,213 H-estimated, 189 with neither |
| `e >= 1` | 0 (the NEO group excludes hyperbolic objects) |
| Null `v_inf` | 23 rows — the reason `vInfinityKms` is optional |
| Null `H` | 210 objects |
| Duplicate window-edge rows | 0 |
| Fetch | 69 requests, 13 cache hits, 29 retries, 0 failures |
| Transfer | 9.1 MB downloaded + 12.7 MB from cache |
| Time | 172 s total (74 s network, 96 s polite waiting) |

The first attempt failed on the 1960–1965 window after 5 attempts of HTTP 502.
Re-running with `--max-attempts 8` resumed from the progress mark, replayed the
completed pages from the cache and finished — which is the resume path working
on the live API rather than only in a test.

## 11. Storage (stage 4, implemented)

`src/neo/storage/Database.{h,cpp}` is the only code that speaks SQL.

### Schema (version 1)

```
meta        one row (id = 1): schema_version, created_utc, sbdb/cad API versions,
            cad_date_min/max, cad_dist_max_au, object_count, approach_count,
            report_checksum
objects     id INTEGER PK, pdes TEXT NOT NULL UNIQUE, spkid TEXT UNIQUE,
            name, full_name, kind, numbered, is_neo, is_pha, orbit_class,
            orbit_id, epoch_jd, e, a_au, q_au, i_deg, om_deg, w_deg, ma_deg,
            n_deg_per_day, period_days, moid_au, condition_code,
            h_mag, diameter_km, diameter_sigma_km, albedo, rot_per_hours,
            est_diameter_km
approaches  id INTEGER PK, object_id INTEGER NOT NULL REFERENCES objects(id),
            jd, dist_au, dist_min_au, dist_max_au, dist_range_derived,
            v_rel_kms, v_inf_kms, h_mag, diameter_km, diameter_sigma_km
```

- `PRAGMA foreign_keys = ON` on both write and read.
- Everything optional is a **nullable column**: unknown stays NULL, never 0 or
  "". `is_neo` / `is_pha` are nullable integers, so the three states
  (yes / no / not asserted) survive. An object with no SPK-ID stores NULL rather
  than "", so `UNIQUE` tolerates any number of them.
- **Measured and estimated diameters are separate columns**
  (`diameter_km`, `est_diameter_km`) and are never merged. The estimate is
  written for anyone querying the file directly but recomputed on load, so H and
  albedo remain the single source of truth.
- `dist_range_derived` records the `dist_min`/`dist_max` fallback per row.
- **No query indexes**, asserted by a test that reads `sqlite_master` and
  requires every index to be a `sqlite_autoindex_` (a UNIQUE constraint).

### Atomic writes

1. Build `neo.db.tmp` — schema, then one `BEGIN IMMEDIATE` transaction with
   prepared statements for every row.
2. Verify **inside the temp file**: row counts against the dataset, then
   `PRAGMA foreign_key_check`.
3. Swap: move the old file to `neo.db.old`, rename the temp into place, delete
   the old one; a failed rename puts the old file back.

Any failure deletes the temp file and leaves the previous `neo.db` untouched.
The cleanup happens after the SQLite connection is closed, because Windows
refuses to delete an open file — the first version leaked a temp file on every
failure path, which the interrupted-write test caught.

The temp file uses `journal_mode = OFF` and `synchronous = OFF`: it is
disposable, so durability pragmas there would only cost time. Safety comes from
the swap, not from the journal.

### Load

`loadDatabase` fills the master record vector and the flat approach vector with
per-object ranges, exactly the in-memory model: objects in `id` order, then
approaches resolved through an id -> index table, then one sort that rebuilds
every range (`Dataset::setApproaches`, sharing the invariant with the join). A
row count that disagrees with the meta row, or an approach with no parent, fails
the load rather than producing a quietly wrong dataset.

A `schema_version` this build does not know is refused with the version numbers
and the fix: re-run `neo_ingest` (`--rebuild-db` rebuilds from the cache
without any network).

### Stage 4 — decisions recorded

1. SQLite is storage only (see the hard rules); the query path never issues SQL.
2. Object ids are 1-based and dense, assigned from the record order, so the
   round trip preserves record order without storing it.
3. `est_diameter_km` is stored but not read back: derived data has one owner.
4. `neo_ingest` writes `neo.db` **only after a completely successful run** — a
   half-downloaded dataset must never look authoritative. `--rebuild-db`
   rebuilds it offline from the cache; `--no-db` skips it; `--check-db N` loads
   it back N times and reports the median.
5. The meta row stores the FNV-1a checksum of the run's `ingest_report.json`, so
   a database can be traced back to the validation numbers that produced it.

### Stage 4 — the real dataset

`neo_ingest --rebuild-db --check-db 5` (rebuilt offline from the cache, 53 cache
hits, 0 requests):

| Measure | Value |
|---|---|
| Rows | 42,666 objects + 42,819 approaches |
| File size | **12.30 MB** (12,902,400 bytes) |
| Write time | 0.13 s |
| Load time | **0.052 s median** of 5 loads (0.048–0.056 s) |

That is ~0.6 us per row to go from the file to the in-memory model with ranges
rebuilt, which is the baseline the stage 7 ingestion/memory experiments compare
against.

## 12. DSA layer (stage 5, implemented)

Code in `src/neo/dsa/`; every header opens with its purpose, per-operation
complexity and the design choices behind it. `docs/DSA_NOTES.md` is the long
form, with worked examples for the viva.

| File | Structure | Used for |
|---|---|---|
| `HashMap.h` | open addressing, Robin Hood, backward-shift delete | `pdes` / `spkid` -> record index |
| `Sort.h` | stable merge sort over index arrays, `lowerBound` / `upperBound`, `SortedView` | ordered views by diameter, H, MOID, a, e, i; approaches by date, distance, velocity |
| `BinaryHeap.h` | array heap, O(n) heapify, `topK` | top-K closest / fastest / largest |
| `AvlTree.h` | AVL over (key, payload) in a vector node pool | ordered index and range queries |
| `BucketIndex.h/.cpp` | counting-sorted CSR buckets | size classes and year buckets |
| `Instrumentation.h` | `NullCounters` / `LiveCounters` policy | stage 7 operation counts |

### Stage 5 — decisions recorded

1. **Indices, never copies.** Every structure stores `std::uint32_t` positions
   into the master vectors. Six sorted views cost ~1 MB of indices; six views of
   copies would cost ~102 MB.
2. **Counters are a template policy inherited privately.** `NullCounters` is an
   empty base with inline empty methods, so an uninstrumented build is
   byte-identical; the tests instantiate the same structures with
   `LiveCounters`, so the counting path is covered without a second build. The
   methods are `const` with `mutable` fields so a `find()` can count its probes.
3. **Sorted views exclude unknown keys structurally**, and count the exclusions.
   A "diameter > 100 m" query cannot match an object with no diameter, because
   such an object is not in the diameter view. NaN is refused for the same
   reason: it would make the ordering meaningless.
4. **AVL over an interval tree**, justified in the header and the notes: a close
   approach is an instant, not an interval.
5. **Buckets keep an "unknown" class** rather than dropping unclassifiable
   objects, and an index records which diameter policy built it.
6. The `SortedView` keeps keys parallel to the indices, so a binary search reads
   one contiguous `double` array instead of touching 42,666 scattered records.

### Stage 5 — the structures over the real dataset

Run as part of `neo_dsa_tests` (it skips cleanly when `data/neo.db` is absent):

| Structure | Measurement |
|---|---|
| `HashMap` of 42,666 designations | capacity 65,536, load 0.65, **mean probe 0.90, worst 12** |
| Measured-diameter `SortedView` | 1,264 in the view, **41,402 excluded as unknown** |
| `AvlTree` of 42,819 approach dates | **height 18** (perfect would be 16) |
| Range query, the 2030s | 1,324 approaches, matching a linear scan exactly |
| `topK` 10 closest | matches `std::partial_sort`; closest is 2025 UC11 at 0.000044 au |
| Size buckets (measured or estimate) | 2,707 / 17,489 / 10,455 / 10,872 / 954, 189 unknown |
| Year buckets | 1950..2149, 200 buckets, every approach filed |

### Stage 5 — bug found by the differential tests

`topK` inverted its ranking predicate before handing it to the heap. Since a
max-heap already puts the greatest element under the comparator on top — and
under "ranks above" that is the element ranking *last* — inverting it a second
time put the **best** element on top, so candidates were compared against the
wrong end. The comparison against `std::partial_sort` caught it on the first
run. It is written up in `docs/DSA_NOTES.md` because it is exactly the kind of
thing a viva question is made of.

### Stage 5 addendum — the grazing / impact flag

`CloseApproach::grazingOrImpact()` (nominal `dist` < 4.2635e-5 au, Earth's
equatorial radius 6378.1 km) is derived, not stored, and is counted in the ingest
report and in `ingest_report.json` (`dataset.grazing_or_impact`). On the real
data it matches **0** approaches: the closest pass, 2025 UC11 at 0.0000441 au, is
1.034 Earth radii, about 220 km above the surface. Separately, 65 rows have a
3-sigma minimum below one radius; all are poorly determined orbits far from their
epoch, which is why the flag uses the nominal distance. Details in
`docs/DSA_NOTES.md` section 6b.

## 13. Query engine (stage 6, implemented)

`src/neo/query/`, `tools/neo_query`, `tests/neo_query_tests.cpp`. Queries run on the
in-memory structures from stage 5; **no query issues SQL**.

### The API (what the NEO FILTER panel calls)

```cpp
neo::QueryEngine engine(dataset);
engine.build();                          // once, after load; ~45 ms for 42k objects
neo::Query q;                            // plain data, no parsing
q.pha = neo::TriState::Yes;
q.dateJd = neo::Range::between(jd2030, jd2040);
q.distanceAU = neo::Range::atMost(5 * neo::kLunarDistanceAU);
q.diameterKm = neo::Range::atLeast(0.14);
q.sortBy = neo::SortField::Distance;  q.topK = 10;
neo::QueryResult r = engine.run(q);      // const; safe from any thread
// r.rows[i].object            -> index into dataset.records()
// r.approachesBegin/End(row)  -> that object's MATCHING approach indices
// r.totalObjects              -> matches before top-K ("showing 10 of 1,324")
// r.stats.explain()           -> the plan, as text
```

- `Query` holds **object filters** (designation, name prefix, kind, NEO/PHA
  tri-state, orbit classes, diameter + mode, H, MOID, a, e, i) and **approach
  filters** (date, distance, v_rel, grazing), then sort field, direction and top-K.
  Units are fixed: JD (TDB), AU, km/s, km, degrees. `kLunarDistanceAU` converts LD.
- **Bounds are inclusive.** A range with `lo > hi`, or a NaN bound, is a
  **validation error** (`validate()` returns one message per problem, and `run()`
  refuses the query) rather than an empty result.
- `engine.findDesignation()`, `findSpkId()` and `searchNames(prefix, limit)` serve the
  search box. `engine.buildReport()` lists every index with its build time and memory.
- **Thread safety:** `build()` finishes first; after that every method is `const` and
  touches only immutable indexes and locals, so any number of threads may `run()`
  concurrently. A test runs 4 threads x 900 queries and compares with sequential
  answers.

### Semantics

1. **Same-row.** All approach conditions apply to ONE approach row. An object
   matches only if a single approach satisfies every approach condition; two
   different approaches, one meeting the distance limit and the other the velocity
   limit, never combine. A hand-built case (`X`: close-but-fast and far-but-slow)
   verifies this on every access path.
2. **Result = objects plus their matching approaches.** Sorting by an approach field
   uses the object's best MATCHING approach (minimum ascending, maximum descending).
   With no approach condition, an object matches on its own properties and all its
   approaches are listed (possibly none).
3. **Unknown is never zero.** A range on H, MOID or diameter never matches an unknown
   value. Tri-state flags: unknown matches neither `Yes` nor `No`.
4. **Diameter modes:** `MeasuredOnly`, `MeasuredOrEstimated` (default), and
   `IncludeUnknown` (as the latter, and objects with no diameter at all also match).
   The mode also decides which diameter a diameter sort reads.
5. **Ordering is total and deterministic.** Unknown sort keys go last in both
   directions; ties break by object index ascending. `topK` uses the heap, and its
   result equals the prefix of the full sort.
6. **Name search** matches the name OR the designation, case-insensitively, by
   prefix. Exact designation is case-sensitive and goes through the hash map.

### Three execution paths, one answer

| Path | What it does |
|---|---|
| **Naive** | A full linear scan reading the records directly. The oracle: no shared index, no shared column, no shared sorting code (it sorts with `std::stable_sort`, so it also checks my merge sort and heap on every query). |
| **Fixed-order** | Uses the indexes but always drives from the first present predicate in a fixed priority (designation, name, date, distance, v_rel, diameter, H, MOID, a, e, i); residuals in declaration order. The benchmark baseline for the planner. |
| **Planned** | Statistics-driven, described below. |

`run(query, mode, force)` also accepts a forced access path, so every structure is
checked against the naive scan even when the planner would never choose it.

### Query planner

**Statistics** (built with the indexes, free of extra sorting because they are read
off arrays that are already sorted):

- an **equi-depth histogram, 64 bins**, for each of 10 numeric fields (diameter x2, H,
  MOID, a, e, i, date, distance, v_rel). Unknown values are excluded from the bins and
  the known fraction scales every estimate, so unknowns never inflate one.
- **exact counts** for the categorical predicates (kind, NEO, PHA, orbit class, grazing).
- **exact** candidate counts where the structure can give them in O(log n) or O(1): the
  name index (two binary searches), the size and year buckets, the hash map.

**Access paths considered** (each an alternative way to produce candidates):

| Access | Serves | Exact? | Per-candidate cost |
|---|---|---|---|
| hash lookup | exact designation | yes | 1.0 |
| name index | name/designation prefix | yes | 1.5 |
| sorted view (object) | diameter, H, MOID, a, e, i | yes | 1.0 |
| size buckets | diameter (the only path for `IncludeUnknown`) | superset | 0.8 |
| AVL tree | date | yes | 2.5 |
| year buckets | date | superset | 0.8 |
| sorted view (approach) | distance, v_rel, grazing | yes | 1.0 |
| scan objects / approaches | anything | yes | 0.5 |

A **superset** path (buckets) over-reads and then re-checks its own predicate as the
first residual, so it stays exactly correct.

**Cost model.** Every predicate has a unit cost (a range test on a dense column 1.0, a
class or diameter test 1.5, a name-prefix test 4.0). A residual chain applies its
predicates in order of **rejection per unit cost**, `(1 - selectivity) / cost`, highest
first; its expected cost per input row is `sum_j cost_j * prod_{k<j} sel_k`, and its
pass fraction is the product of the selectivities. For a driver producing `m` candidates:

```
object-driven:    work = m*access + m*objChain.cost
                       + m*objChain.pass * avgApproaches * appChain.cost   (if approach conditions)
approach-driven:  work = m*access + m*appChain.cost
                       + m*appChain.pass * (0.5 + objChain.cost)           (parent-object fetch)
```

The planner enumerates every applicable driver (including the two scans), estimates
its work, and takes the cheapest (ties: fewer candidates, then access-path order). It
then orders the residuals for that choice. `docs/DSA_NOTES.md` section 8 works one
example through by hand.

**Assumption to state in the report:** selectivities are combined by multiplication,
which assumes the predicates are independent. Real fields correlate (small H implies a
large diameter), so combined estimates can be off; the *driver* choice depends mostly on
the single most selective predicate, which is estimated directly, so it is robust to
that.

### EXPLAIN

Every result carries its plan: the driver, estimated vs actual candidates (with the
estimate error), each residual predicate in execution order with its estimated and
actual selectivity and how many times it ran, the alternatives the planner weighed
with their estimated work (chosen one marked), totals, and time. Real examples on
the 42,666-object / 42,819-approach dataset:

```
$ neo_query --pha --from 2030-01-01 --to 2040-01-01 --max-dist-ld 5 --min-diam 0.14 --top 10 --sort dist --explain

EXPLAIN (planned)
  query       pha=yes | diameter [0.14, -] km (measured or H-estimate) | date 2030-01-01..2040-01-01 | dist [-, 0.0128478] au | sort dist asc | top 10
  driver      year buckets overlapping [2030-01-01, 2040-01-01]
              year buckets access, superset (the predicate is re-checked); estimated 1446 candidates, actual 1,446
  steps       1. date [2030-01-01, 2040-01-01]   [approach row]
                   est selectivity 0.0311, unit cost 1.0 | evaluated 1,446, passed 1,324 (actual 0.9156)
              2. dist [-, 0.0128478] au   [approach row]
                   est selectivity 0.2163, unit cost 1.0 | evaluated 1,324, passed 123 (actual 0.0929)
              3. pha = yes   [object]
                   est selectivity 0.0597, unit cost 1.0 | evaluated 123, passed 10 (actual 0.0813)
              4. diameter [0.14, -] km (measured or H-estimate)   [object]
                   est selectivity 0.2760, unit cost 1.5 | evaluated 10, passed 10 (actual 1.0000)
  considered  * est work       2663, est      1446 candidates  year buckets overlapping [2030-01-01, 2040-01-01]
                est work       5122, est      1333 candidates  AVL tree: date [2030-01-01, 2040-01-01]
                est work      18981, est      9261 candidates  sorted view: approach distance [-, 0.0128478] au
                est work      22548, est     11826 candidates  size buckets overlapping diameter [0.14, -]
                est work      24275, est     11774 candidates  sorted view: diameter [0.14, -] (measured or estimate)
                est work      66019, est     42819 candidates  full scan of all approaches
                est work      68550, est     42666 candidates  full scan of all objects
  work        candidates examined 1,446, predicate evaluations 2,903
  result      10 objects (10 approaches) matched; 10 returned
  time        plan 0.016 ms + execute 0.030 ms = 0.053 ms
```

Where the planner and the fixed order disagree: a **rare distance** with a date window
covering everything (`--max-dist-au 0.0005 --from 1950-01-01 --to 2149-12-31`):

| Path | Driver | Time |
|---|---|---|
| naive | full scan of all objects | 0.896 ms |
| fixed-order | AVL tree on the date (42,819 candidates) | 0.580 ms |
| **planned** | **sorted view on distance (231 candidates)** | **0.031 ms** |

The planner's estimate there was 287 candidates against 231 actual (-19.4%): the
histogram is coarsest where the data is densest near zero, and the estimate error is
printed rather than hidden. Where scanning is genuinely right (`--pha --class ATE`: two
flag tests, no index), the planner says so: `full scan of all objects`, 42,666
candidates, estimate exact.

### Index build report (real dataset, from `neo_query --info`)

| Index | Entries | MB | Build ms |
|---|---:|---:|---:|
| columns (fields as dense arrays) | 85,485 | 3.462 | 2.4 |
| hash map: designation -> record | 42,666 | 2.500 | 2.1 |
| hash map: SPK-ID -> record | 42,666 | 2.500 | 1.9 |
| name index (sorted, prefix search) | 43,055 | 1.642 | 3.9 |
| sorted view: diameter (measured) | 1,264 | 0.014 | 0.1 |
| sorted view: diameter (measured or estimate) | 42,477 | 0.486 | 2.7 |
| sorted view: H | 42,456 | 0.486 | 2.9 |
| sorted view: MOID | 42,535 | 0.487 | 3.0 |
| sorted view: a / e / i (each) | 42,666 | 0.488 | 2.8-2.9 |
| sorted view: approach distance / v_rel (each) | 42,819 | 0.490 | 3.0-3.3 |
| AVL tree: approach date | 42,819 | 0.980 | 5.2 |
| size buckets (measured / measured or estimate) | 42,666 | 0.163 each | 0.4 / 2.3 |
| year buckets (approaches) | 42,819 | 0.165 | 1.4 |
| equi-depth histograms (10 fields x 64 bins) | 640 | 0.005 | 0.4 |
| **total** | | **15.5** | **~43** |

The two hash maps are the largest single items (2.5 MB each for 42,666 entries,
~58 B per entry) because each slot stores its `std::string` key inline. That is the
first thing the stage 7 memory experiment should look at: keying the slots by record
index and comparing against the record would cut both maps to a few hundred KB.

### Stage 6 — decisions recorded (for veto)

1. **Default diameter mode is `MeasuredOrEstimated`.** Only 1,264 of 42,666 objects
   have a measured diameter, so a measured-only default makes "diameter >= 140 m"
   nearly empty. Estimates are derived from H with p = 0.14 (a factor of ~2 either
   way), so the CLI marks them `~` and `MeasuredOnly` is one flag away.
2. **Approach distance is the nominal `dist`**, not the 3-sigma minimum, consistent
   with the grazing flag (65 rows have a 3-sigma minimum inside the Earth; all are
   poorly determined orbits).
3. **A name search matches the name or the designation.**
4. **Approach-field sort** uses the best matching approach, so "closest approaches in
   2030-2040" ranks objects by their closest approach *within that window*.
5. **Unknown sort keys are last in both directions**; ties by object index.
6. `IncludeUnknown` is served only by the size buckets (an index range cannot include
   the unknowns), and the planner knows it.

### Stage 6 — tests

`neo_query_tests`: **298 checks, 15 s**, including:

- validation (`lo > hi`, NaN, several errors at once, querying before `build()`)
- semantics on a hand-built dataset with known answers, each run on **every** path:
  the three diameter policies, boundary-equal values, tri-state flags, classes,
  designation and prefix, grazing, date windows, combined filters, sort with ties and
  unknown keys, top-K
- the same-row cases (dist AND v_rel, date AND v_rel, three conditions, a fourth added)
- histogram estimates (uniform, unknown fraction, a 60% point mass, empty, single value)
- planner driver choices, EXPLAIN content, and estimate quality: over 1,200 random
  ranges the histogram's **mean error is 0.56% of the rows, worst 2.86%**
- **the oracle: 12,200 random queries** (1,500 on the fixtures, 10,000 on synthetic
  data full of ties, unknowns and boundary values, 700 on the real `neo.db`), each run
  as naive, fixed-order, planned and **all nine access paths forced** -- about 146,000
  engine runs -- with **0 mismatches**. Coverage is asserted: every access path drove
  at least one query, results are a healthy mix of empty and non-empty, and top-K
  truncation is exercised
- 4 threads x 900 queries against the sequential answers

**The oracle can fail.** A mutation check broke the engine four ways -- size buckets
skipping their top bucket, objects with no matching approach being kept, year buckets
reading one year short, and one range boundary made exclusive -- and the suite failed
each time (3 to 16 failing checks), then passed again once the engine was restored.

## 14. Earth view (implemented)

Replaces the "UI hook" of section 9. Entered with `E` or the EARTH VIEW button while the
Earth is the target; `ESC` / SOLAR VIEW leaves. The solar view is untouched.

### Layering

```
QueryEngine::run  ->  NeoService (worker thread, latest-wins)  ->  NeoOutcome{QueryResult, FlybyScene}
                                                                        |
                       HUD panels + renderer read it, never write it  <-'
```

The UI never parses JSON, never uses SQL and never touches the network: `NeoService`
loads `neo.db` on its worker (`data/neo.db` found by walking up from the executable, or
`SOLSIM_NEO_DB`) and only ever calls `QueryEngine::run`. The mapping from a result to a
scene is `neo/sim/EarthFlybys` -- pure C++, no GL, no ImGui -- and is what the tests pin down.

### Real versus schematic (stated on screen too)

| REAL, taken from the data | SCHEMATIC, chosen for legibility |
|---|---|
| time of closest approach = the CAD Julian Date, exactly, on the sim clock | approach **direction** and path orientation: a hash of the designation and approach ordinal (CAD gives no usable geometry) |
| closest distance (monotonically mapped, see below) | the **log radial scale** |
| along-track speed proportional to the real `v_rel` | straight-line path instead of the hyperbola |
| marker size from the diameter; hollow when it is only estimated from H | one declared **time scale**: 0.20 render units per day per km/s of `v_rel`; path half-length 14 units |
| PHA = accent colour | |

**Radial scale** (R = Earth's equatorial radius, 6378.137 km = 1 render unit):
`r = d/R` inside the planet, `r = 1 + 3.2 * log10(d/R)` outside. Strictly increasing,
continuous at the surface and invertible, so ordering by distance is ordering by radius.
Reference rings: GEO 3.6, 1 LD 6.7, 5 LD 8.9, 0.05 AU 10.8 units. The scale is printed in
the viewport's top-right corner and in the EARTH VIEW panel.

**Position at time t** = `closest + direction * speed * (t - t_ca)`, visible while
`|along-track| <= 14`. All of it in doubles on the CPU, shared by drawing, picking and the overlay.

### Scene

- The Earth is fixed at the centre and only rotates: 23.44 deg tilt, one turn per 20 s by
  default, independent of the sim clock (speed slider + pause). NASA Blue Marble texture
  (2048x1024) loaded with stb_image; a procedural lat/long grid if the file is missing.
  Lighting uses a fixed Sun direction and does not spin with the texture; soft atmosphere rim.
- Paths are built **once per result** (`setScene`), ten segments each, and drawn as native
  one-pixel lines; only the selected and hovered paths are widened by the geometry shader.
  Markers stream through one dynamic VBO, one draw call. Faintness scales with the flyby
  count so a thousand additive lines read as a field.
- Entering the view sets the clock to +1 d/s and jumps it to the selected approach; leaving
  restores the date, rate, pause state and tracking that were saved, and resets the solar
  event detector so the log does not report every event "between" the two dates.
- Transition: ~0.8 s. The solar camera flies to the Earth while the view fades to the
  background colour; the world is swapped at the dark midpoint; the Earth camera flies in.
  (Neither camera is modified: the solar one is copied.)

### HUD

NEO FILTER (RUN + result count + one-line EXPLAIN on top, then date window, max distance in
LD or AU, min/max diameter with diameter mode, v_rel range, PHA only, grazing only, sort,
top-K), EARTH VIEW (rotation, prev/next result, next approach, rings, scale legend),
NEO RESULTS (clickable table: select + jump the clock to the approach), and TARGET ·
OBSERVATION for the selected asteroid (designation, D measured/estimated, H, PHA, date,
nominal/min/max distance in LD and km, v_rel, v_inf). The panels dock into the nodes of
the panels they replace; an `imgui.ini` from before the Earth view still docks them
(`FindWindowSettingsByID` on the sibling panel).

Each result is an object *with its matching approaches*, so 50 objects can be 116 flybys.
The flyby capped at 1,000 (`kMaxFlybys`); the panel says how many were not drawn.

### Performance (Intel UHD, 1920x1094 window, 4x MSAA HDR + bloom)

| Case | uncapped fps | frame time |
|---|---|---|
| solar view | 406 | 2.5 ms |
| Earth view, empty result | 441 | 2.3 ms |
| Earth view, 116 flybys | ~350 | 2.8 ms |
| Earth view, 1,000 flybys, widened paths (first version) | 130 | 7.7 ms |
| Earth view, 1,000 flybys, native 1 px paths | 179 | 5.6 ms |

The cost of the first version was fill rate, not vertices: cutting 48 segments to 10 changed
nothing, and removing the paths (not the markers) took the scene call from 4.3 ms to 0.45 ms.
With vsync the view holds 60 fps at 1,000 flybys.

### Dev hooks (environment variables)

`SOLSIM_EARTH=1` (start in the view; screenshots wait for the first result),
`SOLSIM_NEO_TOPK`, `SOLSIM_NEO_MAXLD`, `SOLSIM_NEO_PHA=1`, `SOLSIM_NEO_FROM/TO`,
`SOLSIM_NEO_SELECT=i`, `SOLSIM_NEO_HOVER=i|any`, `SOLSIM_EARTH_ENTER=n` /
`SOLSIM_EARTH_LEAVE=n` (frame numbers), `SOLSIM_NEO_DB`, `SOLSIM_VSYNC=0`.

### Not done / known gaps

- Real cursor hover and click picking, the results-row click, prev/next and the spin
  controls were driven only through the same code paths by hooks, not by hand.
- `layoutFlybys` / `pickFlyby` live in the GL renderer file and have no unit test.
- "Show the selected asteroid's orbit in the solar view" was skipped as too costly for
  the first version (see section 15).

## 15. NEOS layer in the solar view (implemented)

A **NEOS** toggle in MISSION CONTROL, independent of the Earth view. It draws the
near-Earth objects as points in the solar view, each from its own orbital elements,
with the same compressed / true scale mapping as the planets. The control is a size
selector, not an on/off for all 42,666.

### Which objects: presets, not "the first N"

| Preset | Objects | Order |
|---|---|---|
| PHAS ONLY | every potentially hazardous asteroid (2,549) | RANK |
| 1,000 / 5,000 / 20,000 | the first N of RANK | RANK |
| ALL | every object with `e < 1` (42,666; none are hyperbolic today) | RANK |
| FILTER RESULT | whatever the Earth view's last NEO FILTER query returned | query order |

**RANK** = diameter, largest first (measured, else the H estimate; unknown last; ties by
record index). It is deterministic and makes the numbered presets **nested** (1,000 inside
5,000 inside 20,000 inside ALL), so raising the count only adds points and nothing on screen
moves or disappears. It is the order that matters to a viewer too: the biggest bodies are
the ones worth seeing first. The cost, stated in the menu's tooltip, is that a small preset
over-represents large objects (the 1,000th object is ~1 km). A uniform sample would look
more like the population but would show a random 1,000 of mostly ~100 m rocks. The
toggle carries the live count: `NEOS: 5,000 / 42,666`.

### Propagation

Each object is propagated by the existing solver (`sim::solveKepler`) from **its own**
elements. SBDB gives elements at each object's own epoch, `sim::propagate` wants them at
J2000, so `neo::toSimElements` back-propagates the mean anomaly with the mean motion the
solver itself uses (section 3): the position *at the object's epoch* is then exactly the
published one. `propagateSwarm` is `sim::propagate` with the orbital-plane rotation folded
into two precomputed basis vectors. **Test: over 2,400 (object, time) pairs its position
differs from `sim::propagate` by 0 AU (bound 1e-12) and its velocity by 4e-16 relative.**
Objects with `e >= 1` are skipped everywhere (the catalogue drops them).

Two ways to serve a frame (`neo::SwarmField`, pure C++, no GL):

- **DIRECT** (up to 2,000 objects): every object, every frame, on the main thread.
- **NODES** (above 2,000): a worker thread computes positions **and velocities** on a time
  grid (1 day by default); each frame interpolates between the two surrounding nodes with a
  cubic **Hermite** spline (a chord would bend visibly on a 100-day orbit). The frame never
  waits: if a node is not ready `positionsAt` returns false and the previous positions stay.
  The grid step follows the clock rate (at most 30 nodes/s: 200 d/s asks for 8 days, the cap
  is 32) and the node in the direction of travel is prefetched; a jump of centuries is served
  once the worker has caught up. Replacing the object set while work is queued is safe (a
  generation counter discards stale nodes; tested repeatedly).

Interpolation accuracy (tests, 300 random orbits, e up to 0.92): at a 1-day step the worst
error is 2.8e-6 AU (~400 km); the test asserts under 5e-5 AU. Coarser grids trade accuracy
for speed (0.54 AU worst object at 32 days, information only): at 365 d/s an orbit is a
blur anyway.

### Rendering

One dynamic VBO of positions (3 floats per object, heliocentric ecliptic AU), one of
per-object attributes, **one draw call**, additive blending, depth-tested against the Sun and
planets. The scale mapping is applied **in the vertex shader** (`swarm.vert`), so the CPU
does no per-point work: compressed `dir * k * log10(1 + d*c)` or true `p * unitsPerAU`, the same
formula as `ScaleMapper::toRender` (a screenshot check: the CPU-projected selection brackets
sit on the GPU-drawn point in both scales). Known limit: positions are floats relative to the
camera target, fine everywhere except zoomed to sub-pixel scale on a far point in true scale.

### Legend (colour and brightness)

`neo/sim/SwarmLegend.h` defines one legend for the shader, the HUD legend bar and the tests:
DISTANCE FROM EARTH (0.01 to 5 AU, log), PHA, DIAMETER (10 m to 10 km, log), TIME TO
APPROACH (now to one year; refreshed about once per simulated day). *The Earth view has no
SWARM mode yet, so there was nothing to share it with; the legend is written so it can adopt
it.*

### Selection, shared by both views

Clicking a point selects that object (the nearest within 10 px, `pickSwarm`, pure and
tested). The selection is one `Dataset` record index used by both views:

- solar -> Earth: the Earth view selects that asteroid's flyby, if its current result has one
  (the one nearest in time); if not, TARGET still shows the asteroid;
- Earth -> solar: selecting a flyby (marker, row, prev/next) highlights the asteroid in the
  solar view even when it is not in the drawn preset.

The solar view shows brackets, a tag and the selected object's **orbit** (96 points of the
ellipse, mapped like the planets' rings; this also delivers the "orbit in the solar view"
item that was skipped in stage 8). TARGET · OBSERVATION shows the object: distance from the
Sun and from the Earth now, orbit (a, e, i, q, Q, period), MOID, diameter and H, class, and
its next approach in the data. Choosing a planet (click or the target combo) clears it.

### Frame rate per preset (Intel UHD, 1920x1094 window, 4x MSAA HDR + bloom, solar view)

Median of three runs each, 600 frames. **Vsync on: 60.0 fps for every preset.** Uncapped:

| Preset | mode | uncapped fps | frame | propagation cost |
|---|---|---|---|---|
| NEOS off | | 347 to 388 (155 on the first launch after a pause) | 2.6 to 2.9 ms | |
| 1,000 | direct | 304 | 3.3 ms | 0.16 to 0.31 ms per frame, main thread |
| PHAs, 2,549 | worker | 348 | 2.9 ms | 0.4 to 0.8 ms per node, worker |
| 5,000 | worker | 325 | 3.1 ms | 0.7 to 2.5 ms per node |
| 20,000 | worker | 294 | 3.4 ms | 3.2 to 6.3 ms per node |
| ALL, 42,666 | worker | 279 | 3.6 ms | 5.6 ms per node idle, 7 to 13 loaded |

For comparison, the **same presets forced onto the main thread** (`SOLSIM_NEOS_DIRECT`):
PHAs 359, 5,000 277, 20,000 175, ALL 114 fps (6.8 ms of propagation per frame). That is
why the worker exists: it costs ALL about one extra millisecond per frame instead of seven.
The 2,000 threshold is where direct mode costs about 0.5 to 1 ms.

The measurement is noisy: the first launch after a pause runs at less than half speed
(GPU clocks), so each figure is a median and the "off" row shows both.

**The practical ceiling on this machine is not reached.** ALL is affordable at 60 fps with
headroom (about 3x). **Default preset: PHAs only** (2,549): it is the smallest set with a
clear meaning, well under the ceiling, and on the worker. The layer itself is **off** until
switched on, so the solar view is unchanged by default.

### Tests

`neo_swarm_tests` (74 checks): kernel vs `sim::propagate`, the epoch convention, selection
(RANK, PHA, nesting, ties, clamps, the filtered result), the field in both modes (float
precision, at and between nodes, forward, backward, a two-century jump, grid step by rate,
pause, replacing objects mid-flight), the legend, and the real dataset (2,549 PHAs, 42,666
propagatable, one direct frame and one worker node timed, a real object equal to
`sim::propagate`). `render_tests` gained the picker (8 checks).

### Dev hooks

`SOLSIM_NEOS=pha|1000|5000|20000|all|result`, `SOLSIM_NEOS_LEGEND=distance|pha|diameter|approach`,
`SOLSIM_NEOS_SELECT=i`, `SOLSIM_NEOS_DIRECT=n` (0 = always on the worker).

### Not done

- Real mouse clicks on a NEOS point were not driven by hand (the picker is unit-tested and
  the selection path was exercised by a hook).
- Double-click to track an asteroid with the camera.
- The Earth view has no legend / SWARM mode.
