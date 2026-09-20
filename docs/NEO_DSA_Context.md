**NEO-DX / ASTRODSA**

Project Context, Data Model, NASA Integration, DSA Architecture, Storage, Querying and Simulation

*Reference document for the semester DSA project*

**UI status:** The custom C++ sci-fi/astronomy UI has already been implemented. This document deliberately focuses on everything around the UI: the astronomical data, data model, ingestion, storage, DSA, query engine, simulation data pipeline, performance evaluation, project scope, and implementation order.

**Document map**

- 1\. Project concept and purpose

- 2\. NASA/JPL data sources and what each one means

- 3\. What the API actually returns (JSON)

- 4\. Core C++ data model

- 5\. Data relationships and identifiers

- 6\. Storage and caching strategy

- 7\. DSA layer and why each structure exists

- 8\. Query engine and multi-condition filtering

- 9\. Simulation/trajectory data pipeline

- 10\. Missing/uncertain data handling

- 11\. Performance and benchmarking

- 12\. Recommended project architecture

- 13\. Scope: required vs optional features

- 14\. Semester implementation roadmap

- 15\. Final project definition and checklist

- 16\. Official references

# 1. Project concept and purpose

The project is a NASA/JPL-data-driven Near-Earth Object (NEO) information and analysis system implemented around a C++ DSA core. The application takes real small-body and close-approach data, converts it into an internal C++ representation, organizes the records using several data structures, and answers queries such as object lookup, filtering, sorting, ranking, and time-range searches. The existing UI is only the presentation layer.

NASA / JPL CNEOS

\|

v

JSON data

\|

v

C++ API client

\|

v

Parser + validation

\|

v

C++ domain objects

\|

v

Persistent storage

\|

v

DSA indexes / structures

\|

v

Query engine

\|

v

Simulation-ready results

\|

v

Existing C++ UI

**Central principle:** NASA provides the raw/authoritative input; the academic contribution is the way the C++ system organizes, searches, filters, ranks and analyzes that input.

# 2. NASA/JPL data sources and what each one means

Do not treat all NEO-related data as one dataset. JPL exposes multiple related services, each answering a different question. The first version should primarily use SBDB and Close Approach Data. Sentry can be added as a risk-oriented extension. Mission Design is best treated as a separate optional module.

| **Source**                | **Main question answered**                                                                     | **Useful project fields**                                                                                                                                                 | **Version 1?**     |
|---------------------------|------------------------------------------------------------------------------------------------|---------------------------------------------------------------------------------------------------------------------------------------------------------------------------|--------------------|
| SBDB Query API            | What is this object, and what are its known physical/orbital properties?                       | SPK-ID, designation, name, kind, NEO/PHA flags, orbit class, H, diameter, eccentricity, semi-major axis, perihelion, inclination, node, perihelion argument, period, MOID | Yes                |
| Close Approach Data (CAD) | When does this object come close to Earth, and how close/fast is the encounter?                | Designation, orbit ID, close-approach date/time, nominal distance, minimum/maximum distance, relative velocity, V-infinity, H, optional diameter                          | Yes                |
| Sentry                    | Which objects have modeled future impact-risk records and what are the associated risk values? | Object/designation, impact-related records, probabilities, Palermo-related values and dates depending on query mode                                                       | Optional extension |
| Mission Design API        | What mission-design opportunities/trajectories can be computed for a target body?              | Departure/arrival dates, C3, V-infinity, delta-V, time of flight, object classification fields                                                                            | Optional extension |

**Important classification distinction:** NEO, PHA, close approach and Sentry impact-risk record are not interchangeable labels. An object can be a NEO and still have no Sentry impact-risk record. A close approach is an encounter event, not an impact claim.

# 3. What the API actually returns (JSON)

JPL APIs return machine-readable JSON. Your C++ program does not receive a C++ struct from NASA; it receives a JSON document that must be parsed and mapped into your own types. Both SBDB Query and CAD document JSON output and advise clients to respect the documented response format/version.

Example conceptual JSON -\> C++ mapping

{

"name": "Example Object",

"diameter": 0.34,

"neo": true

}

becomes

Asteroid obj;

obj.name = "Example Object";

obj.physical.diameterKm = 0.34;

obj.classification.isNEO = true;

CAD is especially useful for the encounter portion. A CAD response contains a list of field names and corresponding data arrays. Fields include the designation, orbit ID, Julian-date and calendar representation of the close approach, nominal/minimum/maximum distance and relative velocity, with other fields depending on the requested output. The documented CAD API also supports optional diameter output.

**Engineering rule:** Check the JSON signature/version before assuming the response structure. JPL explicitly warns that if the API version in the JSON signature differs from the documented version, format compatibility is not guaranteed.

# 4. Core C++ data model

Avoid one enormous Asteroid struct containing every possible field. Separate the concepts so the model matches the science and the query requirements. The following is a recommended starting point; exact fields can be reduced or expanded after the first real API sample is parsed.

struct PhysicalProperties {

std::optional\<double\> diameterKm;

std::optional\<double\> diameterSigmaKm;

std::optional\<double\> absoluteMagnitudeH;

std::optional\<double\> albedo;

std::optional\<double\> density;

std::optional\<double\> rotationPeriodHours;

};

enum class ObjectKind {

Asteroid,

Comet

};

struct ObjectClassification {

ObjectKind kind;

bool isNEO = false;

bool isPHA = false;

std::string orbitClass;

};

struct OrbitalProperties {

std::string orbitId;

double epoch = 0.0;

double eccentricity = 0.0;

double semiMajorAxisAU = 0.0;

double perihelionAU = 0.0;

double inclinationDeg = 0.0;

double ascendingNodeDeg = 0.0;

double argumentPerihelionDeg = 0.0;

double meanAnomalyDeg = 0.0;

double moidAU = 0.0;

std::string orbitClass;

};

struct Asteroid {

std::string spkid;

std::string designation;

std::string name;

PhysicalProperties physical;

OrbitalProperties orbital;

ObjectClassification classification;

};

struct CloseApproach {

std::string objectId;

std::string orbitId;

DateTime date;

double distanceAU = 0.0;

double minDistanceAU = 0.0;

double maxDistanceAU = 0.0;

double relativeVelocityKms = 0.0;

double vInfinityKms = 0.0;

};

struct AsteroidRecord {

Asteroid object;

std::vector\<CloseApproach\> approaches;

};

**Why separate approaches?** One asteroid can have multiple close-approach events. Modeling the relationship as object -\> many CloseApproach records prevents the common mistake of assuming one object has only one encounter.

# 5. Data relationships and identifiers

The different NASA/JPL services use identifiers such as SPK-ID, primary designation and orbit ID. Your internal system needs a consistent canonical object key so records from different endpoints can be associated safely.

Canonical Object ID

\|

+--------------+--------------+

\| \| \|

SBDB CAD Sentry

\| \| \|

object data approach data risk data

A close-approach record should reference the parent object rather than duplicating the whole object. In memory this can be an object ID or an index into a master vector. In SQLite, the corresponding value behaves like a foreign key conceptually.

**Recommended approach:** Keep one master copy of each object in a primary collection and let indexes store IDs or vector positions. This avoids copying full 100,000-object records into multiple structures.

# 6. Storage and caching strategy

Use persistent local storage so the application does not have to re-download everything whenever it starts. SQLite is a practical choice because the entire database can live in a single local file while still supporting structured tables and queries.

neo.db

ASTEROIDS

---------

id / spkid

designation

name

kind

neo

pha

diameter

H

moid

orbit_class

...

CLOSE_APPROACHES

----------------

object_id

close_date

distance

min_distance

max_distance

relative_velocity

v_infinity

...

SENTRY_RISK (optional)

----------------------

object_id

impact_probability

palermo_value

impact_date

...

**Cache strategy:** Download/update NASA data in a controlled ingestion process, store the normalized result locally, and let user filters/query actions operate on the local dataset. Avoid making a new NASA request every time a user changes a UI filter.

**Large downloads:** SBDB Query supports limit and limit-from parameters for paging through large result sets. CAD also documents limit/limit-from. This allows controlled batches rather than treating a huge response as one monolithic operation.

# 7. DSA layer: what each structure is for

The database is storage. The DSA layer is your academic core. The point is not to add structures randomly; each structure should solve a specific access problem.

| **Structure / algorithm**                 | **Role in this project**                               | **Typical operation**                       | **Why it belongs**                                            |
|-------------------------------------------|--------------------------------------------------------|---------------------------------------------|---------------------------------------------------------------|
| std::vector                               | Primary in-memory collection of object records         | Sequential traversal / indexed access       | Simple, compact, cache-friendly master dataset                |
| std::unordered_map                        | Exact ID lookup index                                  | ID -\> object index                         | Fast average lookup for exact keys                            |
| Sorting                                   | Build ordered views or prepare data for binary search  | Sort by diameter/date/distance/velocity     | Supports ranking and range-style operations                   |
| Binary search / lower_bound / upper_bound | Find positions in sorted data                          | First value \>= X, last value \<= Y         | Useful for numeric range queries                              |
| Priority queue / heap                     | Top-K results                                          | Top 10 closest / fastest / largest          | Avoids full sorting when only a small ranked subset is needed |
| Time index                                | Group or locate events by year/date                    | Get approaches in a time window             | Directly matches the project time-slot concept                |
| Size index / buckets                      | Group objects into application-defined size ranges     | Get objects in 10-50 m bucket               | Makes the UI filters and categories fast                      |
| Optional tree structures                  | Advanced ordered/range indexing                        | Range / balanced search                     | Useful for demonstrating advanced DSA if taught in the course |
| Optional KD-tree                          | Spatial nearest-neighbor search                        | Find points near Earth in x/y/z             | Advanced geometry/DSA extension                               |
| Optional graph                            | Represent relationships among celestial objects/events | Traverse or analyze proximity/event network | Advanced extension, not required for MVP                      |

# 8. Query engine and multi-condition filtering

The query engine is where the structures work together. A user should be able to combine conditions rather than running one isolated filter at a time.

Example query:

Find NEOs

AND diameter \> 100 m

AND close approach year in 2030..2040

AND distance \<= 0.05 AU

Possible conceptual pipeline:

Time index

-\> candidate approach records

-\> distance filter

-\> parent object lookup

-\> NEO flag check

-\> diameter check

-\> result set

-\> optional priority queue for Top-K

The order of operations can later be optimized. For example, a highly selective condition may be applied early to reduce the candidate set. This creates a genuine algorithm-design question for the report: which filtering order produces less work for a given query workload?

# 9. Simulation / trajectory data pipeline

The first simulation does not need a full orbital-mechanics engine. The visualization can use encounter information to create a meaningful schematic motion model. The simulation input should be based on real values such as approach date, nominal/minimum distance and relative velocity, while clearly treating the on-screen path as a visualization rather than a literal-to-scale trajectory unless actual orbital propagation is implemented.

NASA/JPL record

\|

+-- approach date

+-- distance

+-- relative velocity

+-- object diameter

+-- type / classification

\|

v

Simulation mapping

\|

+-- trajectory curve / encounter path

+-- marker size

+-- animation speed under a chosen time scale

+-- timeline position

v

Existing C++ UI

**Important distinction:** Do not animate an object at literal physical scale on the screen. A real velocity such as tens of km/s combined with astronomical distances does not map sensibly to a desktop viewport. Use a declared simulation time scale and display the actual physical velocity as telemetry.

**Advanced future version:** If actual orbital propagation is added later, the engine would use orbital elements plus an epoch/reference frame and a propagation method to calculate position as a function of time. That is a separate orbital-mechanics subsystem and is not necessary for the first DSA version.

# 10. Missing, uncertain and optional data

Astronomical databases contain incomplete records. The application must distinguish a value that is unknown from a value that is genuinely zero. JPL documents, for example, that CAD diameter may be null when no diameter is available.

Bad assumption:

double diameter = 0;

// 0 could be mistaken for a real diameter

Better:

std::optional\<double\> diameterKm;

// unknown -\> std::nullopt

// known -\> value in kilometers

The same idea applies to albedo, density, rotation period and other optional physical parameters. Query logic should decide explicitly what to do with unknown values. For example, a “diameter \> 100 m” query naturally excludes records with unknown diameter rather than treating them as 0 m.

# 11. Performance and benchmarking

A strong DSA project should not stop at “it works.” Measure how the system behaves as the dataset grows. Suggested dataset sizes are 1,000; 10,000; 50,000; and 100,000 records, assuming the chosen NASA scope can supply that many relevant records.

| **Experiment**      | **Compare**                                         | **Measure**                   |
|---------------------|-----------------------------------------------------|-------------------------------|
| Exact object lookup | Linear scan vs unordered_map                        | Average lookup time           |
| Numeric range query | Linear scan vs sorted vector + binary-search bounds | Query time                    |
| Top-K closest       | Full sort vs priority queue/heap                    | Time and number of operations |
| Dataset ingestion   | Different batch sizes / parsing approaches          | Total load time               |
| Memory              | Master vector + indexes                             | RAM usage                     |
| Combined query      | Different filter orderings                          | End-to-end query time         |

**Complexity examples to explain in the report:** Linear search is O(N). Binary search on sorted data is O(log N). Hash lookup is O(1) on average under typical conditions. Heap insertion/removal is O(log N). Comparison sorting is commonly O(N log N) for efficient general-purpose algorithms such as introsort/merge sort families.

# 12. Recommended project architecture

NEO-DX / ASTRODSA

│

├── data/

│ ├── nasa_client

│ ├── json_parser

│ └── validator

│

├── model/

│ ├── asteroid

│ ├── physical_properties

│ ├── orbital_properties

│ ├── close_approach

│ └── risk_data

│

├── storage/

│ ├── sqlite_database

│ └── cache

│

├── dsa/

│ ├── master_collection

│ ├── id_index

│ ├── sorted_views

│ ├── heap

│ ├── time_index

│ └── optional_advanced_structures

│

├── query/

│ ├── filters

│ ├── range_query

│ ├── top_k

│ └── query_engine

│

├── simulation/

│ ├── encounter_model

│ └── timeline_mapping

│

└── ui/

└── existing C++ UI

A key architectural rule is that the UI should never be responsible for raw NASA parsing or complicated data management. The UI asks the query layer for results; the query layer uses the DSA engine; the DSA engine uses the normalized data/indexes.

# 13. Scope: required vs optional features

| **Core / required**                   | **Optional / extension**                  |
|---------------------------------------|-------------------------------------------|
| NASA/JPL SBDB + CAD ingestion         | Sentry risk analytics                     |
| JSON parsing and validation           | Mission Design module                     |
| C++ domain structs/classes            | AVL / Red-Black tree                      |
| Primary vector collection             | Interval tree                             |
| Exact ID lookup via hash index        | KD-tree                                   |
| Search, sorting and filtering         | Graph-based relationship analysis         |
| Time and size categorization          | Actual orbital propagation                |
| Top-K queries via heap/priority queue | 3D physically propagated orbit simulation |
| SQLite persistence/cache              | Solar-system/galactic zoom extensions     |
| Benchmarking on growing datasets      | Advanced optimization / parallelism       |

**Scope rule:** Do not let the optional features become dependencies for the core submission. The core project is already substantial if it reliably ingests real data, stores 10,000+ records, supports structured querying, uses several justified DSA techniques, and presents measurable performance results.

# 14. Semester implementation roadmap

| **Stage** | **Goal**                         | **Concrete output**                                                                              |
|-----------|----------------------------------|--------------------------------------------------------------------------------------------------|
| Stage 1   | Freeze the data model            | Finalize Asteroid, PhysicalProperties, OrbitalProperties, CloseApproach and identifier strategy. |
| Stage 2   | Parse one real SBDB response     | Get a real JSON response, inspect fields, convert one object into C++.                           |
| Stage 3   | Parse one CAD response           | Create CloseApproach objects and associate them with the correct parent object.                  |
| Stage 4   | Build a few hundred real records | Validate parsing, missing-field handling, IDs and dates before scaling.                          |
| Stage 5   | Implement core DSA               | vector, hash index, sorting, binary-search/range operations.                                     |
| Stage 6   | Implement query engine           | Combine type, size, date, distance and velocity filters.                                         |
| Stage 7   | Add priority queries             | Top-K closest, fastest, largest or other ranking operations.                                     |
| Stage 8   | Add SQLite persistence           | Store normalized records and reload them without re-downloading.                                 |
| Stage 9   | Scale and benchmark              | Run 1k / 10k / 50k / 100k experiments where appropriate.                                         |
| Stage 10  | Connect to existing UI           | Feed query results to the already-built visualization/simulation layer.                          |
| Stage 11  | Optional extensions              | Add Sentry, advanced trees, KD-tree or actual orbital propagation only after the core is stable. |

# 15. Final project definition and checklist

The final basic project can be described as follows:

*“A C++ data management and analysis system for NASA/JPL Near-Earth Object data that uses structured representations, indexing, searching, sorting, range filtering, priority queues and persistent storage to efficiently answer multi-condition asteroid and close-approach queries, with the resulting records supplied to a custom visualization layer.”*

- Real NASA/JPL source data is used; sample/hard-coded records are only for early development tests.

- The internal data model keeps object information separate from close-approach events.

- One canonical object record is maintained, with indexes pointing to it rather than duplicating complete objects.

- Time, size and type are treated as query dimensions; size ranges are application-defined categories, not NASA scientific classifications unless explicitly documented otherwise.

- Unknown values are represented explicitly rather than silently converted to zero.

- The query engine can combine multiple conditions.

- Performance is tested against increasing dataset sizes.

- The simulation layer receives processed data rather than directly parsing NASA responses.

- The fancy 3D/solar-system/galactic features remain extensions and are not necessary for the core DSA submission.

# 16. Official references

**NASA/JPL SSD APIs overview  
**https://ssd-api.jpl.nasa.gov/  
General SSD/CNEOS API information and usage guidance.

**SBDB Query API  
**https://ssd-api.jpl.nasa.gov/doc/sbdb_query.html  
Querying sets of asteroids/comets, fields, sorting, limits and paging.

**SBDB API  
**https://ssd-api.jpl.nasa.gov/doc/sbdb.html  
Object, orbital, physical and ancillary-data documentation.

**SBDB Filter  
**https://ssd-api.jpl.nasa.gov/doc/sbdb_filter.html  
Common filters such as asteroid/comet, NEO and PHA.

**Close Approach Data API  
**https://ssd-api.jpl.nasa.gov/doc/cad.html  
Close-approach fields, filtering, JSON output, optional diameter and paging.

**Sentry API  
**https://ssd-api.jpl.nasa.gov/doc/sentry.html  
Sentry query modes and impact-risk-related information.

**Mission Design API  
**https://ssd-api.jpl.nasa.gov/doc/mdesign.html  
Optional mission-design data and trajectory-related output fields.

*End of project context document*
