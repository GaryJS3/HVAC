#pragma once
#include <Arduino.h>
#include <driver/uart.h>
#include <atomic>
#include "HardwareConfig.h"
#ifndef HVAC_PROXY_BENCH
#define HVAC_PROXY_BENCH 0
#endif
// Bench-only transport: relay raw chunks, independent of parsing/CRC/cloud state.
// Never enable with the two segments electrically jumpered together.
class RelayService
{
public:
    static constexpr bool Compiled = HVAC_PROXY_BENCH != 0;
    std::atomic<uint32_t> ForwardedBytes{0}, Errors{0};
    bool Begin()
    {
        if (!Compiled) return true;
        Queue = xQueueCreate(32, sizeof(Chunk));
        return Queue && xTaskCreatePinnedToCore(Run, "RawRelay", 4096, this, 4, nullptr, 1) == pdPASS;
    }
    void Submit(unsigned Source, const uint8_t *Bytes, size_t Length)
    {
        if (!Compiled) return;
        Chunk C{}; C.Source = Source; C.Length = Length;
        memcpy(C.Bytes, Bytes, Length);
        if (!Queue || xQueueSend(Queue, &C, 0) != pdTRUE) ++Errors;
    }
private:
    struct Chunk { uint8_t Source; uint16_t Length; uint8_t Bytes[128]; };
    QueueHandle_t Queue = nullptr;
    static void Run(void *Context)
    {
        auto &Self = *static_cast<RelayService *>(Context);
        Chunk C;
        for (;;)
        {
            if (xQueueReceive(Self.Queue, &C, portMAX_DELAY) != pdTRUE) continue;
            const uart_port_t Target = C.Source == 0 ? UART_NUM_2 : UART_NUM_1;
            const int En = C.Source == 0 ? HardwareConfig::IduEn : HardwareConfig::OduEn;
            digitalWrite(En, HIGH);
            delayMicroseconds(10);
            const int N = uart_write_bytes(Target, C.Bytes, C.Length);
            const bool Done = uart_wait_tx_done(Target, pdMS_TO_TICKS(350)) == ESP_OK;
            digitalWrite(En, LOW);
            if (N == C.Length && Done) Self.ForwardedBytes += N;
            else ++Self.Errors;
        }
    }
};
extern RelayService Relay;
