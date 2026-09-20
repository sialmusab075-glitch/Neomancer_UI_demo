#pragma once

#include "neo/model/Dataset.h"
#include "neo/query/QueryEngine.h"
#include "neo/sim/EarthFlybys.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace neo {

// Everything the UI needs from one finished query, built on the worker thread so
// the frame that picks it up only swaps a pointer's worth of data.
struct NeoOutcome {
    std::uint64_t serial = 0;
    bool ok = false;
    std::vector<std::string> errors;  // validation failures, or why the database is unavailable
    QueryResult  result;
    FlybyScene   scene;
    std::string  summary;             // the one-line EXPLAIN
    std::string  explain;             // the full EXPLAIN
    double       totalMs = 0.0;       // query + scene build, wall clock
};

// Owns the database, the query engine and one worker thread.
//
// The UI never parses JSON, never talks to SQLite and never touches the network:
// it asks this service for results, and the service only ever calls
// QueryEngine::run. That is the layering rule from the project plan, enforced by
// what this class exposes.
//
//   startLoad(path)   loads neo.db and builds the indexes on the worker thread
//   submit(query)     runs a query there; a newer submit REPLACES one still
//                     waiting (latest wins), so dragging a control cannot build a
//                     backlog
//   poll(out)         main thread, once per frame: true when a NEW result is ready
//
// dataset() and engine() are non-null only once state() == Ready and are
// immutable from then on, so the main thread may read them (the TARGET panel
// looks up an asteroid's fields there) while queries run.
class NeoService {
public:
    enum class State { Idle, Loading, Ready, Failed };

    NeoService();
    ~NeoService();
    NeoService(const NeoService&) = delete;
    NeoService& operator=(const NeoService&) = delete;

    void startLoad(const std::string& dbPath);

    State       state() const { return state_.load(std::memory_order_acquire); }
    // "42,666 objects / 42,819 approaches", or the reason loading failed.
    std::string message() const;
    std::size_t objectCount() const { return objects_.load(); }
    std::size_t approachCount() const { return approaches_.load(); }
    double      loadMs() const { return loadMs_.load(); }
    double      indexMs() const { return indexMs_.load(); }

    const Dataset*     dataset() const { return state() == State::Ready ? dataset_.get() : nullptr; }
    const QueryEngine* engine() const { return state() == State::Ready ? engine_.get() : nullptr; }

    // Returns the job's serial number. Safe from any thread.
    std::uint64_t submit(const Query& query, const EarthViewScale& scale);
    std::uint64_t latestSerial() const { return serial_.load(); }
    bool busy() const { return busy_.load(); }

    // True and fills `out` when a result newer than the last poll is ready.
    bool poll(NeoOutcome& out);

private:
    struct Job {
        Query query;
        EarthViewScale scale;
        std::uint64_t serial = 0;
    };

    void workerMain();
    void doLoad(const std::string& path);
    NeoOutcome execute(const Job& job);
    void setMessage(const std::string& text);

    std::thread             worker_;
    mutable std::mutex      mutex_;
    std::condition_variable wake_;
    bool                    stop_ = false;
    std::optional<std::string> loadRequest_;
    std::optional<Job>      pending_;
    std::optional<NeoOutcome> ready_;
    std::string             message_;

    std::unique_ptr<Dataset>     dataset_;
    std::unique_ptr<QueryEngine> engine_;

    std::atomic<State>         state_{State::Idle};
    std::atomic<std::uint64_t> serial_{0};
    std::atomic<bool>          busy_{false};
    std::atomic<std::size_t>   objects_{0};
    std::atomic<std::size_t>   approaches_{0};
    std::atomic<double>        loadMs_{0.0};
    std::atomic<double>        indexMs_{0.0};
};

} // namespace neo
