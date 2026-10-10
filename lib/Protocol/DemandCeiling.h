#pragma once
#include "S1S2FrameParser.h"

// Experimental cooling-demand cap, not a compressor frequency guarantee.
// Owned by RelayService under its policy lock; transport stays independent.
class DemandCeiling
{
public:
    bool Enabled = false;
    uint8_t MaximumHz = 20, RequestedHz = 0, ForwardedHz = 0;
    uint32_t CommandId = 0, ModifiedFrames = 0;
    uint64_t LastDemandUs = 0;
    const char *Reason = "disabled";
    uint8_t LastOriginal[20]{}, LastSent[20]{};
    bool HaveFrame = false;

    static bool ValidMaximum(unsigned Hz) { return Hz >= 20 && Hz <= 120; }
    bool Configure(bool Enable, unsigned Hz, uint32_t Id, uint64_t Now)
    {
        if (Enable && (!ValidMaximum(Hz) || Now >= LeaseUntil)) return false;
        Enabled = Enable;
        if (Enable) MaximumHz = Hz;
        CommandId = Id; Reason = Enable ? "waiting_for_cooling" : "disabled";
        return true;
    }
    void RenewLease(uint64_t Now) { LeaseUntil = Now + 10000000u; }
    void Tick(uint64_t Now, bool Healthy)
    {
        // Drop authority, not traffic. Recovery requires explicit re-enabling.
        if (Enabled && (Now >= LeaseUntil || !Healthy))
        { Enabled = false; Reason = !Healthy ? "transport_fault" : "controller_lost"; }
    }
    bool Apply(unsigned Source, uint8_t *B, size_t Length, uint64_t Now, bool Healthy)
    {
        Tick(Now, Healthy);
        if (!Core(B, Length)) return false;
        if (Source == 0 && B[1] == 0 && B[2] == 1)
        { OutdoorMode = B[14]; OutdoorUpdatedUs = Now; return false; }
        if (Source != 1 || B[1] != 1 || B[2] != 0 || B[5] != 0x11) return false;
        if (!Enabled) return false;
        if (B[6] != 1) { Reason = "not_cooling"; return false; }
        if (!OutdoorUpdatedUs || Now - OutdoorUpdatedUs > 15000000u || OutdoorMode != 1)
        { Reason = "outdoor_not_cooling_or_stale"; return false; }
        if (B[8] != 0 || B[7] > 120)
        { Reason = "unsupported_request_flags_or_range"; return false; }
        Reason = B[7] > MaximumHz ? "limiting" : "below_ceiling";
        if (B[7] <= MaximumHz) return false;
        B[7] = MaximumHz;
        const uint16_t Crc = S1S2FrameParser::Crc(B, Length - 2);
        B[Length - 2] = Crc & 255; B[Length - 1] = Crc >> 8;
        return true;
    }
    void Sent(unsigned Source, const uint8_t *Original, const uint8_t *Output, size_t Length, uint64_t Now)
    {
        if (Source != 1 || !Core(Original, Length) || Original[1] != 1 || Original[2] != 0 || Original[5] != 0x11) return;
        RequestedHz = Original[7]; ForwardedHz = Output[7]; LastDemandUs = Now;
        for (unsigned I = 0; I < 20; ++I) { LastOriginal[I] = Original[I]; LastSent[I] = Output[I]; }
        HaveFrame = true;
        if (RequestedHz != ForwardedHz) ++ModifiedFrames;
    }
private:
    uint64_t LeaseUntil = 0, OutdoorUpdatedUs = 0;
    uint8_t OutdoorMode = 0;
    static bool Core(const uint8_t *B, size_t Length)
    {
        return Length == 20 && B[0] == 0xA0 && B[3] == 0x20 && B[4] == 12 &&
            S1S2FrameParser::Crc(B, Length - 2) == uint16_t(B[18] | (B[19] << 8));
    }
};
