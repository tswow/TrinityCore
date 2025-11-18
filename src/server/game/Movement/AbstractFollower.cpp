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

#include "AbstractFollower.h"
#include "Unit.h"

void AbstractFollower::SetTarget(Unit* unit)
{
    if (unit == _target)
        return;

    if (_target)
        _target->FollowerRemoved(this);
    _target = unit;
    if (_target)
        _target->FollowerAdded(this);

    // Reset velocity tracking when target changes
    _lastTargetPosition.reset();
    _lastUpdateTime.reset();
    _targetVelocity = G3D::Vector3::zero();
}

void AbstractFollower::UpdateTargetVelocity(uint32 diff)
{
    if (!_target)
    {
        _targetVelocity = G3D::Vector3::zero();
        return;
    }

    Position currentPos = _target->GetPosition();

    // First update - just store position, can't calculate velocity yet
    if (!_lastTargetPosition)
    {
        _lastTargetPosition = currentPos;
        _lastUpdateTime = diff;
        _targetVelocity = G3D::Vector3::zero();
        return;
    }

    // Calculate displacement
    float dx = currentPos.GetPositionX() - _lastTargetPosition->GetPositionX();
    float dy = currentPos.GetPositionY() - _lastTargetPosition->GetPositionY();
    float dz = currentPos.GetPositionZ() - _lastTargetPosition->GetPositionZ();

    // Calculate time elapsed in seconds
    float timeElapsed = diff / 1000.0f;
    if (timeElapsed < 0.001f)
        timeElapsed = 0.001f; // Avoid division by zero

    // Calculate instantaneous velocity (yards per second)
    float newVelX = dx / timeElapsed;
    float newVelY = dy / timeElapsed;
    float newVelZ = dz / timeElapsed;

    // Smooth velocity using exponential moving average (reduces jitter)
    // Alpha = 0.2 means 20% new value, 80% old value
    // This creates smooth velocity changes instead of sudden snaps
    float alpha = 0.2f;
    _targetVelocity.x = alpha * newVelX + (1.0f - alpha) * _targetVelocity.x;
    _targetVelocity.y = alpha * newVelY + (1.0f - alpha) * _targetVelocity.y;
    _targetVelocity.z = alpha * newVelZ + (1.0f - alpha) * _targetVelocity.z;

    // Update stored position
    _lastTargetPosition = currentPos;
    _lastUpdateTime = diff;
}
