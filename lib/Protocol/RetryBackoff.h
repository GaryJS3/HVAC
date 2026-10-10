#pragma once
#include <stdint.h>

// Bounded polling recovery; only a validated reply clears the failure streak.
class RetryBackoff
{
public:
    uint32_t Fail()
    {
        if (Failures < 6) ++Failures;
        return Failures >= 6 ? 30000u : (1000u << (Failures - 1));
    }
    void Recover() { Failures = 0; }
    uint8_t Count() const { return Failures; }
private:
    uint8_t Failures = 0;
};

class TransportFaultWindow
{
public:
    void Fail(uint64_t Now) { Until = Now + 10000000u; }
    bool Active(uint64_t Now) const { return Now < Until; }
private:
    uint64_t Until = 0;
};
