#pragma once
#include <Arduino.h>
#include "Records.h"
#include "S1S2Decoder.h"
#include "ByteHistory.h"

constexpr size_t HistorySize = 32;
struct DiagnosticSnapshot
{
    CommunicationHealth Health[2];
    Telemetry Latest[2];
    RawFrame Frames[HistorySize];
    size_t Count = 0;
    uint64_t HistoryEvictions = 0;
};
class TelemetryStore
{
public:
    portMUX_TYPE Lock = portMUX_INITIALIZER_UNLOCKED;
    CommunicationHealth Health[2];
    void RecordBytes(unsigned Port, const uint8_t *Data, size_t Length, uint64_t Now)
    {
        portENTER_CRITICAL(&Lock);
        Bytes[Port].Add(Data, Length, Now);
        portEXIT_CRITICAL(&Lock);
    }
    void SnapshotBytes(unsigned Port, ByteHistory &Out)
    {
        portENTER_CRITICAL(&Lock);
        Out = Bytes[Port];
        portEXIT_CRITICAL(&Lock);
    }
    void Record(unsigned Port, const RawFrame &Input)
    {
        // Only the RX task writes telemetry, decode outside the critical section.
        S1S2Decoder::Decode(Input, Latest[Port]);
        portENTER_CRITICAL(&Lock);
        RawFrame F = Input;
        F.Port = Port;
        Frames[Next] = F;
        Next = (Next + 1) % HistorySize;
        if (Count < HistorySize) ++Count; else ++Evictions;
        if (F.Result == FrameResult::Valid)
        {
            ++Health[Port].Frames;
            Health[Port].LastValidUs = F.LastUs;
        }
        else if (F.Result == FrameResult::CrcFailure) ++Health[Port].CrcFailures;
        else ++Health[Port].FramingErrors;
        portEXIT_CRITICAL(&Lock);
    }
    void Snapshot(DiagnosticSnapshot &Out)
    {
        // Published is a coherent copy made by the sole RX writer.
        portENTER_CRITICAL(&Lock);
        memcpy(Out.Health, Health, sizeof(Health));
        memcpy(Out.Latest, Published, sizeof(Published));
        Out.Count = Count;
        Out.HistoryEvictions = Evictions;
        for (size_t I = 0; I < Count; ++I)
            Out.Frames[I] = Frames[(Next + HistorySize - Count + I) % HistorySize];
        portEXIT_CRITICAL(&Lock);
    }
    void Publish()
    {
        portENTER_CRITICAL(&Lock);
        memcpy(Published, Latest, sizeof(Latest));
        portEXIT_CRITICAL(&Lock);
    }
private:
    ByteHistory Bytes[2];
    Telemetry Latest[2], Published[2];
    RawFrame Frames[HistorySize];
    size_t Next = 0, Count = 0;
    uint64_t Evictions = 0;
};
