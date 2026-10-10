#pragma once
#include "Records.h"
class S1S2Decoder
{
public:
    static void Decode(const RawFrame &Frame, Telemetry &Output);
    static const char *Name(Field Id);
};
