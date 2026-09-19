#include "sim/SolarSystem.h"

#include "sim/BodyTable.h"
#include "sim/Constants.h"
#include "sim/KeplerSolver.h"

#include <stdexcept>

namespace sim {

SolarSystem::SolarSystem(const std::vector<int>& tableIndices) {
    bodies_.reserve(tableIndices.size());
    for (int row : tableIndices) {
        if (row < 0 || row >= bodyTableSize()) {
            throw std::out_of_range("SolarSystem: body table index out of range");
        }
        const BodyData& d = bodyData(row);

        Body b;
        b.tableIndex = row;
        if (d.parent >= 0) {
            b.parentIndex = indexOfTableRow(d.parent);
            if (b.parentIndex < 0) {
                throw std::invalid_argument("SolarSystem: a body's parent must be listed before it");
            }
            // Two-body approximation: mu of the parent alone (mu = G * M_parent).
            // For the Sun the published GM is used directly.
            b.mu_m3s2 = (d.parent == kSun) ? kMuSun_m3s2 : kG * bodyData(d.parent).mass_kg;
            b.meanMotion_radPerDay = meanMotionRadPerDay(d.elements.a_AU, b.mu_m3s2);
        }
        bodies_.push_back(b);
    }
    update(0.0);
}

SolarSystem SolarSystem::createFull() {
    std::vector<int> rows;
    for (int i = 0; i < kBodyCount; ++i) {
        rows.push_back(i);
    }
    return SolarSystem(rows);
}

void SolarSystem::update(double t_days) {
    t_days_ = t_days;
    for (Body& b : bodies_) {
        if (b.parentIndex < 0) {
            b.orbit = OrbitState{};
            b.helioPos_AU = Vec3d{};
            b.helioVel_kms = Vec3d{};
            continue;
        }
        b.orbit = propagate(b.data().elements, b.mu_m3s2, t_days);
        // Parents always precede children, so the parent is already updated.
        const Body& parent = bodies_[static_cast<std::size_t>(b.parentIndex)];
        b.helioPos_AU  = parent.helioPos_AU + b.orbit.pos_AU;
        b.helioVel_kms = parent.helioVel_kms + b.orbit.vel_kms;
    }
}

int SolarSystem::indexOfTableRow(int tableIndex) const {
    for (std::size_t i = 0; i < bodies_.size(); ++i) {
        if (bodies_[i].tableIndex == tableIndex) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

} // namespace sim
