// Simulation self-tests. No framework, no window: run `sim_tests` (or `ctest`).
// Exit code 0 = all passed.

#include "sim/BodyTable.h"
#include "sim/Constants.h"
#include "sim/EventDetector.h"
#include "sim/KeplerSolver.h"
#include "sim/OrbitProbe.h"
#include "sim/SimClock.h"
#include "sim/SolarSystem.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool ok, const char* what, const std::string& detail = std::string()) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("  FAIL  %s %s\n", what, detail.c_str());
    }
}

std::string fmt(const char* f, double a, double b = 0.0) {
    char buf[160];
    std::snprintf(buf, sizeof buf, f, a, b);
    return buf;
}

void testKeplerSolver() {
    std::printf("[kepler] Newton solver residual and convergence\n");
    const double eccentricities[] = {0.0, 0.0167, 0.2056, 0.5, 0.8, 0.9, 0.97};
    for (double e : eccentricities) {
        double worst = 0.0;
        bool allConverged = true;
        for (int k = -360; k <= 360; ++k) {
            const double M = k * sim::kDegToRad;
            const sim::KeplerResult r = sim::solveKepler(M, e);
            const double resid = std::fabs(r.E - e * std::sin(r.E) - M);
            worst = std::fmax(worst, resid);
            allConverged = allConverged && r.converged && r.iterations <= sim::kKeplerMaxIterations;
        }
        check(worst < 1e-9, "Kepler residual", fmt("e=%.4f worst=%.3e", e, worst));
        check(allConverged, "Kepler converged within 15 iterations", fmt("e=%.4f", e));
    }
}

void testEarthPeriod() {
    std::printf("[earth] analytic period, periodicity, vis-viva\n");
    const sim::BodyData& earth = sim::bodyData(sim::kEarth);
    const double P = sim::orbitalPeriodDays(earth.elements.a_AU, sim::kMuSun_m3s2);
    std::printf("        Kepler period from a and mu: %.4f d\n", P);
    check(std::fabs(P - 365.25) < 0.02, "Earth period ~365.25 d", fmt("P=%.5f", P));

    const double times[] = {0.0, 123.4, -4000.0, 9000.0};
    for (double t : times) {
        const sim::OrbitState a = sim::propagate(earth.elements, sim::kMuSun_m3s2, t);
        const sim::OrbitState b = sim::propagate(earth.elements, sim::kMuSun_m3s2, t + P);
        const double d = sim::length(a.pos_AU - b.pos_AU);
        check(d < 1e-9, "position repeats after one period", fmt("t=%.1f diff=%.3e AU", t, d));

        const double vModel = sim::length(a.vel_kms);
        const double vVis = sim::visVivaSpeedKms(a.r_AU, earth.elements.a_AU, sim::kMuSun_m3s2);
        check(std::fabs(vModel - vVis) / vVis < 1e-9, "velocity magnitude == vis-viva",
              fmt("model=%.9f vis=%.9f", vModel, vVis));
        check(std::fabs(sim::length(a.pos_AU) - a.r_AU) < 1e-12, "|pos| == r");
    }

    // Position at J2000 vs JPL Horizons Earth-Moon barycentre (-0.1771, 0.9672, 0) AU.
    const sim::OrbitState s0 = sim::propagate(earth.elements, sim::kMuSun_m3s2, 0.0);
    check(std::fabs(s0.pos_AU.x - (-0.1771)) < 0.005 && std::fabs(s0.pos_AU.y - 0.9672) < 0.005 &&
              std::fabs(s0.pos_AU.z) < 1e-4,
          "Earth J2000 position matches ephemeris", fmt("x=%.5f y=%.5f", s0.pos_AU.x, s0.pos_AU.y));

    // Perihelion/aphelion distances reached over a year.
    double rmin = 1e9, rmax = 0.0;
    for (int d = 0; d < 3660; ++d) {
        const sim::OrbitState s = sim::propagate(earth.elements, sim::kMuSun_m3s2, d * 0.1);
        rmin = std::fmin(rmin, s.r_AU);
        rmax = std::fmax(rmax, s.r_AU);
    }
    check(std::fabs(rmin - earth.elements.perihelion_AU()) < 1e-6, "Earth perihelion distance",
          fmt("rmin=%.6f", rmin));
    check(std::fabs(rmax - earth.elements.aphelion_AU()) < 1e-6, "Earth aphelion distance",
          fmt("rmax=%.6f", rmax));
}

void testAllPlanetPeriods() {
    std::printf("[planets] Kepler period vs published sidereal period\n");
    for (int i = sim::kMercury; i < sim::kBodyCount; ++i) {
        const sim::BodyData& d = sim::bodyData(i);
        const double P = sim::orbitalPeriodDays(d.elements.a_AU, sim::kMuSun_m3s2);
        const double rel = std::fabs(P - d.siderealPeriod_days) / d.siderealPeriod_days;
        // Two-body with mu_sun only: Jupiter's own mass shifts its period ~0.05%.
        check(rel < 2e-3, d.name, fmt("P=%.2f rel=%.2e", P, rel));
    }
}

// Acceptance test for Milestone 1: drive the same loop the app runs (60 Hz real
// frames, SimClock at a HUD-selectable scale, SolarSystem update) and measure
// the time for Earth to complete one revolution geometrically.
void testEarthOrbitThroughAppLoop(double scale) {
    std::printf("[loop] Earth revolution via SimClock at %.1f d/s\n", scale);
    sim::SolarSystem system({sim::kSun, sim::kEarth});
    sim::SimClock clock;
    clock.setScale(scale);
    sim::OrbitProbe probe;
    const int earth = system.indexOfTableRow(sim::kEarth);

    const double frameDt = 1.0 / 60.0;
    const double maxSimDays = 3.0 * 365.25;
    while (std::fabs(clock.elapsedDays()) < maxSimDays && !probe.hasPeriod()) {
        clock.update(frameDt);
        system.update(clock.timeDays());
        probe.observe(clock.timeDays(), system.body(earth).orbit.pos_AU);
    }
    check(probe.hasPeriod(), "probe saw two crossings");
    std::printf("        measured revolution: %.4f sim-days\n", probe.lastPeriodDays());
    check(std::fabs(probe.lastPeriodDays() - 365.25) < 0.05, "Earth orbit ~365.25 sim-days",
          fmt("measured=%.5f", probe.lastPeriodDays()));
}

void testClockAndFormatting() {
    std::printf("[clock] controls, slider mapping, formatting\n");
    sim::SimClock c;
    c.setScale(1000.0);
    check(c.scale() == 365.0, "scale clamps to +365");
    c.setScale(-1000.0);
    check(c.scale() == -365.0, "scale clamps to -365");

    c.setScale(10.0);
    c.update(0.05);
    check(std::fabs(c.timeDays() - 0.5) < 1e-12, "10 d/s for 0.05 s = 0.5 d");
    c.reverse();
    c.update(0.05);
    check(std::fabs(c.timeDays()) < 1e-12, "reverse runs time backwards");
    c.update(10.0);
    check(std::fabs(c.timeDays() - (-1.0)) < 1e-12, "long real frames are truncated to 0.1 s");
    check(!c.step(1.0), "step ignored while running");
    c.setPaused(true);
    c.update(1.0);
    check(std::fabs(c.timeDays() - (-1.0)) < 1e-12, "paused clock does not advance");
    check(c.step(1.0) && std::fabs(c.timeDays()) < 1e-12, "step +1 day while paused");
    c.resetToEpoch();
    check(c.timeDays() == 0.0, "reset to epoch");

    check(sim::SimClock::sliderToScale(0.0) == 0.0, "slider 0 -> 0 d/s");
    check(std::fabs(sim::SimClock::sliderToScale(1.0) - 365.0) < 1e-9, "slider 1 -> 365 d/s");
    check(std::fabs(sim::SimClock::sliderToScale(-1.0) + 365.0) < 1e-9, "slider -1 -> -365 d/s");
    const double probes[] = {-200.0, -1.0, 0.25, 10.0, 364.0};
    for (double s : probes) {
        const double back = sim::SimClock::sliderToScale(sim::SimClock::scaleToSlider(s));
        check(std::fabs(back - s) < 1e-9, "slider round trip", fmt("%.3f -> %.9f", s, back));
    }

    check(sim::formatElapsed(42.0 + (6.0 * 60.0 + 13.0) / 1440.0) == "T+0042:06:13", "T+ format",
          sim::formatElapsed(42.0 + (6.0 * 60.0 + 13.0) / 1440.0));
    check(sim::formatElapsed(-1.5) == "T-0001:12:00", "T- format", sim::formatElapsed(-1.5));
    check(sim::formatElapsed(12345.0) == "T+12345:00:00", "T+ five-digit days");

    check(sim::formatCalendar(0.0) == "2000-01-01 12:00", "J2000 calendar", sim::formatCalendar(0.0));
    check(sim::formatCalendar(8765.5) == "2024-01-01 00:00", "2024-01-01", sim::formatCalendar(8765.5));
    check(sim::formatCalendar(-0.5 - 365.0) == "1999-01-01 00:00", "1999-01-01",
          sim::formatCalendar(-0.5 - 365.0));
    check(sim::formatCalendar(59.5) == "2000-03-01 00:00", "leap year 2000",
          sim::formatCalendar(59.5));
}

// Days since J2000.0 for a calendar date at 00:00 (Meeus ch. 7, Gregorian).
double daysSinceJ2000(int y, int m, double d) {
    if (m <= 2) {
        y -= 1;
        m += 12;
    }
    const int A = y / 100;
    const int B = 2 - A + A / 4;
    const double jd = std::floor(365.25 * (y + 4716)) + std::floor(30.6001 * (m + 1)) + d + B - 1524.5;
    return jd - sim::kJ2000_JD;
}

std::vector<sim::SimEvent> runEvents(double t0, double t1, double step) {
    sim::SolarSystem system = sim::SolarSystem::createFull();
    sim::EventDetector det;
    std::vector<sim::SimEvent> events;
    const int n = static_cast<int>(std::ceil(std::fabs(t1 - t0) / step));
    for (int k = 0; k <= n; ++k) {
        const double t = t0 + (t1 - t0) * static_cast<double>(k) / n;
        system.update(t);
        det.update(system, events);
    }
    return events;
}

// Nearest event of a type for a body to a reference time; returns the time offset (days).
double nearestOffset(const std::vector<sim::SimEvent>& ev, sim::EventType type, int tableRow, double tRef,
                     double* value = nullptr) {
    double best = 1e9;
    for (const sim::SimEvent& e : ev) {
        // createFull() keeps table order, so body index == table row.
        if (e.type == type && e.body == tableRow && std::fabs(e.t_days - tRef) < std::fabs(best)) {
            best = e.t_days - tRef;
            if (value) *value = e.value_AU;
        }
    }
    return best;
}

void testEventDetector() {
    std::printf("[events] perihelion/aphelion/opposition/conjunction vs real dates\n");
    const double t0 = daysSinceJ2000(1999, 12, 1.0);
    const double t1 = daysSinceJ2000(2006, 1, 1.0);

    // 6 days per sample = the app at 365 d/s and 60 fps.
    const std::vector<sim::SimEvent> ev = runEvents(t0, t1, 6.0);

    // Earth perihelion (UT dates). Elements are for the Earth-Moon barycentre,
    // whose perihelion differs from Earth's by up to ~1-2 days, hence the tolerance.
    struct DateCheck { int y, m; double d; };
    const DateCheck peri[] = {{2000, 1, 3.22}, {2001, 1, 4.37}, {2002, 1, 2.58}, {2003, 1, 4.21}, {2004, 1, 4.73}};
    for (const DateCheck& c : peri) {
        double r = 0.0;
        const double off = nearestOffset(ev, sim::EventType::Perihelion, sim::kEarth, daysSinceJ2000(c.y, c.m, c.d), &r);
        check(std::fabs(off) < 2.5, "Earth perihelion date", fmt("%d: off by %.2f d", c.y, off));
        check(std::fabs(r - 0.98329) < 2e-4, "Earth perihelion r", fmt("r=%.5f", r));
    }
    double ra = 0.0;
    const double offA = nearestOffset(ev, sim::EventType::Aphelion, sim::kEarth, daysSinceJ2000(2001, 7, 4.5), &ra);
    check(std::fabs(offA) < 3.0 && std::fabs(ra - 1.01671) < 2e-4, "Earth aphelion 2001-07-04",
          fmt("off %.2f d, r=%.5f", offA, ra));

    // Oppositions and conjunctions (published dates).
    const DateCheck marsOpp[] = {{2001, 6, 13.0}, {2003, 8, 28.0}, {2005, 11, 7.0}};
    for (const DateCheck& c : marsOpp) {
        double d = 0.0;
        const double off = nearestOffset(ev, sim::EventType::Opposition, sim::kMars, daysSinceJ2000(c.y, c.m, c.d), &d);
        check(std::fabs(off) < 3.0, "Mars opposition date", fmt("%d: off by %.2f d", c.y, off));
        (void)d;
    }
    double d2003 = 0.0;
    nearestOffset(ev, sim::EventType::Opposition, sim::kMars, daysSinceJ2000(2003, 8, 28.0), &d2003);
    check(std::fabs(d2003 - 0.373) < 0.01, "Mars 2003 opposition distance ~0.373 AU", fmt("d=%.4f", d2003));
    const double offC = nearestOffset(ev, sim::EventType::Conjunction, sim::kMars, daysSinceJ2000(2002, 8, 10.0));
    check(std::fabs(offC) < 4.0, "Mars conjunction 2002-08-10", fmt("off by %.2f d", offC));
    const DateCheck jupOpp[] = {{2000, 11, 28.0}, {2002, 1, 1.0}, {2003, 2, 2.0}, {2004, 3, 4.0}, {2005, 4, 3.0}};
    for (const DateCheck& c : jupOpp) {
        const double off = nearestOffset(ev, sim::EventType::Opposition, sim::kJupiter, daysSinceJ2000(c.y, c.m, c.d));
        check(std::fabs(off) < 3.0, "Jupiter opposition date", fmt("%d: off by %.2f d", c.y, off));
    }

    // No opposition/conjunction events for inner planets or Earth.
    bool innerClean = true;
    for (const sim::SimEvent& e : ev) {
        if ((e.type == sim::EventType::Opposition || e.type == sim::EventType::Conjunction) &&
            (e.body == sim::kMercury || e.body == sim::kVenus || e.body == sim::kEarth)) {
            innerClean = false;
        }
    }
    check(innerClean, "no opposition/conjunction for inner planets");

    // Same events regardless of step size and direction (times refined by bisection).
    const std::vector<sim::SimEvent> fine = runEvents(t0, t1, 0.5);
    const std::vector<sim::SimEvent> back = runEvents(t1, t0, 6.0);
    check(fine.size() == ev.size(), "same event count at 0.5 d and 6 d steps",
          fmt("%.0f vs %.0f", static_cast<double>(fine.size()), static_cast<double>(ev.size())));
    check(back.size() == ev.size(), "same event count when running backwards",
          fmt("%.0f vs %.0f", static_cast<double>(back.size()), static_cast<double>(ev.size())));
    double worst = 0.0;
    for (const sim::SimEvent& e : ev) {
        const double o1 = nearestOffset(fine, e.type, e.body, e.t_days);
        const double o2 = nearestOffset(back, e.type, e.body, e.t_days);
        worst = std::fmax(worst, std::fmax(std::fabs(o1), std::fabs(o2)));
    }
    check(worst < 1e-6, "event times independent of step and direction", fmt("worst %.2e d", worst));
    check(!back.empty() && back.front().t_days > back.back().t_days, "reverse run reports events newest-first");
}

} // namespace

int main() {
    testKeplerSolver();
    testEarthPeriod();
    testAllPlanetPeriods();
    testEarthOrbitThroughAppLoop(10.0);
    testEarthOrbitThroughAppLoop(365.0);
    testEarthOrbitThroughAppLoop(-120.0);
    testClockAndFormatting();
    testEventDetector();

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
