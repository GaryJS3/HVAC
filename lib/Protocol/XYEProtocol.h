#pragma once
#include <cstdint>
#include <cstddef>
// C0 queries and C3 writes, matching the user's previously working project.
class XYEProtocol
{
public:
    static bool BuildSet(uint8_t Address, uint8_t Mode, uint8_t Fan, uint8_t TempF, uint8_t (&Out)[16], uint8_t Master = 63)
    {
        if (!BuildQuery(Address, Out, Master) || TempF < 60 || TempF > 86 ||
            !(Mode == 0 || Mode == 0x81 || Mode == 0x82 || Mode == 0x84 || Mode == 0x88) ||
            !(Fan == 0x80 || Fan == 1 || Fan == 2 || Fan == 4)) return false;
        Out[1] = 0xC3; Out[6] = Mode; Out[7] = Fan; Out[8] = TempF;
        Out[13] = 0x3C; Out[14] = Checksum(Out, 16);
        return true;
    }
    static bool BuildQuery(uint8_t Address, uint8_t (&Out)[16], uint8_t Master = 0)
    {
        if (Address > 0x3F || Master > 0x3F) return false;
        for (auto &B : Out) B = 0;
        Out[0] = 0xAA;
        Out[1] = 0xC0;
        Out[2] = Address;
        Out[3] = Master;
        Out[4] = 0x80;
        Out[5] = Master;
        Out[13] = 0x3F;
        Out[15] = 0x55;
        Out[14] = Checksum(Out, 16);
        return true;
    }
    struct Status
    {
        uint8_t Address = 0, Mode = 0, Fan = 0, SetpointRaw = 0;
        uint16_t ErrorFlags = 0, ProtectionFlags = 0;
    };
    static uint8_t Checksum(const uint8_t *Bytes, size_t Length)
    {
        unsigned Sum = 0;
        for (size_t I = 0; I < Length; ++I)
            if (I != Length - 2) Sum += Bytes[I];
        return static_cast<uint8_t>(0xFF - (Sum & 0xFF));
    }
    static bool DecodeStatus(const uint8_t *B, size_t Length, Status &Out, uint8_t Master = 0)
    {
        if (Length != 32 || B[0] != 0xAA || B[31] != 0x55 || B[1] != 0xC0 ||
            Master > 0x3F || (B[2] & 0x7F) != 0 || B[3] != Master || B[5] != Master ||
            B[4] > 0x3F || B[30] != Checksum(B, Length)) return false;
        Out = {B[4], B[8], B[9], B[10],
            static_cast<uint16_t>(B[22] | (B[23] << 8)),
            static_cast<uint16_t>(B[24] | (B[25] << 8))};
        return true;
    }
};
