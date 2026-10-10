#include "S1S2FrameParser.h"
#include "S1S2Decoder.h"
#include "XYEProtocol.h"
#include "SafetySupervisor.h"
#include "ByteHistory.h"
#include "ControllerLease.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>
#include <string>

std::vector<RawFrame> Results;
void Collect(void *, const RawFrame &F) { Results.push_back(F); }
std::vector<uint8_t> Hex(const char *Text)
{
    std::vector<uint8_t> B;
    while (*Text)
    {
        unsigned V;
        assert(sscanf(Text, "%2x", &V) == 1);
        B.push_back(V);
        Text += 2;
    }
    return B;
}
void Fix(std::vector<uint8_t> &B)
{
    auto C = S1S2FrameParser::Crc(B.data(), B.size() - 2);
    B[B.size() - 2] = C & 255;
    B.back() = C >> 8;
}
uint64_t TimeUs = 1000;
void Feed(S1S2FrameParser &P, const std::vector<uint8_t> &B)
{
    for (auto V : B) { P.Feed(V, TimeUs); TimeUs += 2084; }
}
unsigned ValidCount()
{
    unsigned N = 0;
    for (auto &F : Results) N += F.Result == FrameResult::Valid;
    return N;
}
int main(int Argc, char **Argv)
{
    ControllerLease Lease;
    assert(!Lease.Active(100));
    Lease.Renew(100);
    assert(Lease.Active(100) && Lease.Active(10000099));
    assert(!Lease.Active(10000100));
    Lease.Renew(20000000);
    assert(Lease.Active(20000000));
    Lease.Revoke();
    assert(!Lease.Active(20000001));
    uint8_t Set[16];
    assert(XYEProtocol::BuildSet(0, 0x88, 0x80, 79, Set));
    // Independent layout/checksum from user's old set_frame builder.
    auto ExpectedSet = Hex("AAC3003F803F88804F000000003CAC55");
    assert(memcmp(Set, ExpectedSet.data(), 16) == 0);
    assert(!XYEProtocol::BuildSet(0, 0x88, 0x80, 59, Set));
    assert(!XYEProtocol::BuildSet(0, 0x88, 6, 79, Set));
    assert(!XYEProtocol::BuildSet(64, 0x88, 0x80, 79, Set));
    S1S2FrameParser P(Collect, nullptr);
    auto Odu = Hex("A00001200C120F000077742604B5010001001D51");
    // README's IDU example has one extra trailing 00, preserved in samples.
    auto Idu = Hex("A00100200C11010F000000170F6C60190000E7B400");
    assert(S1S2FrameParser::Crc(reinterpret_cast<const uint8_t *>("123456789"), 9) == 0x4B37);
    Feed(P, Odu);
    Feed(P, Idu);
    assert(ValidCount() == 2);
    assert(Results[0].Length == 20 && Results[0].FirstUs < Results[0].LastUs);
    Telemetry T;
    S1S2Decoder::Decode(Results[0], T);
    assert(T.Values[static_cast<size_t>(Field::ActualHz)].Value == 15);
    S1S2Decoder::Decode(Results[1], T);
    assert(T.Values[static_cast<size_t>(Field::IndoorSetpointC)].Value == 23);
    assert(T.Values[static_cast<size_t>(Field::IndoorDemandHz)].Certainty == Confidence::Probable);
    auto Bad = Odu;
    Bad[9] ^= 1;
    Feed(P, Bad);
    Feed(P, Odu);
    assert(ValidCount() == 3);
    bool Failed = false;
    for (auto &F : Results) Failed |= F.Result == FrameResult::CrcFailure;
    assert(Failed);
    // Corrupt long length followed by valid frame; idle flush must recover.
    Feed(P, {0xA0, 0, 1, 0x20, 255, 0x12});
    Feed(P, Odu);
    P.Tick(TimeUs + 60000);
    assert(ValidCount() == 4);
    P.Reset();
    std::vector<uint8_t> Maximum(263, 0xA0);
    Maximum[4] = 255;
    Fix(Maximum);
    Feed(P, Maximum);
    assert(ValidCount() == 5);
    auto Unknown = Odu;
    Unknown[3] = 0xFE;
    Fix(Unknown);
    Feed(P, Unknown);
    assert(ValidCount() == 6);
    for (uint8_t Command = 0x50; Command <= 0x53; ++Command)
    {
        auto Extended = Odu;
        Extended[3] = Command;
        Fix(Extended);
        Feed(P, Extended);
        S1S2Decoder::Decode(Results.back(), T);
    }
    assert(T.Values[static_cast<size_t>(Field::TargetHz)].Valid);
    auto InvalidTemp = Odu;
    InvalidTemp[9] = 0;
    Fix(InvalidTemp);
    Feed(P, InvalidTemp);
    S1S2Decoder::Decode(Results.back(), T);
    assert(!T.Values[static_cast<size_t>(Field::OutdoorCoilC)].Valid);
    uint8_t Xye[32]{};
    Xye[0] = 0xAA; Xye[1] = 0xC0; Xye[2] = 0x80;
    Xye[4] = 7; Xye[10] = 23 | 0x40; Xye[31] = 0x55;
    Xye[30] = XYEProtocol::Checksum(Xye, 32);
    XYEProtocol::Status X;
    assert(XYEProtocol::DecodeStatus(Xye, 32, X) && X.Address == 7 && X.SetpointRaw == (23 | 0x40));
    Xye[22] ^= 1;
    assert(!XYEProtocol::DecodeStatus(Xye, 32, X));
    uint8_t Query[16];
    assert(XYEProtocol::BuildQuery(7, Query));
    const uint8_t Expected[] = {0xAA, 0xC0, 7, 0, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0x3F, 0x7A, 0x55};
    assert(memcmp(Query, Expected, 16) == 0);
    assert(!XYEProtocol::BuildQuery(64, Query));
    assert(XYEProtocol::BuildQuery(63, Query) && Query[2] == 63);
    assert(XYEProtocol::BuildQuery(0, Query, 63));
    const uint8_t WorkingQuery[] = {0xAA, 0xC0, 0, 0x3F, 0x80, 0x3F, 0, 0, 0, 0, 0, 0, 0, 0x3F, 3, 0x55};
    assert(memcmp(Query, WorkingQuery, 16) == 0);
    assert(!XYEProtocol::BuildQuery(0, Query, 64));
    Xye[22] ^= 1;
    Xye[3] = Xye[5] = 63;
    Xye[10] = 79;
    Xye[30] = XYEProtocol::Checksum(Xye, 32);
    assert(XYEProtocol::DecodeStatus(Xye, 32, X, 63) && X.SetpointRaw == 79);
    assert(!XYEProtocol::DecodeStatus(Xye, 32, X, 0));
    ByteHistory History;
    uint8_t Stream[300];
    for (unsigned I = 0; I < 300; ++I) Stream[I] = I & 255;
    History.Add(Stream, 300, 1234);
    assert(History.Count == 3 && History.At(0).Length == 128 && History.At(2).Length == 44);
    assert(History.At(2).Bytes[43] == Stream[299] && History.At(1).DrainUs == 1234);
    for (unsigned I = 0; I < 20; ++I) History.Add(Stream, 1, I);
    assert(History.Count == 16 && History.Evictions == 7);
    assert(History.At(0).Sequence == 8 && History.At(15).Sequence == 23);
    assert(!SafetySupervisor::TransmissionAllowed());
    assert(SafetySupervisor::Stale(100, 15000101));
    // Independent physical inputs must not share assembly state.
    S1S2FrameParser Second(Collect, nullptr);
    P.Reset();
    unsigned Before = ValidCount();
    for (size_t I = 0; I < Odu.size(); ++I)
    {
        P.Feed(Odu[I], TimeUs);
        Second.Feed(Odu[I], TimeUs + 1);
        TimeUs += 2084;
    }
    assert(ValidCount() == Before + 2);
    // Deterministic noisy stream exercises bounded storage and idle recovery.
    uint32_t Random = 42;
    for (unsigned I = 0; I < 100000; ++I)
    {
        Random = Random * 1664525u + 1013904223u;
        P.Feed(static_cast<uint8_t>(Random >> 24), TimeUs);
        TimeUs += 2084;
    }
    P.Tick(TimeUs + 60000);
    Before = ValidCount();
    Feed(P, Odu);
    assert(ValidCount() == Before + 1);
    if (Argc > 1)
    {
        Results.clear(); P.Reset();
        FILE *F = fopen(Argv[1], "rb"); assert(F);
        int B;
        while ((B = fgetc(F)) != EOF) { P.Feed(B, TimeUs); TimeUs += 2084; }
        fclose(F); P.Tick(TimeUs + 60000);
        for (auto &R : Results)
        {
            printf("%llu %llu %u ", static_cast<unsigned long long>(R.FirstUs), static_cast<unsigned long long>(R.LastUs), static_cast<unsigned>(R.Result));
            for (unsigned I = 0; I < R.Length; ++I) printf("%02X", R.Bytes[I]);
            puts("");
        }
    }
    puts("All parser/decoder/safety tests passed.");
}
