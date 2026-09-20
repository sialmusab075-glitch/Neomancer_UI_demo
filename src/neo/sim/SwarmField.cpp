#include "neo/sim/SwarmField.h"

#include "neo/model/JulianDate.h"
#include "sim/Constants.h"
#include "sim/KeplerSolver.h"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace neo {

sim::OrbitalElements toSimElements(const Asteroid& asteroid) {
    const OrbitalProperties& o = asteroid.orbital;
    sim::OrbitalElements el;
    el.a_AU = o.semiMajorAxisAU;
    el.e = o.eccentricity;
    el.i_deg = o.inclinationDeg;
    el.Omega_deg = o.ascendingNodeDeg;
    el.varpi_deg = o.ascendingNodeDeg + o.argPerihelionDeg;
    // Back-propagate the mean anomaly from the object's own epoch to J2000 with the
    // mean motion the solver will use, so the position at the epoch is the published one.
    const double nDegPerDay = sim::meanMotionRadPerDay(el.a_AU, sim::kMuSun_m3s2) / sim::kDegToRad;
    const double mJ2000 = o.meanAnomalyDeg - nDegPerDay * (o.epochJdTdb - kJ2000Jd);
    el.L_deg = el.varpi_deg + mJ2000;
    return el;
}

SwarmElements makeSwarmElements(const sim::OrbitalElements& el) {
    SwarmElements s;
    s.a = el.a_AU;
    s.e = el.e;
    s.b = el.a_AU * std::sqrt(1.0 - el.e * el.e);
    s.n = sim::meanMotionRadPerDay(el.a_AU, sim::kMuSun_m3s2);
    s.m0 = el.meanAnomalyAtEpoch_deg() * sim::kDegToRad;
    const double omega = el.argPerihelion_deg() * sim::kDegToRad;
    const double node = el.Omega_deg * sim::kDegToRad;
    const double inc = el.i_deg * sim::kDegToRad;
    const sim::Vec3d p = sim::orbitalPlaneToEcliptic(1.0, 0.0, omega, node, inc);
    const sim::Vec3d q = sim::orbitalPlaneToEcliptic(0.0, 1.0, omega, node, inc);
    s.p[0] = p.x; s.p[1] = p.y; s.p[2] = p.z;
    s.q[0] = q.x; s.q[1] = q.y; s.q[2] = q.z;
    return s;
}

std::vector<SwarmElements> makeSwarmElements(const Dataset& dataset, const std::vector<std::uint32_t>& records) {
    std::vector<SwarmElements> out;
    out.reserve(records.size());
    for (const std::uint32_t r : records) {
        out.push_back(makeSwarmElements(dataset.records()[r].object));
    }
    return out;
}

void propagateSwarm(const SwarmElements& s, double tDays, double pos[3], double vel[3]) {
    // sim::propagate, step for step: mean anomaly, Kepler's equation, then the plane.
    const double m = sim::wrapPi(s.m0 + s.n * tDays);
    const double eccentric = sim::solveKepler(m, s.e).E;
    const double cosE = std::cos(eccentric);
    const double sinE = std::sin(eccentric);
    const double xp = s.a * (cosE - s.e);
    const double yp = s.b * sinE;
    pos[0] = xp * s.p[0] + yp * s.q[0];
    pos[1] = xp * s.p[1] + yp * s.q[1];
    pos[2] = xp * s.p[2] + yp * s.q[2];
    if (vel != nullptr) {
        const double eDot = s.n / (1.0 - s.e * cosE); // rad/day
        const double vx = -s.a * sinE * eDot;
        const double vy = s.b * cosE * eDot;
        vel[0] = vx * s.p[0] + vy * s.q[0];
        vel[1] = vx * s.p[1] + vy * s.q[1];
        vel[2] = vx * s.p[2] + vy * s.q[2];
    }
}

// ---------------------------------------------------------------------------
// SwarmField
// ---------------------------------------------------------------------------

SwarmField::SwarmField() { worker_ = std::thread([this] { workerMain(); }); }

SwarmField::~SwarmField() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
        jobs_.clear();
    }
    wake_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
}

void SwarmField::workerMain() {
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait(lock, [this] { return stop_ || !jobs_.empty(); });
            if (stop_) {
                return;
            }
            job = jobs_.front();
            jobs_.pop_front();
            running_ = true;
        }
        Slot& slot = slots_[static_cast<std::size_t>(job.slot)];
        if (job.gen == gen_.load(std::memory_order_acquire)) {
            const auto start = std::chrono::steady_clock::now();
            const double t = static_cast<double>(job.index) * job.step;
            const std::size_t n = objects_.size();
            float* pos = slot.pos.data();
            float* vel = slot.vel.data();
            for (std::size_t i = 0; i < n; ++i) {
                double p[3], v[3];
                propagateSwarm(objects_[i], t, p, v);
                pos[3 * i] = static_cast<float>(p[0]);
                pos[3 * i + 1] = static_cast<float>(p[1]);
                pos[3 * i + 2] = static_cast<float>(p[2]);
                vel[3 * i] = static_cast<float>(v[0]);
                vel[3 * i + 1] = static_cast<float>(v[1]);
                vel[3 * i + 2] = static_cast<float>(v[2]);
            }
            const std::chrono::duration<double, std::milli> ms = std::chrono::steady_clock::now() - start;
            lastNodeMs_.store(ms.count(), std::memory_order_relaxed);
            nodes_.fetch_add(1, std::memory_order_relaxed);
            slot.state.store(2, std::memory_order_release);
        } else {
            slot.state.store(0, std::memory_order_release); // obsolete: the objects or the grid changed
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            running_ = false;
        }
        idle_.notify_all();
    }
}

void SwarmField::setObjects(std::vector<SwarmElements> objects) {
    gen_.fetch_add(1, std::memory_order_acq_rel);
    {
        std::unique_lock<std::mutex> lock(mutex_);
        for (const Job& j : jobs_) {
            slots_[static_cast<std::size_t>(j.slot)].state.store(0, std::memory_order_release);
        }
        jobs_.clear();
        idle_.wait(lock, [this] { return !running_; }); // the worker is done with the old objects
        objects_ = std::move(objects);
    }
    for (Slot& s : slots_) {
        s.state.store(0, std::memory_order_release);
        s.pos.assign(objects_.size() * 3, 0.0f);
        s.vel.assign(objects_.size() * 3, 0.0f);
    }
    haveLast_ = false;
}

void SwarmField::setDirectLimit(std::size_t limit) {
    directLimit_ = limit;
    invalidate();
}

// Forget every node (the grid changed): ready ones are freed, queued ones free themselves.
void SwarmField::invalidate() {
    gen_.fetch_add(1, std::memory_order_acq_rel);
    for (Slot& s : slots_) {
        int expected = 2;
        s.state.compare_exchange_strong(expected, 0, std::memory_order_acq_rel);
    }
    haveLast_ = false;
}

void SwarmField::chooseStep(double rate) {
    const double need = std::fabs(rate) / kMaxNodesPerSecond;
    double step = step_;
    while (need > step && step < kMaxStepDays) {
        step *= 2.0;
    }
    if (rate != 0.0) { // a pause keeps the grid: nothing to catch up on, and no reason to recompute
        while (need * 4.0 < step && step > 1.0) {
            step *= 0.5;
        }
    }
    if (step != step_) {
        step_ = step;
        invalidate();
    }
}

SwarmField::Slot* SwarmField::find(long long index, int wantState) {
    const std::uint64_t gen = gen_.load(std::memory_order_acquire);
    for (Slot& s : slots_) {
        const int state = s.state.load(std::memory_order_acquire);
        if (s.gen == gen && s.index == index && (wantState == 0 ? state != 0 : state == wantState)) {
            return &s;
        }
    }
    return nullptr;
}

void SwarmField::request(long long index) {
    // A free slot, or a ready one that is no longer wanted by anybody (the caller
    // requests at most three indices at a time and there are six slots).
    Slot* target = nullptr;
    for (Slot& s : slots_) {
        if (s.state.load(std::memory_order_acquire) == 0) {
            target = &s;
            break;
        }
    }
    if (target == nullptr) {
        for (Slot& s : slots_) {
            if (s.state.load(std::memory_order_acquire) == 2 && (s.index < index - 2 || s.index > index + 2)) {
                target = &s;
                break;
            }
        }
    }
    if (target == nullptr) {
        return; // try again next frame
    }
    const int slotIndex = static_cast<int>(target - slots_.data());
    target->index = index;
    target->gen = gen_.load(std::memory_order_acquire);
    target->state.store(1, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        jobs_.push_back({slotIndex, index, step_, target->gen});
    }
    wake_.notify_one();
}

bool SwarmField::interpolate(double t, long long k, std::vector<float>& out) {
    Slot* a = find(k, 2);
    Slot* b = find(k + 1, 2);
    if (a == nullptr || b == nullptr) {
        return false;
    }
    const double s = step_;
    const double u = (t - static_cast<double>(k) * s) / s;
    const double u2 = u * u, u3 = u2 * u;
    const double h00 = 2.0 * u3 - 3.0 * u2 + 1.0;
    const double h10 = (u3 - 2.0 * u2 + u) * s;
    const double h01 = -2.0 * u3 + 3.0 * u2;
    const double h11 = (u3 - u2) * s;
    const std::size_t count = objects_.size() * 3;
    out.resize(count);
    const float* pa = a->pos.data();
    const float* va = a->vel.data();
    const float* pb = b->pos.data();
    const float* vb = b->vel.data();
    float* dst = out.data();
    for (std::size_t i = 0; i < count; ++i) {
        dst[i] = static_cast<float>(h00 * pa[i] + h10 * va[i] + h01 * pb[i] + h11 * vb[i]);
    }
    return true;
}

bool SwarmField::positionsAt(double tDays, double rateDaysPerSec, std::vector<float>& out) {
    const std::size_t n = objects_.size();
    if (n == 0) {
        out.clear();
        return true;
    }
    if (direct()) {
        const auto start = std::chrono::steady_clock::now();
        out.resize(n * 3);
        for (std::size_t i = 0; i < n; ++i) {
            double p[3];
            propagateSwarm(objects_[i], tDays, p);
            out[3 * i] = static_cast<float>(p[0]);
            out[3 * i + 1] = static_cast<float>(p[1]);
            out[3 * i + 2] = static_cast<float>(p[2]);
        }
        const std::chrono::duration<double, std::milli> ms = std::chrono::steady_clock::now() - start;
        lastDirectMs_ = ms.count();
        return true;
    }

    chooseStep(rateDaysPerSec);
    const long long k = static_cast<long long>(std::floor(tDays / step_));
    const std::uint64_t gen = gen_.load(std::memory_order_acquire);

    // An unchanged clock (paused) costs nothing.
    if (haveLast_ && lastOut_ == out.data() && out.size() == n * 3 && tDays == lastT_ && k == lastK_ && gen == lastGen_) {
        return true;
    }

    long long wanted[3];
    int wantedCount = 0;
    wanted[wantedCount++] = k;
    wanted[wantedCount++] = k + 1;
    if (rateDaysPerSec > 0.0) {
        wanted[wantedCount++] = k + 2; // prefetch in the direction of travel
    } else if (rateDaysPerSec < 0.0) {
        wanted[wantedCount++] = k - 1;
    }
    for (int i = 0; i < wantedCount; ++i) {
        if (find(wanted[i], 0) == nullptr) {
            request(wanted[i]);
        }
    }
    if (!interpolate(tDays, k, out)) {
        return false;
    }
    lastOut_ = out.data();
    lastT_ = tDays;
    lastK_ = k;
    lastGen_ = gen;
    haveLast_ = true;
    return true;
}

SwarmField::Stats SwarmField::stats() const {
    Stats s;
    s.nodesComputed = nodes_.load(std::memory_order_relaxed);
    s.lastNodeMs = lastNodeMs_.load(std::memory_order_relaxed);
    s.lastDirectMs = lastDirectMs_;
    return s;
}

} // namespace neo
