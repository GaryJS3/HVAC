#pragma once
#include <Arduino.h>
#include <esp_timer.h>
#include <atomic>
#include "TelemetryStore.h"

// Independent of protocol framing: records every byte drained from either UART.
class SerialCapture
{
public:
    bool Begin()
    {
        Queue = xQueueCreateStatic(32, sizeof(Entry), Storage, &QueueState);
        return Queue != nullptr;
    }
    void Record(unsigned Port, const uint8_t *Data, size_t Length, uint64_t Now)
    {
        Entry E{};
        E.Port = Port;
        E.Sequence = ++Sequences[Port];
        E.Chunk.DrainUs = Now;
        E.Chunk.Length = Length;
        memcpy(E.Chunk.Bytes, Data, Length);
        if (xQueueSend(Queue, &E, 0) != pdTRUE) ++DroppedChunks;
    }
    void Poll(TelemetryStore &Store)
    {
        if (Offset == Length)
        {
            Offset = Length = 0;
            uint64_t Now = esp_timer_get_time();
            if (Now >= NextHealthUs)
            {
                CommunicationHealth H[2];
                portENTER_CRITICAL(&Store.Lock);
                memcpy(H, Store.Health, sizeof(H));
                portEXIT_CRITICAL(&Store.Lock);
                Length = snprintf(Line, sizeof(Line),
                    "HEALTH %llu log_dropped_chunks=%u p0_bytes=%llu p0_frames=%llu p0_crc=%llu p0_framing=%llu p0_overruns=%llu p1_bytes=%llu p1_frames=%llu p1_crc=%llu p1_framing=%llu p1_overruns=%llu\n",
                    (unsigned long long)Now, DroppedChunks.load(),
                    (unsigned long long)H[0].Bytes, (unsigned long long)H[0].Frames,
                    (unsigned long long)H[0].CrcFailures, (unsigned long long)H[0].FramingErrors,
                    (unsigned long long)H[0].RxOverruns, (unsigned long long)H[1].Bytes,
                    (unsigned long long)H[1].Frames, (unsigned long long)H[1].CrcFailures,
                    (unsigned long long)H[1].FramingErrors, (unsigned long long)H[1].RxOverruns);
                NextHealthUs = Now + 5000000;
            }
            else
            {
                Entry E;
                if (xQueueReceive(Queue, &E, 0) != pdTRUE) return;
                Length = snprintf(Line, sizeof(Line), "RX %llu %u %llu %u ",
                    (unsigned long long)E.Chunk.DrainUs, E.Port,
                    (unsigned long long)E.Sequence, E.Chunk.Length);
                for (unsigned I = 0; I < E.Chunk.Length; ++I)
                {
                    snprintf(Line + Length, sizeof(Line) - Length, "%02X", E.Chunk.Bytes[I]);
                    Length += 2;
                }
                Line[Length++] = '\n';
            }
        }
        size_t Available = Serial.availableForWrite();
        size_t N = Length - Offset;
        if (N > Available) N = Available;
        if (N) Offset += Serial.write(reinterpret_cast<const uint8_t *>(Line + Offset), N);
    }
private:
    struct Entry
    {
        ByteChunk Chunk;
        uint64_t Sequence = 0;
        uint8_t Port = 0;
    };
    StaticQueue_t QueueState;
    uint8_t Storage[32 * sizeof(Entry)];
    QueueHandle_t Queue = nullptr;
    std::atomic<unsigned> DroppedChunks{0};
    uint64_t Sequences[2]{}, NextHealthUs = 0;
    char Line[768]{};
    size_t Offset = 0, Length = 0;
};

#if !defined(HVAC_XYE_QUERIES) || !HVAC_XYE_QUERIES
extern SerialCapture SerialLog;
#endif
