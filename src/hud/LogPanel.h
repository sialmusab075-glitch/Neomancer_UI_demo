#pragma once

#include <cstddef>
#include <deque>
#include <string>

namespace hud {

enum class LogKind {
    Orbital,   // perihelion / aphelion
    Alignment, // opposition / conjunction
    System,    // selection, time scale, pause, ...
};

// Rolling event log shown in the EVENT LOG panel. Keeps the newest kCapacity lines.
class EventLog {
public:
    static constexpr std::size_t kCapacity = 500;

    struct Entry {
        std::string stamp;  // "T+0042:06:13"
        std::string text;   // "EARTH PERIHELION r=0.9833 AU"
        LogKind kind;
        bool alarm;         // attention event (e.g. perihelion, conjunction)
        std::size_t split;  // index where the trailing number starts ("0.9833 AU"); == text.size() if none
    };

    // `alarm` marks lines that deserve attention; themes may flag them.
    void add(std::string stamp, std::string text, LogKind kind, bool alarm = false);
    void clear();
    const std::deque<Entry>& entries() const { return entries_; }
    std::size_t totalAdded() const { return totalAdded_; }

    // Draws the log body inside the current window. Auto-scrolls to new lines
    // unless the user has scrolled up. Returns true if CLEAR was pressed.
    bool draw();

private:
    std::deque<Entry> entries_;
    std::size_t totalAdded_ = 0;
    std::size_t lastDrawnTotal_ = 0;
};

} // namespace hud
