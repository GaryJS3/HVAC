#pragma once
#include <cstdint>
class ControllerLease
{
public:
    static constexpr uint64_t DurationUs = 10000000;
    void Renew(uint64_t Now) { ExpiresUs = Now + DurationUs; }
    void Revoke() { ExpiresUs = 0; }
    bool Active(uint64_t Now) const { return ExpiresUs != 0 && Now < ExpiresUs; }
    uint64_t Deadline() const { return ExpiresUs; }
private:
    uint64_t ExpiresUs = 0;
};
