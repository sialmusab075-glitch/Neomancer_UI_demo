#include "neo/query/NeoService.h"

#include "neo/storage/Database.h"

#include <chrono>
#include <cstdio>
#include <utility>

namespace neo {

namespace {

using Clock = std::chrono::steady_clock;

double msSince(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

std::string thousands(std::size_t n) {
    std::string digits = std::to_string(n);
    std::string out;
    for (std::size_t i = 0; i < digits.size(); ++i) {
        out += digits[i];
        const std::size_t remaining = digits.size() - 1 - i;
        if (remaining > 0 && remaining % 3 == 0) {
            out += ',';
        }
    }
    return out;
}

} // namespace

// The thread starts in the body, after every member (mutex, condition variable,
// flags) exists. Starting it from the member-initialiser list would run it while
// members declared after `worker_` were still unconstructed.
NeoService::NeoService() { worker_ = std::thread([this] { workerMain(); }); }

NeoService::~NeoService() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
    }
    wake_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
}

void NeoService::startLoad(const std::string& dbPath) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        loadRequest_ = dbPath;
        message_ = "loading " + dbPath;
    }
    state_.store(State::Loading, std::memory_order_release);
    wake_.notify_all();
}

std::string NeoService::message() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return message_;
}

void NeoService::setMessage(const std::string& text) {
    std::lock_guard<std::mutex> lock(mutex_);
    message_ = text;
}

std::uint64_t NeoService::submit(const Query& query, const EarthViewScale& scale) {
    Job job;
    job.query = query;
    job.scale = scale;
    job.serial = ++serial_;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_ = std::move(job); // replaces a job still waiting: latest wins
    }
    wake_.notify_all();
    return serial_.load();
}

bool NeoService::poll(NeoOutcome& out) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!ready_) {
        return false;
    }
    out = std::move(*ready_);
    ready_.reset();
    return true;
}

void NeoService::doLoad(const std::string& path) {
    const Clock::time_point start = Clock::now();
    auto dataset = std::make_unique<Dataset>();
    DatabaseMeta meta;
    const DbStatus status = loadDatabase(path, *dataset, meta);
    if (!status) {
        setMessage(status.error);
        state_.store(State::Failed, std::memory_order_release);
        return;
    }
    loadMs_.store(msSince(start));

    const Clock::time_point indexStart = Clock::now();
    auto engine = std::make_unique<QueryEngine>(*dataset);
    engine->build();
    indexMs_.store(msSince(indexStart));

    objects_.store(dataset->objectCount());
    approaches_.store(dataset->approachCount());
    {
        std::lock_guard<std::mutex> lock(mutex_);
        dataset_ = std::move(dataset);
        engine_ = std::move(engine);
        message_ = thousands(dataset_->objectCount()) + " objects / " + thousands(dataset_->approachCount()) +
                   " approaches";
    }
    // Published last: a reader that sees Ready also sees the pointers above.
    state_.store(State::Ready, std::memory_order_release);
}

NeoOutcome NeoService::execute(const Job& job) {
    const Clock::time_point start = Clock::now();
    NeoOutcome out;
    out.serial = job.serial;

    if (state() != State::Ready) {
        out.ok = false;
        out.errors.push_back("the NEO database is not available: " + message());
        return out;
    }
    out.result = engine_->run(job.query, ExecMode::Planned);
    out.ok = out.result.ok;
    out.errors = out.result.errors;
    if (out.ok) {
        out.scene = buildFlybyScene(*dataset_, out.result, job.scale);
        out.summary = out.result.stats.summaryLine();
        out.explain = out.result.stats.explain();
    }
    out.totalMs = msSince(start);
    return out;
}

void NeoService::workerMain() {
    for (;;) {
        std::optional<std::string> load;
        std::optional<Job> job;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait(lock, [this] {
                const State s = state_.load(std::memory_order_acquire);
                const bool canQuery = s == State::Ready || s == State::Failed;
                return stop_ || loadRequest_.has_value() || (pending_.has_value() && canQuery);
            });
            if (stop_) {
                return;
            }
            if (loadRequest_) {
                load = std::move(loadRequest_);
                loadRequest_.reset();
            } else {
                job = std::move(pending_);
                pending_.reset();
            }
        }
        if (load) {
            doLoad(*load);
            continue;
        }
        busy_.store(true);
        NeoOutcome outcome = execute(*job);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ready_ = std::move(outcome);
        }
        busy_.store(false);
    }
}

} // namespace neo
