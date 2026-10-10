#include "S1S2FrameParser.h"
#include <cstring>

S1S2FrameParser::S1S2FrameParser(Sink Callback, void *Context)
    : Callback(Callback), Context(Context)
{
}
uint16_t S1S2FrameParser::Crc(const uint8_t *Bytes, size_t Length)
{
    uint16_t Value = 0xFFFF;
    for (size_t I = 0; I < Length; ++I)
    {
        Value ^= Bytes[I];
        for (unsigned Bit = 0; Bit < 8; ++Bit)
        {
            Value = (Value >> 1) ^ ((Value & 1) ? 0xA001 : 0);
        }
    }
    return Value;
}
void S1S2FrameParser::Remove(size_t Count)
{
    Used -= Count;
    memmove(Buffer, Buffer + Count, Used);
    memmove(Times, Times + Count, Used * sizeof(Times[0]));
}
void S1S2FrameParser::Reset()
{
    Used = 0;
}
void S1S2FrameParser::Feed(uint8_t Byte, uint64_t NowUs)
{
    Tick(NowUs);
    Buffer[Used] = Byte;
    Times[Used++] = NowUs;
    Process();
}
void S1S2FrameParser::Process()
{
    while (Used)
    {
        if (Buffer[0] != 0xA0)
        {
            ++NoiseBytes;
            Remove(1);
            continue;
        }
        if (Used < 5) return;
        const size_t Length = static_cast<size_t>(Buffer[4]) + 8;
        if (Used < Length) return;
        RawFrame Frame;
        Frame.Length = Length;
        memcpy(Frame.Bytes, Buffer, Length);
        Frame.FirstUs = Times[0];
        Frame.LastUs = Times[Length - 1];
        const uint16_t Expected = Buffer[Length - 2] | (Buffer[Length - 1] << 8);
        Frame.Result = Crc(Buffer, Length - 2) == Expected ? FrameResult::Valid : FrameResult::CrcFailure;
        Callback(Context, Frame);
        Remove(Frame.Result == FrameResult::Valid ? Length : 1);
    }
}
void S1S2FrameParser::Tick(uint64_t NowUs)
{
    // 50 ms > 20 character times at 4800 baud. Flush truncated/noisy candidates.
    while (Used && NowUs - Times[Used - 1] > 50000)
    {
        // A damaged length may conceal a complete subsequent frame. Slide and
        // process every suffix before finally reporting the incomplete tail.
        RawFrame Frame;
        Frame.Length = Used;
        memcpy(Frame.Bytes, Buffer, Used);
        Frame.FirstUs = Times[0];
        Frame.LastUs = Times[Used - 1];
        Callback(Context, Frame);
        Remove(1);
        Process();
    }
}
