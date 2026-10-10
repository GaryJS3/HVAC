#pragma once
#include <cstddef>
#include <cstdint>
#include "Records.h"

// Transport assembly is independent of parser resynchronization: every byte,
// including noise and bad CRCs, is emitted exactly once in original order.
class PassthroughBuffer
{
public:
    using Sink = void (*)(void *, const uint8_t *, size_t, uint64_t, uint64_t);
    PassthroughBuffer(Sink Callback, void *Context) : Callback(Callback), Context(Context) {}
    size_t Pending() const { return Used; }
    void Reset() { Used = 0; }
    void Feed(uint8_t Byte, uint64_t Now)
    {
        Tick(Now);
        if (Used && Buffer[0] != 0xA0 && Byte == 0xA0) Flush();
        if (!Used) First = Now;
        Buffer[Used++] = Byte;
        Last = Now;
        if (Used == sizeof(Buffer) || (Buffer[0] == 0xA0 && Used >= 5 && Used == size_t(Buffer[4]) + 8)) Flush();
    }
    void Tick(uint64_t Now)
    {
        // More than 3.5 characters at 4800/8N1. Also drains damaged lengths.
        if (Used && Now - Last >= 8000) Flush();
    }
private:
    void Flush()
    {
        Callback(Context, Buffer, Used, First, Last);
        Used = 0;
    }
    Sink Callback;
    void *Context;
    uint8_t Buffer[MaxFrameSize]{};
    size_t Used = 0;
    uint64_t First = 0, Last = 0;
};
