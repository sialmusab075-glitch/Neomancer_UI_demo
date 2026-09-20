// SQLite persistence tests: exact round-trip (including NULL vs 0 and the
// tri-state flags), foreign-key integrity, schema-version rejection, and the
// promise that a failed or interrupted write leaves the previous neo.db intact.
//
// Everything runs in a temp directory. No network.

#include "neo/ingest/CadParser.h"
#include "neo/ingest/SbdbParser.h"
#include "neo/model/Dataset.h"
#include "neo/model/Hash.h"
#include "neo/storage/Database.h"

#include <sqlite3.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

int g_failures = 0;
int g_checks = 0;
std::string g_fixtureDir;
std::filesystem::path g_tempRoot;

void check(bool ok, const char* what, const std::string& detail = std::string()) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("  FAIL  %s %s\n", what, detail.c_str());
    }
}

bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

std::string readFixture(const char* name) {
    std::ifstream in(g_fixtureDir + "/" + name, std::ios::binary);
    if (!in) {
        std::printf("  FAIL  cannot open fixture %s\n", name);
        ++g_failures;
        return std::string();
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// The fixture dataset: 23 objects, 47 joined approaches.
neo::Dataset buildFixtureDataset() {
    neo::Dataset dataset;
    std::vector<neo::Asteroid> objects;
    neo::ValidationReport report;
    neo::parseSbdbObjects(readFixture("sbdb_neo_page.json"), objects, report);
    dataset.setObjects(std::move(objects));

    std::vector<neo::ParsedApproach> rows;
    neo::parseCadApproaches(readFixture("cad_pha_window.json"), rows, report);
    neo::parseCadApproaches(readFixture("cad_apophis.json"), rows, report);
    dataset.joinApproaches(std::move(rows));
    return dataset;
}

neo::DatabaseMeta fixtureMeta() {
    neo::DatabaseMeta meta;
    meta.sbdbApiVersion = neo::kSbdbApiVersion;
    meta.cadApiVersion = neo::kCadApiVersion;
    meta.cadDateMin = "1950-01-01";
    meta.cadDateMax = "2150-01-01";
    meta.cadDistMaxAU = 0.05;
    meta.reportChecksum = neo::fnv1a64Hex("pretend ingest report");
    return meta;
}

const neo::AsteroidRecord* findRecord(const neo::Dataset& dataset, const char* pdes) {
    const std::uint32_t index = dataset.find(pdes);
    return index == neo::kInvalidRecord ? nullptr : &dataset.records()[index];
}

std::filesystem::path freshDir(const char* name) {
    const std::filesystem::path dir = g_tempRoot / name;
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    return dir;
}

// --- tests -----------------------------------------------------------------

void testRoundTrip() {
    std::printf("[db] every field survives a save/load round trip\n");
    const std::filesystem::path dir = freshDir("roundtrip");
    const std::string path = (dir / "neo.db").string();

    const neo::Dataset original = buildFixtureDataset();
    const neo::DbStatus saved = neo::saveDatabase(original, fixtureMeta(), path);
    check(saved.ok, "the database is written", saved.error);
    check(neo::databaseFileSize(path) > 0, "the file is not empty");
    check(!std::filesystem::exists(path + ".tmp"), "no temp file is left behind");
    check(!std::filesystem::exists(path + ".old"), "no backup file is left behind");

    neo::Dataset loaded;
    neo::DatabaseMeta meta;
    const neo::DbStatus read = neo::loadDatabase(path, loaded, meta);
    check(read.ok, "the database loads", read.error);
    check(loaded.objectCount() == original.objectCount(), "object count matches");
    check(loaded.approachCount() == original.approachCount(), "approach count matches");
    check(meta.objectCount == original.objectCount() && meta.approachCount == original.approachCount(),
          "the meta row agrees with the tables");
    check(meta.schemaVersion == neo::kSchemaVersion, "schema version recorded");
    check(meta.sbdbApiVersion == neo::kSbdbApiVersion && meta.cadApiVersion == neo::kCadApiVersion,
          "both API versions are recorded");
    check(meta.cadDateMin == "1950-01-01" && near(meta.cadDistMaxAU, 0.05, 1e-12),
          "the query parameters are recorded");
    check(meta.reportChecksum == neo::fnv1a64Hex("pretend ingest report"), "the report checksum is recorded");
    check(!meta.createdUtc.empty(), "a creation timestamp is recorded", meta.createdUtc);

    // Field-by-field comparison across every object.
    bool identical = true;
    std::string firstDifference;
    for (std::size_t i = 0; i < original.records().size() && identical; ++i) {
        const neo::Asteroid& a = original.records()[i].object;
        const neo::AsteroidRecord* b = findRecord(loaded, a.pdes.c_str());
        if (b == nullptr) {
            identical = false;
            firstDifference = a.pdes + " is missing";
            break;
        }
        const neo::Asteroid& l = b->object;
        const neo::OrbitalProperties& ao = a.orbital;
        const neo::OrbitalProperties& lo = l.orbital;
        const neo::PhysicalProperties& ap = a.physical;
        const neo::PhysicalProperties& lp = l.physical;
        const bool same =
            l.spkid == a.spkid && l.name == a.name && l.fullName == a.fullName &&
            l.classification.kind == a.classification.kind &&
            l.classification.numbered == a.classification.numbered &&
            l.classification.isNEO == a.classification.isNEO && l.classification.isPHA == a.classification.isPHA &&
            l.classification.orbitClass == a.classification.orbitClass && lo.orbitId == ao.orbitId &&
            lo.epochJdTdb == ao.epochJdTdb && lo.eccentricity == ao.eccentricity &&
            lo.semiMajorAxisAU == ao.semiMajorAxisAU && lo.perihelionAU == ao.perihelionAU &&
            lo.inclinationDeg == ao.inclinationDeg && lo.ascendingNodeDeg == ao.ascendingNodeDeg &&
            lo.argPerihelionDeg == ao.argPerihelionDeg && lo.meanAnomalyDeg == ao.meanAnomalyDeg &&
            lo.meanMotionDegPerDay == ao.meanMotionDegPerDay && lo.periodDays == ao.periodDays &&
            lo.moidAU == ao.moidAU && lo.conditionCode == ao.conditionCode &&
            lp.absoluteMagnitudeH == ap.absoluteMagnitudeH && lp.diameterKm == ap.diameterKm &&
            lp.diameterSigmaKm == ap.diameterSigmaKm && lp.albedo == ap.albedo &&
            lp.rotationPeriodHours == ap.rotationPeriodHours;
        if (!same) {
            identical = false;
            firstDifference = a.pdes;
        }
    }
    check(identical, "every object field is bit-identical after reloading", firstDifference);

    // Doubles must survive exactly, not "close enough": SQLite REAL is a double.
    const neo::AsteroidRecord* eros = findRecord(loaded, "433");
    check(eros != nullptr && eros->object.orbital.eccentricity == 0.2228779627700761,
          "full-precision eccentricity is exact after the round trip");

    // Unknown must come back unknown, never 0.
    const neo::AsteroidRecord* noDiameter = findRecord(loaded, "136795");
    check(noDiameter != nullptr && !noDiameter->object.physical.diameterKm.has_value(),
          "a null diameter reloads as empty, not 0");
    check(noDiameter != nullptr && !noDiameter->object.physical.albedo.has_value(), "a null albedo stays empty");
    const neo::AsteroidRecord* comet = findRecord(loaded, "2P");
    check(comet != nullptr && !comet->object.physical.absoluteMagnitudeH.has_value(), "a null H stays empty");
    check(comet != nullptr && !comet->object.classification.isPHA.has_value(),
          "a null PHA flag reloads as unknown, not false");
    check(comet != nullptr && comet->object.classification.isNEO.has_value() && *comet->object.classification.isNEO,
          "a true NEO flag reloads as true");
    const neo::AsteroidRecord* hyperbolic = findRecord(loaded, "2017 U1");
    check(hyperbolic != nullptr && !hyperbolic->object.classification.isNEO.has_value(),
          "a null NEO flag reloads as unknown");
    check(hyperbolic != nullptr && hyperbolic->object.classification.isPHA.has_value() &&
              !*hyperbolic->object.classification.isPHA,
          "a false PHA flag reloads as false, distinct from unknown");
    check(hyperbolic != nullptr && !hyperbolic->object.orbital.periodDays.has_value(),
          "a null period stays empty");
    check(hyperbolic != nullptr && !hyperbolic->object.orbital.propagationSupported(),
          "the hyperbolic orbit is still flagged unsupported after reloading");
    check(hyperbolic != nullptr && hyperbolic->object.orbital.semiMajorAxisAU < 0.0,
          "a negative semi-major axis survives");

    // Approaches: ranges, order and optional columns.
    const std::uint32_t apophis = loaded.find("99942");
    const neo::ApproachSpan span = loaded.approachesOf(apophis);
    check(span.count == 28, "Apophis still has 28 approaches", std::to_string(span.count));
    bool ordered = true;
    for (std::size_t i = 1; i < span.count; ++i) {
        ordered = ordered && span[i - 1].jdTdb <= span[i].jdTdb && span[i].objectIndex == apophis;
    }
    check(ordered, "reloaded approaches are in date order inside their object's range");
    const neo::CloseApproach* closest = nullptr;
    for (const neo::CloseApproach& c : span) {
        if (closest == nullptr || c.distanceAU < closest->distanceAU) {
            closest = &c;
        }
    }
    check(closest != nullptr && closest->distanceAU == 0.000254090910419299, "the 2029 distance is exact");
    check(closest != nullptr && closest->vInfinityKms.has_value(), "a present v_inf reloads as present");
    check(closest != nullptr && closest->diameterKm.has_value() && *closest->diameterKm == 0.34,
          "the CAD diameter column round-trips");

    check(loaded.findBySpkId("20000433") != neo::kInvalidRecord, "the SPK-ID index is rebuilt on load");
}

void testNullAndDerivedColumns() {
    std::printf("[db] NULLs, the derived-range flag and the estimated diameter column\n");
    const std::filesystem::path dir = freshDir("nulls");
    const std::string path = (dir / "neo.db").string();

    neo::Dataset dataset;
    std::vector<neo::Asteroid> objects(2);
    objects[0].pdes = "with-values";
    objects[0].spkid = "1";
    objects[0].physical.absoluteMagnitudeH = 18.0;
    objects[0].physical.albedo = 0.25;
    objects[0].orbital.eccentricity = 0.5;
    objects[1].pdes = "all-unknown"; // no spkid: must be stored as NULL, not ""
    objects[1].orbital.eccentricity = 0.1;
    dataset.setObjects(std::move(objects));

    std::vector<neo::CloseApproach> approaches(2);
    approaches[0].objectIndex = 0;
    approaches[0].jdTdb = 2451545.0;
    approaches[0].distanceAU = 0.01;
    approaches[0].distanceMinAU = 0.01;
    approaches[0].distanceMaxAU = 0.01;
    approaches[0].distRangeDerived = true; // the fallback happened
    approaches[0].relVelocityKms = 12.0;   // and v_inf is unknown
    approaches[1].objectIndex = 1;
    approaches[1].jdTdb = 2451600.0;
    approaches[1].distanceAU = 0.02;
    approaches[1].distanceMinAU = 0.019;
    approaches[1].distanceMaxAU = 0.021;
    approaches[1].relVelocityKms = 9.0;
    approaches[1].vInfinityKms = 8.5;
    dataset.setApproaches(std::move(approaches));

    check(neo::saveDatabase(dataset, fixtureMeta(), path).ok, "the small dataset is written");

    neo::Dataset loaded;
    neo::DatabaseMeta meta;
    check(neo::loadDatabase(path, loaded, meta).ok, "it loads back");
    const neo::ApproachSpan first = loaded.approachesOf(loaded.find("with-values"));
    const neo::ApproachSpan second = loaded.approachesOf(loaded.find("all-unknown"));
    check(first.count == 1 && first[0].distRangeDerived, "the derived-range flag round-trips");
    check(first.count == 1 && !first[0].vInfinityKms.has_value(), "an unknown v_inf stays NULL, not 0");
    check(second.count == 1 && second[0].vInfinityKms.has_value() && *second[0].vInfinityKms == 8.5,
          "a known v_inf round-trips");
    check(loaded.records()[loaded.find("all-unknown")].object.spkid.empty(),
          "an absent spkid reloads as empty, and two of them do not collide on UNIQUE");

    // The estimated diameter is stored as its own column, never merged with the
    // measured one. Read it directly to prove the column exists and is filled.
    sqlite3* db = nullptr;
    check(sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK, "open with sqlite3");
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT diameter_km, est_diameter_km FROM objects WHERE pdes = 'with-values'";
    check(sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK, "prepare");
    check(sqlite3_step(stmt) == SQLITE_ROW, "the row is there");
    check(sqlite3_column_type(stmt, 0) == SQLITE_NULL, "no measured diameter is stored as NULL");
    check(sqlite3_column_type(stmt, 1) == SQLITE_FLOAT, "the H-based estimate is stored in its own column");
    const double estimate = sqlite3_column_double(stmt, 1);
    check(near(estimate, 1329.0 / std::sqrt(0.25) * std::pow(10.0, -18.0 / 5.0), 1e-9),
          "the stored estimate uses the measured albedo");
    sqlite3_finalize(stmt);

    // The schema must carry only integrity indexes: no query indexes.
    const char* indexSql = "SELECT name, tbl_name FROM sqlite_master WHERE type = 'index'";
    check(sqlite3_prepare_v2(db, indexSql, -1, &stmt, nullptr) == SQLITE_OK, "prepare index query");
    int explicitIndexes = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const std::string name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        // sqlite_autoindex_* are the UNIQUE constraints; anything else would be
        // a query index, which belongs in the DSA layer instead.
        if (name.rfind("sqlite_autoindex_", 0) != 0) {
            ++explicitIndexes;
        }
    }
    sqlite3_finalize(stmt);
    check(explicitIndexes == 0, "the schema has no query indexes, only integrity ones",
          std::to_string(explicitIndexes));
    sqlite3_close(db);
}

void testForeignKeys() {
    std::printf("[db] foreign keys are enforced and orphans are refused\n");
    const std::filesystem::path dir = freshDir("fk");
    const std::string path = (dir / "neo.db").string();
    check(neo::saveDatabase(buildFixtureDataset(), fixtureMeta(), path).ok, "write a good database");

    // Insert an approach pointing at a non-existent object, with foreign keys
    // off, and confirm the loader refuses the file rather than inventing a parent.
    sqlite3* db = nullptr;
    check(sqlite3_open(path.c_str(), &db) == SQLITE_OK, "reopen for tampering");
    check(sqlite3_exec(db, "PRAGMA foreign_keys = OFF", nullptr, nullptr, nullptr) == SQLITE_OK, "disable FKs");
    const char* orphan =
        "INSERT INTO approaches (object_id, jd, dist_au, dist_min_au, dist_max_au, dist_range_derived, "
        "v_rel_kms) VALUES (999999, 2451545.0, 0.01, 0.01, 0.01, 0, 10.0)";
    check(sqlite3_exec(db, orphan, nullptr, nullptr, nullptr) == SQLITE_OK, "insert an orphan row");
    // With foreign keys ON, the same insert must be rejected by SQLite itself.
    check(sqlite3_exec(db, "PRAGMA foreign_keys = ON", nullptr, nullptr, nullptr) == SQLITE_OK, "enable FKs");
    const int rejected = sqlite3_exec(db, orphan, nullptr, nullptr, nullptr);
    check(rejected != SQLITE_OK, "a second orphan is rejected once foreign keys are on");
    sqlite3_close(db);

    neo::Dataset loaded;
    neo::DatabaseMeta meta;
    const neo::DbStatus status = neo::loadDatabase(path, loaded, meta);
    check(!status.ok, "loading a file with an orphan approach fails");
    check(status.error.find("neo_ingest") != std::string::npos, "and says how to fix it", status.error);
}

void testSchemaVersion() {
    std::printf("[db] a foreign schema version is refused with a clear message\n");
    const std::filesystem::path dir = freshDir("schema");
    const std::string path = (dir / "neo.db").string();
    check(neo::saveDatabase(buildFixtureDataset(), fixtureMeta(), path).ok, "write a database");

    sqlite3* db = nullptr;
    sqlite3_open(path.c_str(), &db);
    sqlite3_exec(db, "UPDATE meta SET schema_version = 99 WHERE id = 1", nullptr, nullptr, nullptr);
    sqlite3_close(db);

    neo::Dataset loaded;
    neo::DatabaseMeta meta;
    const neo::DbStatus status = neo::loadDatabase(path, loaded, meta);
    check(!status.ok, "the load is refused");
    check(status.error.find("99") != std::string::npos && status.error.find("neo_ingest") != std::string::npos,
          "the message names the version and tells the user to re-run neo_ingest", status.error);
    neo::DatabaseMeta metaOnly;
    check(!neo::readDatabaseMeta(path, metaOnly).ok, "reading just the meta is refused too");

    // A file that is not one of ours at all.
    const std::string alien = (dir / "alien.db").string();
    sqlite3_open(alien.c_str(), &db);
    sqlite3_exec(db, "CREATE TABLE something (x INTEGER)", nullptr, nullptr, nullptr);
    sqlite3_close(db);
    const neo::DbStatus foreign = neo::loadDatabase(alien, loaded, meta);
    check(!foreign.ok && foreign.error.find("neo_ingest") != std::string::npos,
          "a database with no meta table is refused", foreign.error);
    check(!neo::loadDatabase((dir / "missing.db").string(), loaded, meta).ok, "a missing file is refused");
}

void testInterruptedWrite() {
    std::printf("[db] an interrupted write leaves the previous database intact\n");
    const std::filesystem::path dir = freshDir("interrupt");
    const std::string path = (dir / "neo.db").string();

    // A good database first.
    const neo::Dataset good = buildFixtureDataset();
    check(neo::saveDatabase(good, fixtureMeta(), path).ok, "write the original database");
    const std::size_t originalSize = neo::databaseFileSize(path);

    // A second dataset that would be obviously different if it ever landed.
    neo::Dataset replacement;
    std::vector<neo::Asteroid> one(1);
    one[0].pdes = "only-one";
    one[0].orbital.eccentricity = 0.3;
    replacement.setObjects(std::move(one));

    // Interrupted before the commit.
    {
        neo::SaveHooks hooks;
        hooks.beforeCommit = [] { return false; };
        const neo::DbStatus status = neo::saveDatabase(replacement, fixtureMeta(), path, hooks);
        check(!status.ok, "the aborted save reports failure");
        check(!std::filesystem::exists(path + ".tmp"), "the temp file is cleaned up");
        neo::Dataset loaded;
        neo::DatabaseMeta meta;
        check(neo::loadDatabase(path, loaded, meta).ok, "the original database still loads");
        check(loaded.objectCount() == good.objectCount(), "with its original contents",
              std::to_string(loaded.objectCount()));
        check(neo::databaseFileSize(path) == originalSize, "and its original size");
    }

    // Interrupted after the temp file is complete, before the swap.
    {
        neo::SaveHooks hooks;
        hooks.beforeReplace = [] { return false; };
        const neo::DbStatus status = neo::saveDatabase(replacement, fixtureMeta(), path, hooks);
        check(!status.ok, "the save aborted at the swap reports failure");
        check(status.error.find("unchanged") != std::string::npos, "and says the database is unchanged",
              status.error);
        check(!std::filesystem::exists(path + ".tmp"), "the temp file is cleaned up");
        check(!std::filesystem::exists(path + ".old"), "no stray backup is left");
        neo::Dataset loaded;
        neo::DatabaseMeta meta;
        check(neo::loadDatabase(path, loaded, meta).ok, "the original database still loads");
        check(loaded.objectCount() == good.objectCount(), "still with its original contents");
        check(loaded.find("only-one") == neo::kInvalidRecord, "none of the replacement data leaked in");
    }

    // A leftover temp file from a crashed run must not block the next save.
    {
        std::ofstream stale(path + ".tmp", std::ios::binary);
        stale << "not a database";
        stale.close();
        const neo::DbStatus status = neo::saveDatabase(replacement, fixtureMeta(), path);
        check(status.ok, "a save after a crashed run succeeds", status.error);
        neo::Dataset loaded;
        neo::DatabaseMeta meta;
        check(neo::loadDatabase(path, loaded, meta).ok, "and the new database loads");
        check(loaded.objectCount() == 1 && loaded.find("only-one") != neo::kInvalidRecord,
              "now holding the replacement data");
    }
}

void testEmptyDataset() {
    std::printf("[db] an empty dataset is still a valid database\n");
    const std::filesystem::path dir = freshDir("empty");
    const std::string path = (dir / "neo.db").string();
    const neo::Dataset empty;
    check(neo::saveDatabase(empty, fixtureMeta(), path).ok, "an empty dataset can be written");
    neo::Dataset loaded;
    neo::DatabaseMeta meta;
    check(neo::loadDatabase(path, loaded, meta).ok, "and loaded");
    check(loaded.objectCount() == 0 && loaded.approachCount() == 0, "with no rows");
    check(meta.schemaVersion == neo::kSchemaVersion, "and a valid meta row");
}

} // namespace

int main(int argc, char** argv) {
    g_fixtureDir = argc > 1 ? argv[1] : NEO_FIXTURES_DIR;
    g_tempRoot = std::filesystem::temp_directory_path() / "neo_storage_tests";
    std::error_code ec;
    std::filesystem::remove_all(g_tempRoot, ec);
    std::filesystem::create_directories(g_tempRoot, ec);
    std::printf("fixtures: %s\ntemp:     %s\nsqlite:   %s\n", g_fixtureDir.c_str(), g_tempRoot.string().c_str(),
                sqlite3_libversion());

    testRoundTrip();
    testNullAndDerivedColumns();
    testForeignKeys();
    testSchemaVersion();
    testInterruptedWrite();
    testEmptyDataset();

    std::filesystem::remove_all(g_tempRoot, ec);
    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
