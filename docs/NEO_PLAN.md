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
