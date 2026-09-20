#include "neo/sim/SwarmSelection.h"

#include <algorithm>

namespace neo {

const char* toString(SwarmPreset preset) {
    switch (preset) {
    case SwarmPreset::PhaOnly: return "PHAS ONLY";
    case SwarmPreset::N1000: return "1,000";
    case SwarmPreset::N5000: return "5,000";
    case SwarmPreset::N20000: return "20,000";
    case SwarmPreset::All: return "ALL";
    case SwarmPreset::CurrentResult: return "FILTER RESULT";
    }
    return "?";
}

std::size_t presetCount(SwarmPreset preset) {
    switch (preset) {
    case SwarmPreset::N1000: return 1000;
    case SwarmPreset::N5000: return 5000;
    case SwarmPreset::N20000: return 20000;
    default: return 0;
    }
}

SwarmCatalog::SwarmCatalog(const Dataset& dataset) : dataset_(&dataset), objects_(dataset.objectCount()) {
    const std::vector<AsteroidRecord>& records = dataset.records();
    struct Ranked {
        std::uint32_t record;
        double diameterKm; // -1 when unknown, so unknowns sort last
    };
    std::vector<Ranked> ranked;
    ranked.reserve(records.size());
    for (std::size_t i = 0; i < records.size(); ++i) {
        const Asteroid& a = records[i].object;
        if (!a.orbital.propagationSupported()) {
            continue;
        }
        const std::optional<double> d = a.physical.bestDiameterKm();
        ranked.push_back({static_cast<std::uint32_t>(i), d ? *d : -1.0});
    }
    std::sort(ranked.begin(), ranked.end(), [](const Ranked& x, const Ranked& y) {
        if (x.diameterKm != y.diameterKm) {
            return x.diameterKm > y.diameterKm;
        }
        return x.record < y.record;
    });
    rank_.reserve(ranked.size());
    for (const Ranked& r : ranked) {
        rank_.push_back(r.record);
        const Flag pha = records[r.record].object.classification.isPHA;
        if (pha && *pha) {
            pha_.push_back(r.record);
        }
    }
}

std::vector<std::uint32_t> SwarmCatalog::select(SwarmPreset preset, const std::vector<std::uint32_t>* current) const {
    switch (preset) {
    case SwarmPreset::PhaOnly:
        return pha_;
    case SwarmPreset::All:
        return rank_;
    case SwarmPreset::CurrentResult: {
        std::vector<std::uint32_t> out;
        if (current != nullptr) {
            out.reserve(current->size());
            for (const std::uint32_t record : *current) {
                if (record < dataset_->records().size() &&
                    dataset_->records()[record].object.orbital.propagationSupported()) {
                    out.push_back(record);
                }
            }
        }
        return out;
    }
    default: {
        const std::size_t n = std::min(presetCount(preset), rank_.size());
        return std::vector<std::uint32_t>(rank_.begin(), rank_.begin() + static_cast<std::ptrdiff_t>(n));
    }
    }
}

} // namespace neo
