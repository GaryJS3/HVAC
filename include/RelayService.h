#pragma once
#include <Arduino.h>
#include <driver/uart.h>
#include <esp_timer.h>
#include <atomic>
#include "HardwareConfig.h"
#include "PassthroughBuffer.h"
#ifndef HVAC_PROXY_BENCH
#define HVAC_PROXY_BENCH 0
#endif

class RelayService
{
public:
    static constexpr bool Compiled = HVAC_PROXY_BENCH != 0;
    std::atomic<bool> Armed{false}, ProbeBusy{false};
    std::atomic<uint32_t> ForwardedBytes{0}, Errors{0}, DroppedBytes{0};
    std::atomic<uint32_t> Packets[2]{}, DirectionBytes[2]{};
    std::atomic<uint32_t> MaxQueueUs{0}, MaxBufferUs{0}, MaxTxUs{0}, QueuePeak{0};
    std::atomic<uint32_t> ProbesPassed{0};
    std::atomic<uint32_t> ProbeId{0}, ProbeState{0}; // 0 none, 1 running, 2 passed, 3 failed
    std::atomic<uint32_t> CommandId{0}, CommandState{0};
    RelayService() : Buffers{PassthroughBuffer(Emit, &Contexts[0]), PassthroughBuffer(Emit, &Contexts[1])}
    {
        Contexts[0] = {this, 0}; Contexts[1] = {this, 1};
    }
    bool Begin()
    {
        if (!Compiled) return true;
        Queue = xQueueCreate(16, sizeof(Packet));
        Ready = Queue && xTaskCreatePinnedToCore(Run, "S1S2Relay", 4096, this, 4, nullptr, 1) == pdPASS;
        return Ready;
    }
    bool Arm(bool Enable, uint32_t Id = 0)
    {
        if (!Compiled || !Ready || (Enable && (ProbeBusy.load() || Errors.load() || ProbesPassed.load() != 3))) return false;
        CommandId.store(Id); CommandState.store(1);
        RequestDeadline.store(uint32_t(esp_timer_get_time()) + 2000000);
        if (Enable && Armed.load()) { CommandState.store(2); return true; }
        Request.store(Enable ? 1 : 2);
        return true;
    }
    bool Probe(unsigned Port, uint32_t Id)
    {
        if (!Compiled || !Ready || Port > 1 || Armed.load() || Request.load() || ProbeBusy.exchange(true)) return false;
        Packet P{}; P.Source = Port; P.IsProbe = true; P.Length = 24;
        for (unsigned I = 0; I < P.Length; ++I) P.Bytes[I] = uint8_t((Id >> ((I % 4) * 8)) ^ (I * 29));
        ProbeId.store(Id); ProbeState.store(1);
        if (xQueueSend(Queue, &P, 0) == pdTRUE) return true;
        ProbeState.store(3); ProbeBusy.store(false); return false;
    }
    void Submit(unsigned Source, const uint8_t *Bytes, size_t Length, uint64_t Now)
    {
        if (!Compiled || Source > 1) return;
        LastRx[Source].store(uint32_t(Now));
        portENTER_CRITICAL(&ProbeLock);
        if (ProbeListening && Source == ProbeTarget)
        {
            for (size_t I = 0; I < Length; ++I)
            {
                if (ProbeUsed < ProbeLength && Bytes[I] == ProbeBytes[ProbeUsed]) ++ProbeUsed;
                else { ProbeMismatch = true; }
            }
        }
        portEXIT_CRITICAL(&ProbeLock);
        if (Armed.load()) for (size_t I = 0; I < Length; ++I) Buffers[Source].Feed(Bytes[I], Now);
    }
    void Poll()
    {
        if (!Compiled) return;
        const uint64_t Now = esp_timer_get_time();
        const unsigned Wanted = Request.load();
        if (Wanted == 2)
        {
            Armed.store(false); ++Epoch;
            for (auto &B : Buffers) { DroppedBytes += B.Pending(); B.Reset(); }
            Request.store(0); CommandState.store(2);
        }
        if (Armed.load()) for (auto &B : Buffers) B.Tick(Now);
        if (Wanted == 1 && int32_t(uint32_t(Now) - RequestDeadline.load()) >= 0)
        { Request.store(0); CommandState.store(3); return; }
        if (Wanted == 1 && !ProbeBusy.load() && uint32_t(Now) - LastRx[0].load() >= 100000 && uint32_t(Now) - LastRx[1].load() >= 100000)
        {
            // Start only after both inputs have been quiet, never mid-frame.
            ++Epoch; Armed.store(true); Request.store(0); CommandState.store(2);
        }
    }
    String Json(bool Connected) const
    {
        return String("{\"compiled\":") + (Compiled ? "true" : "false") + ",\"armed\":" + (Armed.load() ? "true" : "false") +
            ",\"arm_pending\":" + (Request.load() == 1 ? "true" : "false") + ",\"state\":\"" +
            (Errors.load() ? "transport_fault" : !Armed.load() ? "disarmed" : Connected ? "supervised_passthrough" : "failover_passthrough") +
            "\",\"forwarded_bytes\":" + ForwardedBytes.load() + ",\"errors\":" + Errors.load() + ",\"dropped_bytes\":" + DroppedBytes.load() +
            ",\"odu_to_idu_bytes\":" + DirectionBytes[0].load() + ",\"idu_to_odu_bytes\":" + DirectionBytes[1].load() +
            ",\"odu_to_idu_packets\":" + Packets[0].load() + ",\"idu_to_odu_packets\":" + Packets[1].load() +
            ",\"max_queue_us\":" + MaxQueueUs.load() + ",\"max_buffer_us\":" + MaxBufferUs.load() + ",\"max_tx_us\":" + MaxTxUs.load() +
            ",\"queue_peak\":" + QueuePeak.load() + ",\"probe_id\":" + ProbeId.load() + ",\"probe_state\":" + ProbeState.load() + ",\"probes_passed\":" + ProbesPassed.load() + ",\"command_id\":" + CommandId.load() + ",\"command_state\":" + CommandState.load() + "}";
    }
private:
    struct Packet
    {
        uint8_t Source = 0; bool IsProbe = false; uint16_t Length = 0;
        uint32_t Generation = 0; uint64_t QueuedUs = 0;
        uint8_t Bytes[MaxFrameSize]{};
    };
    struct Context { RelayService *Self; unsigned Source; } Contexts[2];
    PassthroughBuffer Buffers[2];
    QueueHandle_t Queue = nullptr;
    bool Ready = false;
    std::atomic<unsigned> Request{0}, Epoch{0};
    std::atomic<uint32_t> LastRx[2]{}, RequestDeadline{0};
    portMUX_TYPE ProbeLock = portMUX_INITIALIZER_UNLOCKED;
    bool ProbeListening = false, ProbeMismatch = false;
    unsigned ProbeTarget = 0, ProbeUsed = 0, ProbeLength = 0;
    uint8_t ProbeBytes[MaxFrameSize]{};
    static void Maximum(std::atomic<uint32_t> &Value, uint32_t Candidate)
    {
        uint32_t Old = Value.load();
        while (Candidate > Old && !Value.compare_exchange_weak(Old, Candidate)) {}
    }
    static void Emit(void *Pointer, const uint8_t *Data, size_t Length, uint64_t First, uint64_t Last)
    {
        auto &C = *static_cast<Context *>(Pointer); auto &Self = *C.Self;
        Packet P; P.Source = C.Source; P.Length = Length; P.Generation = Self.Epoch.load(); P.QueuedUs = esp_timer_get_time();
        memcpy(P.Bytes, Data, Length);
        Maximum(Self.MaxBufferUs, uint32_t(P.QueuedUs - First));
        (void)Last;
        if (xQueueSend(Self.Queue, &P, 0) != pdTRUE) { ++Self.Errors; Self.DroppedBytes += Length; }
        else Maximum(Self.QueuePeak, uxQueueMessagesWaiting(Self.Queue));
    }
    static bool Transmit(unsigned Target, const Packet &P)
    {
        const uart_port_t Uart = Target == 0 ? UART_NUM_1 : UART_NUM_2;
        const int En = Target == 0 ? HardwareConfig::OduEn : HardwareConfig::IduEn;
        if (uart_wait_tx_done(Uart, pdMS_TO_TICKS(600)) != ESP_OK) return false;
        digitalWrite(En, HIGH); delayMicroseconds(10);
        const int N = uart_write_bytes(Uart, P.Bytes, P.Length);
        // Full 263-byte frames take ~548 ms at 4800/8N1.
        const bool Done = uart_wait_tx_done(Uart, pdMS_TO_TICKS((P.Length * 10000u + 4799) / 4800 + 30)) == ESP_OK;
        digitalWrite(En, LOW);
        return N == P.Length && Done;
    }
    void RunProbe(const Packet &P)
    {
        portENTER_CRITICAL(&ProbeLock);
        ProbeTarget = 1 - P.Source; ProbeUsed = 0; ProbeLength = P.Length; ProbeMismatch = false;
        memcpy(ProbeBytes, P.Bytes, P.Length); ProbeListening = true;
        portEXIT_CRITICAL(&ProbeLock);
        bool Ok = Transmit(P.Source, P);
        const uint64_t Deadline = esp_timer_get_time() + 1000000;
        bool Matched = false;
        do
        {
            portENTER_CRITICAL(&ProbeLock);
            Matched = ProbeUsed == ProbeLength && !ProbeMismatch;
            portEXIT_CRITICAL(&ProbeLock);
            if (Matched) break;
            vTaskDelay(1);
        } while (esp_timer_get_time() < Deadline);
        portENTER_CRITICAL(&ProbeLock); ProbeListening = false; portEXIT_CRITICAL(&ProbeLock);
        if (Ok && Matched) ProbesPassed.fetch_or(1u << P.Source);
        else ProbesPassed.fetch_and(~(1u << P.Source));
        ProbeState.store(Ok && Matched ? 2 : 3); ProbeBusy.store(false);
    }
    static void Run(void *Pointer)
    {
        auto &Self = *static_cast<RelayService *>(Pointer); Packet P;
        for (;;)
        {
            if (xQueueReceive(Self.Queue, &P, portMAX_DELAY) != pdTRUE) continue;
            if (P.IsProbe) { Self.RunProbe(P); continue; }
            if (!Self.Armed.load() || P.Generation != Self.Epoch.load()) { Self.DroppedBytes += P.Length; continue; }
            const uint64_t IdleDeadline = esp_timer_get_time() + 600000;
            while (uint32_t(esp_timer_get_time()) - Self.LastRx[1 - P.Source].load() < 8000 &&
                Self.Armed.load() && P.Generation == Self.Epoch.load() && esp_timer_get_time() < IdleDeadline) vTaskDelay(1);
            if (!Self.Armed.load() || P.Generation != Self.Epoch.load()) { Self.DroppedBytes += P.Length; continue; }
            if (esp_timer_get_time() >= IdleDeadline) { ++Self.Errors; Self.DroppedBytes += P.Length; continue; }
            const uint64_t Started = esp_timer_get_time();
            Maximum(Self.MaxQueueUs, uint32_t(Started - P.QueuedUs));
            const bool Ok = Transmit(1 - P.Source, P);
            Maximum(Self.MaxTxUs, uint32_t(esp_timer_get_time() - Started));
            if (Ok) { Self.ForwardedBytes += P.Length; Self.DirectionBytes[P.Source] += P.Length; ++Self.Packets[P.Source]; }
            else { ++Self.Errors; Self.DroppedBytes += P.Length; }
        }
    }
};
extern RelayService Relay;
