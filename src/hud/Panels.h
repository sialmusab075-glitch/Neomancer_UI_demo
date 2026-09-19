#pragma once

#include "hud/HudState.h"

#include <cstddef>

namespace sim {
class SimClock;
class SolarSystem;
} // namespace sim

namespace hud {

// Window names (the part after ### is the stable ID used by the dock layout
// and imgui.ini; the visible part is the tab label when the tab bar is shown).
constexpr const char* kWinStatus    = "STATUS###sol_status";
constexpr const char* kWinControls  = "CONTROL###sol_controls";
constexpr const char* kWinTarget    = "TARGET###sol_target";
constexpr const char* kWinTelemetry = "STATE VECTOR###sol_telemetry";
constexpr const char* kWinHistory   = "HISTORY###sol_history";
constexpr const char* kWinLog       = "EVENT LOG###sol_log";

class EventLog;

// Full-width status strips above and below the dock area (only in themes with
// statusStrips). Call before the dockspace each frame; they reserve their
// height in the viewport's work area.
void drawTopStrip(const sim::SimClock& clock, const HudState& state, unsigned sessionId);
void drawBottomStrip(const HudState& state, const EventLog& log, const char* targetName);

// "YYYY-MM-DD HH:MM" for sim time t (days since J2000.0), without allocating.
void formatDate(char* buf, std::size_t size, double t_days);

// Top-left title bar: name, T+elapsed, calendar date, sim speed, FPS and the
// SIM / RENDER / SCALE status dots.
void drawTitleBar(const sim::SimClock& clock, const HudState& state);

// Play/pause, reverse, step, time-scale slider, reset, scale mode, target,
// layer toggles. Changes the clock directly and reports what happened.
HudEvents drawControls(sim::SimClock& clock, const sim::SolarSystem& system, HudState& state);

// Right panel: selected body's name and ID, hero distance, and readout bars
// (mass, radius, distance, speed, period, eccentricity, inclination) scaled to
// the range across the eight planets.
void drawPlanetPanel(const sim::SolarSystem& system, const HudState& state, double elapsedDays);

// 2x4 state-vector blocks (position, velocity, mean and true anomaly) and ring
// gauges (orbit completion, eccentricity, position between perihelion and aphelion).
void drawDataGrid(const sim::SolarSystem& system, const HudState& state);

// Four-hex-digit ID derived from a string or a number, for the "0x7F3A" tag style.
unsigned hexId(const char* text);
unsigned hexId(double value);

} // namespace hud
