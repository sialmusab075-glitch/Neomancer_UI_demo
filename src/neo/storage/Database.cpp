#include "neo/storage/Database.h"

#include "neo/model/JulianDate.h"

#include <sqlite3.h>

#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace neo {

namespace {

// --- small RAII wrappers ---------------------------------------------------

class Statement {
public:
    Statement() = default;
    ~Statement() { finalize(); }
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    bool prepare(sqlite3* db, const char* sql) {
        finalize();
        return sqlite3_prepare_v2(db, sql, -1, &stmt_, nullptr) == SQLITE_OK;
    }
    void finalize() {
        if (stmt_ != nullptr) {
            sqlite3_finalize(stmt_);
            stmt_ = nullptr;
        }
    }
    sqlite3_stmt* get() const { return stmt_; }

    void bindNull(int i) { sqlite3_bind_null(stmt_, i); }
    void bind(int i, double v) { sqlite3_bind_double(stmt_, i, v); }
    void bind(int i, sqlite3_int64 v) { sqlite3_bind_int64(stmt_, i, v); }
    void bind(int i, const std::string& v) {
        sqlite3_bind_text(stmt_, i, v.c_str(), static_cast<int>(v.size()), SQLITE_TRANSIENT);
    }
    // "Unknown" is written as SQL NULL, never as 0 or an empty string.
    void bind(int i, const std::optional<double>& v) {
        if (v) {
            bind(i, *v);
        } else {
            bindNull(i);
        }
    }
    void bind(int i, const std::optional<int>& v) {
        if (v) {
            bind(i, static_cast<sqlite3_int64>(*v));
        } else {
            bindNull(i);
        }
    }
    void bind(int i, const std::optional<bool>& v) {
        if (v) {
            bind(i, static_cast<sqlite3_int64>(*v ? 1 : 0));
        } else {
            bindNull(i);
        }
    }
    void bindTextOrNull(int i, const std::string& v) {
        if (v.empty()) {
            bindNull(i);
        } else {
            bind(i, v);
        }
    }

    int step() { return sqlite3_step(stmt_); }
    bool reset() { return sqlite3_reset(stmt_) == SQLITE_OK; }

    bool isNull(int col) const { return sqlite3_column_type(stmt_, col) == SQLITE_NULL; }
    double columnDouble(int col) const { return sqlite3_column_double(stmt_, col); }
    sqlite3_int64 columnInt(int col) const { return sqlite3_column_int64(stmt_, col); }
    std::string columnText(int col) const {
        const unsigned char* text = sqlite3_column_text(stmt_, col);
        if (text == nullptr) {
            return std::string();
        }
        return std::string(reinterpret_cast<const char*>(text),
                           static_cast<std::size_t>(sqlite3_column_bytes(stmt_, col)));
    }
    std::optional<double> optionalDouble(int col) const {
        if (isNull(col)) {
            return std::nullopt;
        }
        return columnDouble(col);
    }
    std::optional<int> optionalInt(int col) const {
        if (isNull(col)) {
            return std::nullopt;
        }
        return static_cast<int>(columnInt(col));
    }
    std::optional<bool> optionalBool(int col) const {
        if (isNull(col)) {
            return std::nullopt;
        }
        return columnInt(col) != 0;
    }

private:
    sqlite3_stmt* stmt_ = nullptr;
};

class Connection {
public:
    Connection() = default;
    ~Connection() { close(); }
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    bool open(const std::string& path, int flags) {
        close();
        return sqlite3_open_v2(path.c_str(), &db_, flags, nullptr) == SQLITE_OK;
    }
    void close() {
        if (db_ != nullptr) {
            sqlite3_close(db_);
            db_ = nullptr;
        }
    }
    sqlite3* get() const { return db_; }
    bool exec(const char* sql) { return sqlite3_exec(db_, sql, nullptr, nullptr, nullptr) == SQLITE_OK; }
    std::string lastError() const { return db_ == nullptr ? "no database" : sqlite3_errmsg(db_); }

private:
    sqlite3* db_ = nullptr;
};

const char* const kSchemaSql = R"(
CREATE TABLE meta (
    id               INTEGER PRIMARY KEY CHECK (id = 1),
    schema_version   INTEGER NOT NULL,
    created_utc      TEXT    NOT NULL,
    sbdb_api_version TEXT,
    cad_api_version  TEXT,
    cad_date_min     TEXT,
    cad_date_max     TEXT,
    cad_dist_max_au  REAL,
    object_count     INTEGER NOT NULL,
    approach_count   INTEGER NOT NULL,
    report_checksum  TEXT
);

CREATE TABLE objects (
    id                INTEGER PRIMARY KEY,
    pdes              TEXT NOT NULL UNIQUE,
    spkid             TEXT UNIQUE,
    name              TEXT,
    full_name         TEXT,
    kind              INTEGER NOT NULL,
    numbered          INTEGER NOT NULL,
    is_neo            INTEGER,
    is_pha            INTEGER,
    orbit_class       TEXT,
    orbit_id          TEXT,
    epoch_jd          REAL NOT NULL,
    e                 REAL NOT NULL,
    a_au              REAL NOT NULL,
    q_au              REAL NOT NULL,
    i_deg             REAL NOT NULL,
    om_deg            REAL NOT NULL,
    w_deg             REAL NOT NULL,
    ma_deg            REAL NOT NULL,
    n_deg_per_day     REAL NOT NULL,
    period_days       REAL,
    moid_au           REAL,
    condition_code    INTEGER,
    h_mag             REAL,
    diameter_km       REAL,
    diameter_sigma_km REAL,
    albedo            REAL,
    rot_per_hours     REAL,
    est_diameter_km   REAL
);

CREATE TABLE approaches (
    id                 INTEGER PRIMARY KEY,
    object_id          INTEGER NOT NULL REFERENCES objects(id),
    jd                 REAL NOT NULL,
    dist_au            REAL NOT NULL,
    dist_min_au        REAL NOT NULL,
    dist_max_au        REAL NOT NULL,
    dist_range_derived INTEGER NOT NULL,
    v_rel_kms          REAL NOT NULL,
    v_inf_kms          REAL,
    h_mag              REAL,
    diameter_km        REAL,
    diameter_sigma_km  REAL
);
)";

// Columns in the order the object queries use them.
const char* const kInsertObjectSql =
    "INSERT INTO objects (id, pdes, spkid, name, full_name, kind, numbered, is_neo, is_pha, orbit_class, "
    "orbit_id, epoch_jd, e, a_au, q_au, i_deg, om_deg, w_deg, ma_deg, n_deg_per_day, period_days, moid_au, "
    "condition_code, h_mag, diameter_km, diameter_sigma_km, albedo, rot_per_hours, est_diameter_km) "
    "VALUES (?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,?16,?17,?18,?19,?20,?21,?22,?23,?24,?25,?26,"
    "?27,?28,?29)";

const char* const kSelectObjectsSql =
    "SELECT id, pdes, spkid, name, full_name, kind, numbered, is_neo, is_pha, orbit_class, orbit_id, epoch_jd, "
    "e, a_au, q_au, i_deg, om_deg, w_deg, ma_deg, n_deg_per_day, period_days, moid_au, condition_code, h_mag, "
    "diameter_km, diameter_sigma_km, albedo, rot_per_hours FROM objects ORDER BY id";

const char* const kInsertApproachSql =
    "INSERT INTO approaches (object_id, jd, dist_au, dist_min_au, dist_max_au, dist_range_derived, v_rel_kms, "
    "v_inf_kms, h_mag, diameter_km, diameter_sigma_km) VALUES (?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11)";

const char* const kSelectApproachesSql =
    "SELECT object_id, jd, dist_au, dist_min_au, dist_max_au, dist_range_derived, v_rel_kms, v_inf_kms, h_mag, "
    "diameter_km, diameter_sigma_km FROM approaches";

const char* const kInsertMetaSql =
    "INSERT INTO meta (id, schema_version, created_utc, sbdb_api_version, cad_api_version, cad_date_min, "
    "cad_date_max, cad_dist_max_au, object_count, approach_count, report_checksum) "
    "VALUES (1,?1,?2,?3,?4,?5,?6,?7,?8,?9,?10)";

const char* const kSelectMetaSql =
    "SELECT schema_version, created_utc, sbdb_api_version, cad_api_version, cad_date_min, cad_date_max, "
    "cad_dist_max_au, object_count, approach_count, report_checksum FROM meta WHERE id = 1";

bool countRows(Connection& db, const char* table, sqlite3_int64& out) {
    Statement stmt;
    const std::string sql = std::string("SELECT COUNT(*) FROM ") + table;
    if (!stmt.prepare(db.get(), sql.c_str()) || stmt.step() != SQLITE_ROW) {
        return false;
    }
    out = stmt.columnInt(0);
    return true;
}

DbStatus fail(const std::string& what, Connection& db) {
    return DbStatus::failure(what + ": " + db.lastError());
}

} // namespace

// ---------------------------------------------------------------------------
// save
// ---------------------------------------------------------------------------

namespace {

// Builds the complete temp database. Every exit closes the connection (the
// Connection and Statement destructors run on return), which matters on
// Windows: an open handle makes the caller's cleanup of the temp file fail.
DbStatus buildTempDatabase(const Dataset& dataset, const DatabaseMeta& metaIn, const std::string& tmpPath,
                           const SaveHooks& hooks) {
    {
        Connection db;
        if (!db.open(tmpPath, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE)) {
            return DbStatus::failure("cannot create " + tmpPath + ": " + db.lastError());
        }
        // The temp file is disposable: if the process dies mid-write, it is
        // deleted and neo.db was never touched. So durability pragmas that cost
        // time buy nothing here.
        db.exec("PRAGMA journal_mode = OFF");
        db.exec("PRAGMA synchronous = OFF");
        if (!db.exec("PRAGMA foreign_keys = ON")) {
            return fail("cannot enable foreign keys", db);
        }
        if (!db.exec(kSchemaSql)) {
            return fail("cannot create the schema", db);
        }
        if (!db.exec("BEGIN IMMEDIATE")) {
            return fail("cannot begin the transaction", db);
        }

        Statement object;
        Statement approach;
        if (!object.prepare(db.get(), kInsertObjectSql) || !approach.prepare(db.get(), kInsertApproachSql)) {
            return fail("cannot prepare the insert statements", db);
        }

        for (std::size_t i = 0; i < dataset.records().size(); ++i) {
            const Asteroid& a = dataset.records()[i].object;
            const OrbitalProperties& o = a.orbital;
            const PhysicalProperties& p = a.physical;
            object.reset();
            object.bind(1, static_cast<sqlite3_int64>(i + 1)); // ids are 1-based and dense
            object.bind(2, a.pdes);
            object.bindTextOrNull(3, a.spkid); // NULL, not "", so UNIQUE tolerates missing ids
            object.bindTextOrNull(4, a.name);
            object.bindTextOrNull(5, a.fullName);
            object.bind(6, static_cast<sqlite3_int64>(a.classification.kind));
            object.bind(7, static_cast<sqlite3_int64>(a.classification.numbered ? 1 : 0));
            object.bind(8, a.classification.isNEO);
            object.bind(9, a.classification.isPHA);
            object.bindTextOrNull(10, a.classification.orbitClass);
            object.bindTextOrNull(11, o.orbitId);
            object.bind(12, o.epochJdTdb);
            object.bind(13, o.eccentricity);
            object.bind(14, o.semiMajorAxisAU);
            object.bind(15, o.perihelionAU);
            object.bind(16, o.inclinationDeg);
            object.bind(17, o.ascendingNodeDeg);
            object.bind(18, o.argPerihelionDeg);
            object.bind(19, o.meanAnomalyDeg);
            object.bind(20, o.meanMotionDegPerDay);
            object.bind(21, o.periodDays);
            object.bind(22, o.moidAU);
            object.bind(23, o.conditionCode);
            object.bind(24, p.absoluteMagnitudeH);
            object.bind(25, p.diameterKm);
            object.bind(26, p.diameterSigmaKm);
            object.bind(27, p.albedo);
            object.bind(28, p.rotationPeriodHours);
            // Measured and estimated diameters are separate columns and are
            // never merged: a query must be able to accept one and not the other.
            object.bind(29, p.estimatedDiameterKm());
            if (object.step() != SQLITE_DONE) {
                return fail("cannot insert object " + a.pdes, db);
            }
        }

        for (const CloseApproach& c : dataset.approaches()) {
            approach.reset();
            approach.bind(1, static_cast<sqlite3_int64>(c.objectIndex + 1));
            approach.bind(2, c.jdTdb);
            approach.bind(3, c.distanceAU);
            approach.bind(4, c.distanceMinAU);
            approach.bind(5, c.distanceMaxAU);
            approach.bind(6, static_cast<sqlite3_int64>(c.distRangeDerived ? 1 : 0));
            approach.bind(7, c.relVelocityKms);
            approach.bind(8, c.vInfinityKms); // NULL when JPL gave none
            approach.bind(9, c.absoluteMagnitudeH);
            approach.bind(10, c.diameterKm);
            approach.bind(11, c.diameterSigmaKm);
            if (approach.step() != SQLITE_DONE) {
                return fail("cannot insert an approach", db);
            }
        }

        DatabaseMeta meta = metaIn;
        meta.schemaVersion = kSchemaVersion;
        meta.objectCount = dataset.objectCount();
        meta.approachCount = dataset.approachCount();
        if (meta.createdUtc.empty()) {
            meta.createdUtc = utcNowIso();
        }
        Statement metaStmt;
        if (!metaStmt.prepare(db.get(), kInsertMetaSql)) {
            return fail("cannot prepare the meta insert", db);
        }
        metaStmt.bind(1, static_cast<sqlite3_int64>(meta.schemaVersion));
        metaStmt.bind(2, meta.createdUtc);
        metaStmt.bindTextOrNull(3, meta.sbdbApiVersion);
        metaStmt.bindTextOrNull(4, meta.cadApiVersion);
        metaStmt.bindTextOrNull(5, meta.cadDateMin);
        metaStmt.bindTextOrNull(6, meta.cadDateMax);
        metaStmt.bind(7, meta.cadDistMaxAU);
        metaStmt.bind(8, static_cast<sqlite3_int64>(meta.objectCount));
        metaStmt.bind(9, static_cast<sqlite3_int64>(meta.approachCount));
        metaStmt.bindTextOrNull(10, meta.reportChecksum);
        if (metaStmt.step() != SQLITE_DONE) {
            return fail("cannot insert the meta row", db);
        }

        if (hooks.beforeCommit && !hooks.beforeCommit()) {
            return DbStatus::failure("save aborted before commit");
        }
        if (!db.exec("COMMIT")) {
            return fail("cannot commit", db);
        }

        // Verify what was written, inside the temp file, before it can replace
        // anything: counts first, then the foreign keys.
        sqlite3_int64 objects = 0, approaches = 0;
        if (!countRows(db, "objects", objects) || !countRows(db, "approaches", approaches)) {
            return fail("cannot count the written rows", db);
        }
        if (objects != static_cast<sqlite3_int64>(dataset.objectCount()) ||
            approaches != static_cast<sqlite3_int64>(dataset.approachCount())) {
            char buf[192];
            std::snprintf(buf, sizeof buf,
                          "row count mismatch: wrote %lld objects / %lld approaches, expected %zu / %zu",
                          static_cast<long long>(objects), static_cast<long long>(approaches),
                          dataset.objectCount(), dataset.approachCount());
            return DbStatus::failure(buf);
        }
        Statement check;
        if (!check.prepare(db.get(), "PRAGMA foreign_key_check")) {
            return fail("cannot run the foreign key check", db);
        }
        if (check.step() == SQLITE_ROW) {
            return DbStatus::failure("foreign key check failed: an approach points at no object");
        }
    } // the connection closes here, so the file can be renamed or deleted
    return DbStatus::success();
}

} // namespace

DbStatus saveDatabase(const Dataset& dataset, const DatabaseMeta& metaIn, const std::string& path,
                      const SaveHooks& hooks) {
    const std::string tmpPath = path + ".tmp";
    const std::string oldPath = path + ".old";
    std::error_code ec;
    const std::filesystem::path target(path);
    if (target.has_parent_path()) {
        std::filesystem::create_directories(target.parent_path(), ec);
    }
    std::filesystem::remove(tmpPath, ec); // a leftover temp from a crashed run

    // Any failure while building leaves the temp file behind, so it is deleted
    // here, where the connection is guaranteed closed. The existing database is
    // untouched either way: nothing above this point can write to it.
    const DbStatus built = buildTempDatabase(dataset, metaIn, tmpPath, hooks);
    if (!built) {
        std::filesystem::remove(tmpPath, ec);
        return DbStatus::failure(built.error + "; " + path + " is unchanged");
    }

    if (hooks.beforeReplace && !hooks.beforeReplace()) {
        std::filesystem::remove(tmpPath, ec);
        return DbStatus::failure("save aborted before replacing " + path + "; it is unchanged");
    }

    // Swap the files. std::filesystem::rename onto an existing path is not
    // reliably atomic on Windows, so the old file is moved aside first and put
    // back if the rename fails: at no point is the old database deleted before
    // the new one is in place.
    const bool hadPrevious = std::filesystem::exists(path, ec);
    if (hadPrevious) {
        std::filesystem::remove(oldPath, ec);
        std::filesystem::rename(path, oldPath, ec);
        if (ec) {
            std::filesystem::remove(tmpPath, ec);
            return DbStatus::failure("cannot move the existing " + path + " aside: " + ec.message());
        }
    }
    std::filesystem::rename(tmpPath, path, ec);
    if (ec) {
        const std::string message = ec.message();
        if (hadPrevious) {
            std::error_code restore;
            std::filesystem::rename(oldPath, path, restore); // put the old file back
        }
        std::filesystem::remove(tmpPath, ec);
        return DbStatus::failure("cannot move the new database into place: " + message);
    }
    if (hadPrevious) {
        std::filesystem::remove(oldPath, ec);
    }
    return DbStatus::success();
}

// ---------------------------------------------------------------------------
// load
// ---------------------------------------------------------------------------

namespace {

DbStatus readMetaRow(Connection& db, DatabaseMeta& meta) {
    Statement stmt;
    if (!stmt.prepare(db.get(), kSelectMetaSql)) {
        return DbStatus::failure("this file has no meta table: it was not written by neo_ingest");
    }
    if (stmt.step() != SQLITE_ROW) {
        return DbStatus::failure("the meta table is empty: the database is incomplete, re-run neo_ingest");
    }
    meta.schemaVersion = static_cast<int>(stmt.columnInt(0));
    meta.createdUtc = stmt.columnText(1);
    meta.sbdbApiVersion = stmt.columnText(2);
    meta.cadApiVersion = stmt.columnText(3);
    meta.cadDateMin = stmt.columnText(4);
    meta.cadDateMax = stmt.columnText(5);
    meta.cadDistMaxAU = stmt.columnDouble(6);
    meta.objectCount = static_cast<std::size_t>(stmt.columnInt(7));
    meta.approachCount = static_cast<std::size_t>(stmt.columnInt(8));
    meta.reportChecksum = stmt.columnText(9);

    if (meta.schemaVersion != kSchemaVersion) {
        char buf[256];
        std::snprintf(buf, sizeof buf,
                      "database schema version %d, but this build reads version %d. "
                      "Re-run neo_ingest (it can rebuild from the cache: neo_ingest --rebuild-db).",
                      meta.schemaVersion, kSchemaVersion);
        return DbStatus::failure(buf);
    }
    return DbStatus::success();
}

} // namespace

DbStatus readDatabaseMeta(const std::string& path, DatabaseMeta& meta) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        return DbStatus::failure("no database at " + path + "; run neo_ingest to create it");
    }
    Connection db;
    if (!db.open(path, SQLITE_OPEN_READONLY)) {
        return DbStatus::failure("cannot open " + path + ": " + db.lastError());
    }
    return readMetaRow(db, meta);
}

DbStatus loadDatabase(const std::string& path, Dataset& dataset, DatabaseMeta& meta) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        return DbStatus::failure("no database at " + path + "; run neo_ingest to create it");
    }
    Connection db;
    if (!db.open(path, SQLITE_OPEN_READONLY)) {
        return DbStatus::failure("cannot open " + path + ": " + db.lastError());
    }
    db.exec("PRAGMA foreign_keys = ON");

    const DbStatus metaStatus = readMetaRow(db, meta);
    if (!metaStatus) {
        return metaStatus;
    }

    // Objects, in id order: the row order becomes the record order, and the
    // id -> index map below resolves the approaches' foreign keys.
    std::vector<Asteroid> objects;
    objects.reserve(meta.objectCount);
    std::vector<std::uint32_t> indexOfId;
    {
        Statement stmt;
        if (!stmt.prepare(db.get(), kSelectObjectsSql)) {
            return fail("cannot read the objects table", db);
        }
        while (stmt.step() == SQLITE_ROW) {
            const sqlite3_int64 id = stmt.columnInt(0);
            Asteroid a;
            a.pdes = stmt.columnText(1);
            a.spkid = stmt.columnText(2);
            a.name = stmt.columnText(3);
            a.fullName = stmt.columnText(4);
            a.classification.kind = static_cast<ObjectKind>(stmt.columnInt(5));
            a.classification.numbered = stmt.columnInt(6) != 0;
            a.classification.isNEO = stmt.optionalBool(7);
            a.classification.isPHA = stmt.optionalBool(8);
            a.classification.orbitClass = stmt.columnText(9);
            a.orbital.orbitId = stmt.columnText(10);
            a.orbital.epochJdTdb = stmt.columnDouble(11);
            a.orbital.eccentricity = stmt.columnDouble(12);
            a.orbital.semiMajorAxisAU = stmt.columnDouble(13);
            a.orbital.perihelionAU = stmt.columnDouble(14);
            a.orbital.inclinationDeg = stmt.columnDouble(15);
            a.orbital.ascendingNodeDeg = stmt.columnDouble(16);
            a.orbital.argPerihelionDeg = stmt.columnDouble(17);
            a.orbital.meanAnomalyDeg = stmt.columnDouble(18);
            a.orbital.meanMotionDegPerDay = stmt.columnDouble(19);
            a.orbital.periodDays = stmt.optionalDouble(20);
            a.orbital.moidAU = stmt.optionalDouble(21);
            a.orbital.conditionCode = stmt.optionalInt(22);
            a.physical.absoluteMagnitudeH = stmt.optionalDouble(23);
            a.physical.diameterKm = stmt.optionalDouble(24);
            a.physical.diameterSigmaKm = stmt.optionalDouble(25);
            a.physical.albedo = stmt.optionalDouble(26);
            a.physical.rotationPeriodHours = stmt.optionalDouble(27);
            // est_diameter_km is not read back: it is derived from H and albedo,
            // so recomputing it keeps one source of truth. It is stored for
            // anyone querying the file directly.

            if (id > 0) {
                const std::size_t slot = static_cast<std::size_t>(id);
                if (indexOfId.size() <= slot) {
                    indexOfId.resize(slot + 1, kInvalidRecord);
                }
                indexOfId[slot] = static_cast<std::uint32_t>(objects.size());
            }
            objects.push_back(std::move(a));
        }
    }
    dataset.setObjects(std::move(objects));

    std::vector<CloseApproach> approaches;
    approaches.reserve(meta.approachCount);
    {
        Statement stmt;
        if (!stmt.prepare(db.get(), kSelectApproachesSql)) {
            return fail("cannot read the approaches table", db);
        }
        while (stmt.step() == SQLITE_ROW) {
            const sqlite3_int64 objectId = stmt.columnInt(0);
            CloseApproach c;
            const std::size_t slot = static_cast<std::size_t>(objectId);
            c.objectIndex = objectId > 0 && slot < indexOfId.size() ? indexOfId[slot] : kNoObject;
            c.jdTdb = stmt.columnDouble(1);
            c.distanceAU = stmt.columnDouble(2);
            c.distanceMinAU = stmt.columnDouble(3);
            c.distanceMaxAU = stmt.columnDouble(4);
            c.distRangeDerived = stmt.columnInt(5) != 0;
            c.relVelocityKms = stmt.columnDouble(6);
            c.vInfinityKms = stmt.optionalDouble(7);
            c.absoluteMagnitudeH = stmt.optionalDouble(8);
            c.diameterKm = stmt.optionalDouble(9);
            c.diameterSigmaKm = stmt.optionalDouble(10);
            approaches.push_back(c);
        }
    }
    const std::size_t orphaned = dataset.setApproaches(std::move(approaches));

    if (dataset.objectCount() != meta.objectCount || dataset.approachCount() + orphaned != meta.approachCount) {
        char buf[224];
        std::snprintf(buf, sizeof buf,
                      "database is inconsistent with its own meta row: read %zu objects / %zu approaches, "
                      "meta says %zu / %zu. Re-run neo_ingest.",
                      dataset.objectCount(), dataset.approachCount(), meta.objectCount, meta.approachCount);
        return DbStatus::failure(buf);
    }
    if (orphaned > 0) {
        return DbStatus::failure(std::to_string(orphaned) +
                                 " approach row(s) reference no object; the database is corrupt, re-run neo_ingest");
    }
    return DbStatus::success();
}

std::size_t databaseFileSize(const std::string& path) {
    std::error_code ec;
    const std::uintmax_t size = std::filesystem::file_size(path, ec);
    if (ec) {
        return 0;
    }
    return static_cast<std::size_t>(size);
}

} // namespace neo
