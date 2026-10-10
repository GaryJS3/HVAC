#include "S1S2Decoder.h"
#include "S1S2FrameParser.h"
#include <cmath>

const char *S1S2Decoder::Name(Field Id)
{
    static const char *Names[] = {"indoor_mode", "indoor_setpoint_c", "indoor_demand_hz", "indoor_fan_mode",
        "indoor_ambient_c", "indoor_coil_c", "actual_hz", "outdoor_coil_c", "outdoor_ambient_c",
        "discharge_c", "current_a", "outdoor_mode", "fan_rpm", "exv_steps", "dc_bus_v",
        "fan_target_rpm", "exv_target_steps", "session_minutes", "lifetime_hours",
        "pid_step", "fan_gear", "compressor_state", "power_w", "target_hz"};
    return Names[static_cast<size_t>(Id)];
}
void S1S2Decoder::Decode(const RawFrame &F, Telemetry &Output)
{
    if (F.Result != FrameResult::Valid || F.Length < 8 || F.Bytes[0] != 0xA0 ||
        F.Length != F.Bytes[4] + 8 || S1S2FrameParser::Crc(F.Bytes, F.Length - 2) !=
        (F.Bytes[F.Length - 2] | (F.Bytes[F.Length - 1] << 8))) return;
    const auto *B = F.Bytes;
    const unsigned Address = (B[1] << 8) | B[2];
    // Decode only the documented 12-byte payload layouts; other layouts remain raw.
    if (B[4] != 12) return;
    using C = Confidence;
    auto Set = [&](Field Id, double Value, C Certainty)
    {
        auto &M = Output.Values[static_cast<size_t>(Id)];
        M = {Value, F.LastUs, std::isfinite(Value), Certainty};
    };
    auto Ntc = [](double V)
    {
        return V > 0 && V < 255 ? 1 / (1 / 298.15 + std::log(0.81 * (255 - V) / V) / 4150) - 273.15 : NAN;
    };
    if (Address == 0x0100 && B[3] == 0x20)
    {
        Set(Field::IndoorMode, B[6], C::ReferenceConfirmed);
        Set(Field::IndoorSetpointC, B[11], C::ReferenceConfirmed);
        Set(Field::IndoorDemandHz, B[7], C::Probable);
        Set(Field::IndoorFanMode, B[12], C::Probable);
        Set(Field::IndoorAmbientC, Ntc(B[13]), C::ReferenceConfirmed);
        Set(Field::IndoorCoilC, Ntc(B[14]), C::Probable);
    }
    if (Address != 0x0001) return;
    switch (B[3])
    {
        case 0x20:
        {
            Set(Field::ActualHz, B[6], C::Probable);
            Set(Field::OutdoorCoilC, Ntc(B[9]), C::ReferenceConfirmed);
            Set(Field::OutdoorAmbientC, Ntc(B[10] + B[15] / 256.0), C::ReferenceConfirmed);
            double L = B[11] > 0 && B[11] < 255 ? std::log((255.0 - B[11]) / B[11]) : NAN;
            Set(Field::DischargeC, 1 / (2.873e-3 + 2.491e-4 * L + 9.74e-7 * L * L * L) - 273.15, C::Probable);
            Set(Field::CurrentA, B[12] / 1.875, C::Probable);
            Set(Field::OutdoorMode, B[14], C::ReferenceConfirmed);
            break;
        }
        case 0x50:
            Set(Field::FanRpm, B[11] * 8, C::ReferenceConfirmed);
            Set(Field::ExvSteps, B[12] * 2, C::Probable);
            Set(Field::DcBusV, B[14] * 2 - 29, C::Probable);
            break;
        case 0x51:
            Set(Field::FanTargetRpm, B[5] * 8, C::ReferenceConfirmed);
            Set(Field::ExvTargetSteps, B[6] * 2, C::Probable);
            Set(Field::SessionMinutes, B[11], C::ReferenceConfirmed);
            Set(Field::LifetimeHours, B[12] | (B[13] << 8), C::ReferenceConfirmed);
            break;
        case 0x52:
            Set(Field::PidStep, static_cast<int8_t>(B[9]), C::Probable);
            Set(Field::FanGear, B[13], C::Probable);
            break;
        case 0x53:
            Set(Field::CompressorState, B[9], C::ReferenceConfirmed);
            Set(Field::PowerW, B[11] | (B[12] << 8), C::Probable);
            Set(Field::TargetHz, B[13], C::ReferenceConfirmed);
            break;
    }
}
