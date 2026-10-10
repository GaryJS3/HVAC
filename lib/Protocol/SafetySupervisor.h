#pragma once
#include "Records.h"

class SafetySupervisor
{
public:
    static constexpr bool TransmissionAllowed() { return false; }
    static bool Stale(uint64_t UpdatedUs, uint64_t NowUs)
    {
        return UpdatedUs == 0 || NowUs - UpdatedUs > 15000000;
    }
};
// Deliberately inert interfaces for later milestones. No frame modification or TX.
class S1S2Proxy
{
public:
    ProxyHealth Health;
};
class IndoorUnitController
{
public:
    CommandAcknowledgment QueryStatus() const { return {}; }
};
struct CapacityLimitPolicy
{
    bool Enabled = false;
    Measurement FactoryRequestedHz, ConfiguredMaximumHz;
};
