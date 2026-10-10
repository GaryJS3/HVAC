#pragma once
#include "Records.h"

class S1S2FrameParser
{
public:
    using Sink = void (*)(void *, const RawFrame &);
    S1S2FrameParser(Sink Callback, void *Context);
    void Feed(uint8_t Byte, uint64_t NowUs);
    void Tick(uint64_t NowUs);
    void Reset();
    static uint16_t Crc(const uint8_t *Bytes, size_t Length);
    uint64_t NoiseBytes = 0;
private:
    void Process();
    void Remove(size_t Count);
    Sink Callback;
    void *Context;
    uint8_t Buffer[MaxFrameSize]{};
    uint64_t Times[MaxFrameSize]{};
    size_t Used = 0;
};
