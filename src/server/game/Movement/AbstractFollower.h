/*
 * This file is part of the TrinityCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef TRINITY_ABSTRACTFOLLOWER_H
#define TRINITY_ABSTRACTFOLLOWER_H

#include "Optional.h"
#include "Position.h"
#include <G3D/Vector3.h>

class Unit;

struct AbstractFollower
{
    public:
        AbstractFollower(Unit* target = nullptr) { SetTarget(target); }
        ~AbstractFollower() { SetTarget(nullptr); }

        void SetTarget(Unit* unit);
        Unit* GetTarget() const { return _target; }

        // Velocity tracking for predictive movement
        void UpdateTargetVelocity(uint32 diff);
        G3D::Vector3 const& GetTargetVelocity() const { return _targetVelocity; }
        bool HasVelocityData() const { return _lastTargetPosition.has_value(); }

    private:
        Unit* _target = nullptr;

        // Predictive movement data
        Optional<Position> _lastTargetPosition;
        Optional<uint32> _lastUpdateTime;
        G3D::Vector3 _targetVelocity = G3D::Vector3::zero();
};

#endif
