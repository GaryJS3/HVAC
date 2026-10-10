#pragma once
#include <driver/uart.h>
#include <esp_timer.h>
#include "TelemetryStore.h"
#include "S1S2FrameParser.h"
#include "SerialCapture.h"
#include "RelayService.h"

class Rs485Port
{
public:
    Rs485Port(uart_port_t Uart, unsigned Index, TelemetryStore &Store)
        : Uart(Uart), Index(Index), Store(Store), Parser(OnFrame, this) {}
    bool Begin(int Rx)
    {
        uart_config_t Config{};
        Config.baud_rate = 4800;
        Config.data_bits = UART_DATA_8_BITS;
        Config.parity = UART_PARITY_DISABLE;
        Config.stop_bits = UART_STOP_BITS_1;
        Config.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
        Config.source_clk = UART_SCLK_APB;
        // RX only: TX GPIO is deliberately never routed to a transceiver.
        return uart_param_config(Uart, &Config) == ESP_OK &&
            uart_set_pin(Uart, RelayService::Compiled ? (Index == 0 ? HardwareConfig::OduTx : HardwareConfig::IduTx) : UART_PIN_NO_CHANGE, Rx, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE) == ESP_OK &&
            uart_driver_install(Uart, 2048, 0, 64, &Events, 0) == ESP_OK &&
            uart_set_rx_full_threshold(Uart, RelayService::Compiled ? 1 : 64) == ESP_OK &&
            uart_set_rx_timeout(Uart, RelayService::Compiled ? 1 : 3) == ESP_OK;
    }
    void Poll()
    {
        uart_event_t Event;
        unsigned Budget = 64;
        while (Budget-- && xQueueReceive(Events, &Event, 0) == pdTRUE)
        {
            if (Event.type == UART_DATA)
            {
                size_t Left = Event.size;
                while (Left)
                {
                    uint8_t Buffer[128];
                    int N = uart_read_bytes(Uart, Buffer, Left < sizeof(Buffer) ? Left : sizeof(Buffer), 0);
                    if (N <= 0) break;
                    Left -= N;
                    const uint64_t Now = esp_timer_get_time();
                    Relay.Submit(Index, Buffer, N);
                    portENTER_CRITICAL(&Store.Lock);
                    Store.Health[Index].Bytes += N;
                    Store.Health[Index].LastByteUs = Now;
                    portEXIT_CRITICAL(&Store.Lock);
                    Store.RecordBytes(Index, Buffer, N, Now);
#if !defined(HVAC_XYE_QUERIES) || !HVAC_XYE_QUERIES
                    SerialLog.Record(Index, Buffer, N, Now);
#endif
                    for (int I = 0; I < N; ++I) Parser.Feed(Buffer[I], Now);
                }
            }
            else
            {
                portENTER_CRITICAL(&Store.Lock);
                auto &H = Store.Health[Index];
                if (Event.type == UART_FRAME_ERR) ++H.FramingErrors;
                if (Event.type == UART_PARITY_ERR) ++H.ParityErrors;
                if (Event.type == UART_FIFO_OVF) ++H.RxOverruns;
                if (Event.type == UART_BUFFER_FULL) ++H.BufferFull;
                portEXIT_CRITICAL(&Store.Lock);
                if (Event.type == UART_FIFO_OVF || Event.type == UART_BUFFER_FULL)
                {
                    size_t Buffered = 0;
                    uart_get_buffered_data_len(Uart, &Buffered);
                    portENTER_CRITICAL(&Store.Lock);
                    Store.Health[Index].DroppedBytes += Buffered;
                    ++Store.Health[Index].DropEvents;
                    portEXIT_CRITICAL(&Store.Lock);
                    uart_flush_input(Uart);
                    xQueueReset(Events);
                    Parser.Reset();
                }
            }
        }
        if (uxQueueMessagesWaiting(Events) == 64)
        {
            portENTER_CRITICAL(&Store.Lock);
            ++Store.Health[Index].QueueSaturations;
            portEXIT_CRITICAL(&Store.Lock);
        }
        Parser.Tick(esp_timer_get_time());
    }
private:
    static void OnFrame(void *Context, const RawFrame &Frame)
    {
        auto *Self = static_cast<Rs485Port *>(Context);
        Self->Store.Record(Self->Index, Frame);
    }
    uart_port_t Uart;
    unsigned Index;
    TelemetryStore &Store;
    S1S2FrameParser Parser;
    QueueHandle_t Events = nullptr;
};
