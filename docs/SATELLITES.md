# Satellites and space stations

**Visualization only.** This is not part of the graded NEO data, DSA and query work. It lives in its own
source tree (`src/sat`), its own tools, its own tests and this document; `docs/NEO_PLAN.md` does not
describe it and `src/neo` never includes anything from it. Status: the data and propagation layer is built
and tested; **nothing is drawn yet** (see "Not done").

## Scope

ISS, Tiangong, Hubble, plus CelesTrak's `stations` and `visual` groups: a few hundred objects. **No
Starlink yet**: propagation cost was measured on this set first (below) and the decision about
mega-constellations is left open.

## Layout

```
external/sgp4/           Vallado's reference SGP4 (C++), vendored unmodified; MIT; see THIRD_PARTY.md
src/sat/model/           ElementSet (the TLE/OMM fields, in published units), Epoch (calendar and Julian Date)
src/sat/parse/           TleParser (TLE, 3LE and 2LE text), OmmJsonParser (CelesTrak GP JSON), detail/Number
src/sat/sim/             Propagator (SGP4: mean elements in, TEME position and velocity out)
src/sat/store/           Catalog (data/sat/elements.json, atomic write, exact round trip)
src/sat/ingest/          CelesTrakIngestor (download, merge, pinned-object check)
tools/sat_ingest/        download / import / list
tools/sat_bench/         propagation cost
tests/sat_tests.cpp      158 checks, including Vallado's reference vectors
tests/sat_ingest_tests.cpp  51 checks, fake HTTP client, no network
tests/fixtures/sgp4/     SGP4-VER.TLE, tcppver.out (Vallado), sample_omm.json; PROVENANCE.json
tests/fixtures/sat/      real TLEs and an OMM listing from python-sgp4; PROVENANCE.json
```

CMake targets: `solsim_sat` (parsers, propagator, catalogue; depends on `sgp4` and nlohmann/json, **not** on
`src/neo`), `solsim_sat_ingest` (the downloader), `sat_ingest`, `sat_bench`, `sat_tests`,
`sat_ingest_tests`. `solsim_sat_ingest` is the one place that touches `src/neo`, and only to borrow its HTTP
interface, retry/backoff and raw-response cache (`neo::IHttpClient`, `neo::Fetcher`, `neo::ResponseCache`)
so a download is polite and replayable the same way. The dependency runs one way. The future UI links
`solsim_sat` only.

## Decisions

- **SGP4, not the Kepler solver the asteroids use.** TLE and OMM elements are *mean* elements fitted for
  SGP4; two-body Kepler motion drifts from them by kilometres within hours in low Earth orbit.
- **Vallado's reference implementation, vendored** (python-sgp4 2.27, commit
  `6e7428df7acc48e8176828289a8c14486a114759`, files byte-identical to the PyPI sdist whose SHA-256
  `06d37247...85f9` matches PyPI's). **Licence: MIT (Brandon Rhodes), not public domain.** The C++ files are
  David Vallado's and carry no licence text of their own; they are redistributed under the package's MIT
  licence. Vallado's own terms were not re-read. Built silenced and never edited, so the claim "this is the
  reference code" stays true. `.gitattributes` keeps those files and the vectors out of line-ending
  normalisation so the recorded hashes hold on every checkout.
- **Our own parsers, around the vendored propagator.** Vallado's `twoline2rv` reads with `sscanf`, which is
  locale-dependent (a German locale breaks `0.5`), and it needs workarounds for leading spaces and for line
  checksums. The parsers here are locale-independent, validate every column and range, and report each
  rejected set with a reason. Only `sgp4init` and `sgp4` from the reference are called.
- **Two input formats, one `ElementSet`.** `sat_ingest` downloads **OMM JSON** (CelesTrak's GP format): it
  has no 5-digit catalogue-number limit and no checksum fragility. TLE text is parsed too (the Vallado
  vectors are TLEs, and `--from-tle FILE` imports a local file with no network). The two parsers are held
  to each other: the README's ISS gives the same element set from its TLE and from its OMM listing, and
  both propagate to the same position within a millimetre.
- **Epochs are `(whole, fraction)` Julian Dates**, so a date a century away loses no precision.
- **Operation mode and gravity** are the ones python-sgp4 and the reference outputs use: WGS-72, "improved".
- **A `Propagator` is not thread-safe**: the deep-space branch keeps integrator state in the object. A test
  shows results do not depend on call order (forward and reverse give the same positions to 1e-9 km for
  every deep-space satellite in the set); only the work does. Give each thread its own.
- **A satellite SGP4 refuses at its epoch fails in `init`** (SGP4 runs one step while initialising), with
  SGP4's error code and a reason; a satellite that decays later fails in `propagate*` with NaN output, never
  with plausible-looking numbers.

## Data: CelesTrak

`https://celestrak.org/NORAD/elements/gp.php?GROUP=<group>&FORMAT=json` for each group, and
`...?CATNR=<n>&FORMAT=json` for a pinned object the groups did not contain. **Nothing has been downloaded
from CelesTrak: it is not reachable from the build sandbox, so the URLs, the response format and the group
names are from the documentation as known and from python-sgp4's samples, and are not yet exercised
against the live service.** The first live `sat_ingest` run is the first real test. `tests/fixtures/` holds
only data with traceable provenance (`PROVENANCE.json` in each folder); no element set was typed from
memory.

- **Pinned objects**: 25544 (ISS), 20580 (Hubble), 48274 (Tiangong core module), fetched by number when the
  groups lack them, **and the name found is checked** (`ISS`; `HST`/`HUBBLE`; `CSS`/`TIANHE`/`TIANGONG`). The
  numbers are from memory, not checked against the live catalogue: a wrong one is reported as "NOT FOUND" or
  "FOUND BUT THE NAME DOES NOT MATCH", never silently drawn as the wrong satellite. A missing pinned object
  does not fail the run; a failing *group* does.
- **Politeness**: one request at a time, at least a second apart, five attempts with exponential backoff
  (the `neo::Fetcher` defaults), and **CelesTrak's two-hour rule**: its sets are regenerated about every two
  hours and it blocks clients that re-request unchanged data, so in `--refresh` mode a cached copy younger
  than `--min-age` (default 120 minutes) is kept and the report says so. The rule is from CelesTrak's
  published guidance as I know it; its terms were not re-read here.
- **Caching and offline use**: raw responses in `data/sat/cache/`; `--offline` rebuilds the catalogue from
  them. The output, `data/sat/elements.json`, is written only after a successful run, atomically. `data/`
  is not in git.
- **Merging**: one set per catalogue number; the newest epoch wins; sorted by number.
- **Accuracy ages**: SGP4 degrades as an element set gets older (days, not years). `sat_ingest --list` shows
  each set's age; the app should too.

## Verification

`tests/sat_tests.cpp` runs **Vallado's verification set** (Vallado, Crawford, Hujsak and Kelso, *Revisiting
Spacetrack Report #3*, AIAA 2006-6753; `SGP4-VER.TLE` and `tcppver.out`, produced by the official C++ code)
through this project's own parser and `Propagator`: 33 satellites, near-Earth and deep-space, 667 reference
rows. Every position, velocity and time must match to **2e-7** (km, km/s, minutes), the tolerance
python-sgp4 uses on the same file. Result: **666 rows compared and one stale line (satellite 33334, which
SGP4 refuses at its epoch and the reference prints as a repeat), 0 mismatches, and all 7 SGP4 errors
reproduced in order (1, 1, 6, 6, 4, 3, 6)**, the same list python-sgp4 expects. Three of the 33 satellites
carry TLE checksums that do not verify, as published; the parser refuses them by default (30 accepted) and
the vector test opts out of the checksum.

The comparison is sharp. The propagator was deliberately broken three ways and the test failed each time,
then the code was restored byte-identical: WGS-84 instead of WGS-72 (3,631 mismatches), B* 0.1 % too large
(1,380; the first discrepancy is 6e-7 km), and an epoch 86 ms off (1,410).

Two defects were found by running the tools rather than the unit tests, and fixed with tests: the age
column of `sat_ingest --list` used a minute-precision timestamp that the epoch parser (correctly) rejects, so
"now" silently became Julian Date 0; and a bad `--groups` value was only refused after requests had begun.
A third was found by the vector test itself: old element sets can leave the ephemeris-type column blank,
which means 0.

## Performance (sandbox, single thread, `-O3`)

`sat_bench --vallado tests/fixtures/sgp4/SGP4-VER.TLE`: 28 usable real element sets (6 near-Earth, 22
deep-space; the Vallado set is weighted toward the hard cases), copied up to each count so each object has its
own `Propagator`. Median of 7 repeats; positions only. A shared 4-vCPU Linux container: indicative, not the
figures for the target machine.

| objects | near-Earth ns/propagation | ms per frame | % of a 60 fps frame | deep-space ns | ms per frame | % |
|---:|---:|---:|---:|---:|---:|---:|
| 300 | 333 | 0.10 | 0.6 % | 585 | 0.18 | 1.1 % |
| 1,000 | 354 | 0.35 | 2.1 % | 585 | 0.59 | 3.5 % |
| 5,000 | 346 | 1.73 | 10.4 % | 615 | 3.08 | 18.5 % |
| 10,000 | 359 | 3.59 | 21.5 % | 617 | 6.17 | 37.0 % |

`sgp4init` costs about 1.2 us per satellite. On the three real near-Earth sets of the 3LE sample the cost
is the same (about 355 ns). **For the scope above (a few hundred objects) propagation is under 1 % of a
frame and is not a constraint.** At Starlink scale (several thousand near-Earth objects) it is 2 to 4 ms per
frame here, affordable on a worker thread with interpolation the way the NEOS layer does it, but not free on a
slower machine, and the drawing, picking and label cost has not been measured at all. That is the input to
the Starlink decision; the decision is not made.

## Not done

- **Nothing is drawn.** The UI work is next: TEME to Earth-fixed or the Earth view's frame (a sidereal-time
  rotation), a layer toggle, markers and labels for the ISS, Tiangong and Hubble, picking, orbit trails,
  a legend by group, element-set age on screen. None of it exists.
- **No contact with CelesTrak.** See "Data". Group names, the JSON layout, the pinned catalogue numbers and
  the refresh interval are unverified against the live service.
- **Not built on Windows or with MSVC.** The code, the CMake wiring and the Windows-only `sat_ingest` target
  were written to the project's rules and compile cleanly under GCC 13 (`-Wall -Wextra -Wpedantic`) on Linux
  with a stand-in for the WinHTTP client; they have not been through the real Windows build here.
- **The real few hundred objects have not been propagated or measured**; the performance table uses the
  Vallado set and the three sample sets.
- Out of scope: manoeuvres, perturbations beyond SGP4, anything other than Earth-orbiting objects.
