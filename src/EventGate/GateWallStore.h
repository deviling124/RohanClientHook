#pragma once
#include "../../shared/EventGateProtocol.h"
#include <windows.h>

// The closed event gates the server has reported. Written by the packet decoder, read by the walk check.
namespace EventGate {
class WallStore {
public:
    void Apply(const EventGateWire::Message& message, unsigned long long now) {
        AcquireSRWLockExclusive(&lock);
        Wall* slot = nullptr;
        for (auto& wall : walls)
            if (wall.used && message.name == wall.name) slot = &wall;
        if (!message.closed) {
            if (slot) slot->used = false;
        } else {
            for (auto& wall : walls)
                if (!slot && (!wall.used || now >= wall.expires)) slot = &wall;
            if (slot) {
                slot->used = true;
                slot->name[message.name.copy(slot->name, EventGateWire::MaxName)] = '\0';
                slot->ax = message.ax; slot->az = message.az; slot->bx = message.bx; slot->bz = message.bz;
                slot->expires = now + EventGateWire::ExpiryMs;
            }
        }
        LONG used = 0;
        for (const auto& wall : walls) used += wall.used ? 1 : 0;
        InterlockedExchange(&count, used);
        ReleaseSRWLockExclusive(&lock);
    }
    // True when the world position is inside a wall the server still reports.
    bool Blocked(float x, float z, unsigned long long now) {
        if (!InterlockedCompareExchange(&count, 0, 0)) return false; // The usual case: no walls.
        bool blocked = false;
        AcquireSRWLockShared(&lock);
        for (const auto& wall : walls)
            blocked = blocked || (wall.used && now < wall.expires &&
                EventGateWire::SegmentDistance(x, z, wall.ax, wall.az, wall.bx, wall.bz) < EventGateWire::ClientBand);
        ReleaseSRWLockShared(&lock);
        return blocked;
    }
private:
    struct Wall {
        bool used = false;
        char name[EventGateWire::MaxName + 1]{};
        float ax = 0, az = 0, bx = 0, bz = 0;
        unsigned long long expires = 0;
    };
    Wall walls[EventGateWire::MaxGates];
    SRWLOCK lock = SRWLOCK_INIT;
    volatile LONG count = 0;
};
inline WallStore Walls;
}
