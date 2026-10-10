#pragma once
#include <cstddef>
#include <cstdint>

constexpr size_t MaxFrameSize = 263; // 255 payload + header, trailer and CRC
enum class FrameResult : uint8_t { Valid, CrcFailure, Truncated };
struct RawFrame
{
    uint8_t Bytes[MaxFrameSize]{};
    uint16_t Length = 0;
    uint64_t FirstUs = 0, LastUs = 0;
    FrameResult Result = FrameResult::Truncated;
    uint8_t Port = 0;
};
struct CommunicationHealth
{
    uint64_t Bytes = 0, Frames = 0, CrcFailures = 0;
    uint64_t FramingErrors = 0, ParityErrors = 0, RxOverruns = 0;
    uint64_t BufferFull = 0, DroppedBytes = 0, DropEvents = 0;
    uint64_t QueueSaturations = 0, LastByteUs = 0, LastValidUs = 0;
};
enum class Confidence : uint8_t { ReferenceConfirmed, Probable, Unknown };
struct Measurement
{
    double Value = 0;
    uint64_t UpdatedUs = 0;
    bool Valid = false;
    Confidence Certainty = Confidence::Unknown;
};
enum class Field : uint8_t
{
    IndoorMode, IndoorSetpointC, IndoorDemandHz, IndoorFanMode,
    IndoorAmbientC, IndoorCoilC, ActualHz, OutdoorCoilC, OutdoorAmbientC,
    DischargeC, CurrentA, OutdoorMode, FanRpm, ExvSteps, DcBusV,
    FanTargetRpm, ExvTargetSteps, SessionMinutes, LifetimeHours,
    PidStep, FanGear, CompressorState, PowerW, TargetHz, Count
};
struct Telemetry
{
    Measurement Values[static_cast<size_t>(Field::Count)]{};
};
struct CommandAcknowledgment
{
    bool Accepted = false, ReadBackVerified = false;
    const char *Reason = "Transmission not implemented";
};
struct ProxyHealth
{
    bool Enabled = false;
    uint64_t Forwarded = 0, MaximumLatencyUs = 0;
};
