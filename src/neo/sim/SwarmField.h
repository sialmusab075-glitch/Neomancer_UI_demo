#pragma once

#include "neo/model/Dataset.h"
#include "sim/OrbitalElements.h"

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

namespace neo {

// Positions of many asteroids at the simulation time, for the solar view's NEOS
// layer. Pure C++ (no GL, no ImGui); the renderer only uploads what this produces.
//
// Every object is propagated from ITS OWN elements by the existing two-body Kepler
// solver in src/sim (solveKepler), heliocentric ecliptic J2000, in AU:
//
//   * the elements are converted exactly as docs/NEO_PLAN.md section 3 describes:
//     SBDB gives them at each object's own epoch, sim::propagate wants them at
//     J2000, so M(J2000) = M(epoch) - n * (epoch - J2000) with the same n the
//     solver uses -- the position AT the object's epoch is then exactly the
//     published one, and nothing else changes;
//   * the kernel below is sim::propagate with the orbital-plane rotation folded
//     into two precomputed basis vectors; the tests hold it to sim::propagate's
//     own output (1e-12 AU).
//
// Two ways to serve a frame, chosen by the object count:
//
//   DIRECT   (size <= directLimit, default 2,000)   propagate every object, every frame
//   NODES    (above)   a worker thread computes positions AND velocities on a time grid
//            of `stepDays`; the frame interpolates between the two surrounding nodes
//            with a cubic Hermite spline, which is essentially exact for a step of a
//            day or two. The frame never waits: if a node is not ready it returns
//            false and the caller keeps the previous positions.
//
// The grid step follows the clock rate (nodes per second are capped), so playing time
// at 365 days/s coarsens the grid instead of falling behind.
struct SwarmElements {
    double a = 0.0;  // semi-major axis, AU
    double e = 0.0;  // eccentricity, 0 <= e < 1
    double b = 0.0;  // semi-minor axis, AU
    double n = 0.0;  // mean motion, rad/day (from a, as sim::propagate does)
    double m0 = 0.0; // mean anomaly at J2000.0, rad
    double p[3] = {0.0, 0.0, 0.0}; // orbital-plane x axis (towards perihelion) in the ecliptic frame
    double q[3] = {0.0, 0.0, 0.0}; // orbital-plane y axis
};

// SBDB elements (at the object's own epoch) as sim::OrbitalElements at J2000.
sim::OrbitalElements toSimElements(const Asteroid& asteroid);
SwarmElements makeSwarmElements(const sim::OrbitalElements& elements);
inline SwarmElements makeSwarmElements(const Asteroid& asteroid) { return makeSwarmElements(toSimElements(asteroid)); }
std::vector<SwarmElements> makeSwarmElements(const Dataset& dataset, const std::vector<std::uint32_t>& records);

// The kernel, in doubles. Position in AU; velocity (optional) in AU/day.
void propagateSwarm(const SwarmElements& s, double tDays, double pos[3], double vel[3] = nullptr);

class SwarmField {
public:
    static constexpr std::size_t kDefaultDirectLimit = 2000;
    static constexpr double kMaxNodesPerSecond = 30.0; // grid step coarsens to keep to this
    static constexpr double kMaxStepDays = 32.0;
    static constexpr int kSlots = 6;

    SwarmField();
    ~SwarmField();
    SwarmField(const SwarmField&) = delete;
    SwarmField& operator=(const SwarmField&) = delete;

    // Main thread. Replaces the objects and drops every computed node.
    void setObjects(std::vector<SwarmElements> objects);
    std::size_t size() const { return objects_.size(); }

    void setDirectLimit(std::size_t limit);
    bool direct() const { return objects_.size() <= directLimit_; }
    double stepDays() const { return step_; }

    // Main thread, once per frame. `tDays` is days since J2000.0 and `rateDaysPerSec`
    // the clock's signed rate (it picks the grid step and which node to prefetch).
    // On true, `out` holds 3 floats per object (x, y, z; ecliptic AU) for `tDays`;
    // on false it is untouched and still holds the previous frame's positions.
    bool positionsAt(double tDays, double rateDaysPerSec, std::vector<float>& out);

    struct Stats {
        std::uint64_t nodesComputed = 0;
        double lastNodeMs = 0.0;   // worker time for one node
        double lastDirectMs = 0.0; // main-thread time for one direct frame
    };
    Stats stats() const;

private:
    struct Slot {
        std::atomic<int> state{0}; // 0 free, 1 queued or running (worker owns the arrays), 2 ready (main reads)
        long long index = 0;       // main thread only
        std::uint64_t gen = 0;     // main thread only: the generation it was requested in
        std::vector<float> pos, vel;
    };
    struct Job {
        int slot = 0;
        long long index = 0;
        double step = 1.0;
        std::uint64_t gen = 0;
    };

    void workerMain();
    void invalidate();
    void chooseStep(double rate);
    Slot* find(long long index, int wantState);
    void request(long long index);
    bool interpolate(double t, long long k, std::vector<float>& out);

    std::vector<SwarmElements> objects_;
    std::size_t directLimit_ = kDefaultDirectLimit;

    std::thread worker_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::condition_variable idle_;
    std::deque<Job> jobs_;
    bool stop_ = false;
    bool running_ = false;

    std::array<Slot, kSlots> slots_;
    std::atomic<std::uint64_t> gen_{1};
    double step_ = 1.0;

    // What the last frame produced, so an unchanged clock costs nothing.
    const float* lastOut_ = nullptr;
    double lastT_ = 0.0;
    long long lastK_ = 0;
    std::uint64_t lastGen_ = 0;
    bool haveLast_ = false;

    std::atomic<std::uint64_t> nodes_{0};
    std::atomic<double> lastNodeMs_{0.0};
    double lastDirectMs_ = 0.0;
};

} // namespace neo
