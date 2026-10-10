#pragma once
#include <Arduino.h>
#include <driver/uart.h>
#include <esp_timer.h>
#include <atomic>
#include "HardwareConfig.h"
#include "XYEProtocol.h"
#include "ByteHistory.h"

#ifndef HVAC_XYE_QUERIES
#define HVAC_XYE_QUERIES 0
#endif

struct XyeDiagnostics
{
    bool Ready = false, Armed = false, AddressValidated = false;
    uint8_t Address = 0;
    uint64_t Queries = 0, Replies = 0, Timeouts = 0, InvalidCandidates = 0, TxErrors = 0;
    uint64_t UpdatedUs = 0;
    uint32_t CommandId = 0;
    uint8_t CommandState = 0; // 0 none, 1 sent, 2 verified, 3 expired/failed
    XYEProtocol::Status Status;
    ByteHistory Bytes;
};
class XyeQueryController
{
public:
    static constexpr bool Compiled = HVAC_XYE_QUERIES != 0;
    static constexpr uint8_t MasterAddress = 63; // Matches user's working AC project.
    bool Begin()
    {
        if (!Compiled) return true;
        uart_config_t C{};
        C.baud_rate = 4800;
        C.data_bits = UART_DATA_8_BITS;
        C.parity = UART_PARITY_DISABLE;
        C.stop_bits = UART_STOP_BITS_1;
        C.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
        C.source_clk = UART_SCLK_APB;
        State.Ready = uart_param_config(UART_NUM_0, &C) == ESP_OK &&
            uart_set_pin(UART_NUM_0, HardwareConfig::XyeTx, HardwareConfig::XyeRx, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE) == ESP_OK &&
            uart_driver_install(UART_NUM_0, 1024, 0, 0, nullptr, 0) == ESP_OK &&
            uart_set_rx_timeout(UART_NUM_0, 3) == ESP_OK;
        return State.Ready;
    }
    bool Set(uint8_t Mode, uint8_t Fan, uint8_t TempF, uint32_t Id, uint64_t Deadline)
    {
        uint8_t Check[16];
        if (!Compiled || !XYEProtocol::BuildSet(0, Mode, Fan, TempF, Check)) return false;
        portENTER_CRITICAL(&Lock);
        const bool Ok = Published.AddressValidated && Published.Armed && !CommandBusy;
        if (Ok) { SetMode = Mode; SetFan = Fan; SetTemp = TempF; SetId = Id; SetDeadline = Deadline; PendingSet = true; CommandBusy = true; }
        portEXIT_CRITICAL(&Lock);
        return Ok;
    }
    bool Arm(bool Enable)
    {
        if (!Compiled || !State.Ready) return false;
        Requested.store(Enable);
        return true;
    }
    void Snapshot(XyeDiagnostics &Out)
    {
        portENTER_CRITICAL(&Lock);
        Out = Published;
        portEXIT_CRITICAL(&Lock);
    }
    void Poll()
    {
        if (!Compiled || !State.Ready) return;
        uint64_t Now = esp_timer_get_time();
        bool ArmRequested = Requested.load();
        if (ArmRequested != State.Armed)
        {
            State.Armed = ArmRequested;
            Waiting = false;
            Used = 0;
            uart_flush_input(UART_NUM_0);
            NextQueryUs = Now + 1000000;
        }
        uint8_t Data[128];
        int N = uart_read_bytes(UART_NUM_0, Data, sizeof(Data), 0);
        if (N > 0)
        {
            State.Bytes.Add(Data, N, Now);
            LastRxUs = Now;
            for (int I = 0; I < N; ++I) Feed(Data[I], Now);
        }
        if (Waiting && Now >= DeadlineUs)
        {
            Waiting = false;
            ++State.Timeouts;
            if (State.AddressValidated && ++ConsecutiveTimeouts >= 3)
            {
                State.AddressValidated = false;
                ScanAddress = 0;
            }
            else if (!State.AddressValidated) ScanAddress = (ScanAddress + 1) & 0x3F;
        }
        if (State.CommandState == 1 && Now >= VerificationDeadlineUs) State.CommandState = 3;
        if (!Waiting && State.Armed && Now - LastRxUs >= 130000)
        {
            bool Take = false;
            portENTER_CRITICAL(&Lock);
            if (PendingSet)
            {
                Take = true; PendingSet = false;
                ExpectedMode = SetMode; ExpectedFan = SetFan; ExpectedTemp = SetTemp;
                State.CommandId = SetId;
            }
            portEXIT_CRITICAL(&Lock);
            if (Take)
            {
                uint8_t Command[16];
                bool Ok = Now < SetDeadline && State.AddressValidated &&
                    XYEProtocol::BuildSet(State.Address, ExpectedMode, ExpectedFan, ExpectedTemp, Command, MasterAddress);
                if (Ok)
                {
                    digitalWrite(HardwareConfig::XyeEn, HIGH); delayMicroseconds(10);
                    const int N = uart_write_bytes(UART_NUM_0, Command, sizeof(Command));
                    Ok = uart_wait_tx_done(UART_NUM_0, pdMS_TO_TICKS(60)) == ESP_OK && N == sizeof(Command);
                    digitalWrite(HardwareConfig::XyeEn, LOW);
                }
                State.CommandState = Ok ? 1 : 3;
                VerificationDeadlineUs = esp_timer_get_time() + 5000000;
                NextQueryUs = esp_timer_get_time() + 250000;
            }
        }
        // At most one C0 per second; skip rather than catch up. Require an idle
        // receive line for 130 ms. Another XYE master must be disconnected.
        if (State.Armed && !Waiting && Now >= NextQueryUs && Now - LastRxUs >= 130000)
        {
            Used = 0;
            PendingAddress = State.AddressValidated ? State.Address : ScanAddress;
            uint8_t Query[16];
            XYEProtocol::BuildQuery(PendingAddress, Query, MasterAddress);
            digitalWrite(HardwareConfig::XyeEn, HIGH);
            delayMicroseconds(10);
            int Written = uart_write_bytes(UART_NUM_0, Query, sizeof(Query));
            esp_err_t Finished = uart_wait_tx_done(UART_NUM_0, pdMS_TO_TICKS(60));
            digitalWrite(HardwareConfig::XyeEn, LOW);
            const uint64_t Done = esp_timer_get_time();
            NextQueryUs = Done + 1000000;
            if (Written != sizeof(Query) || Finished != ESP_OK)
            {
                ++State.TxErrors;
                State.Armed = false;
                Requested.store(false);
            }
            else
            {
                ++State.Queries;
                Waiting = true;
                DeadlineUs = Done + 1000000;
            }
        }
        portENTER_CRITICAL(&Lock);
        Published = State;
        if (!PendingSet && State.CommandState != 1) CommandBusy = false;
        portEXIT_CRITICAL(&Lock);
    }
private:
    void Feed(uint8_t Byte, uint64_t Now)
    {
        if (!Used && Byte != 0xAA) return;
        Buffer[Used++] = Byte;
        if (Used < sizeof(Buffer)) return;
        XYEProtocol::Status Reply;
        bool Valid = XYEProtocol::DecodeStatus(Buffer, sizeof(Buffer), Reply, MasterAddress);
        if (Valid && Waiting && Now <= DeadlineUs && Reply.Address == PendingAddress)
        {
            State.Status = Reply;
            State.Address = Reply.Address;
            State.AddressValidated = true;
            State.UpdatedUs = Now;
            ++State.Replies;
            if (State.CommandState == 1 && XYEProtocol::MatchesSet(Reply, ExpectedMode, ExpectedFan, ExpectedTemp)) State.CommandState = 2;
            ConsecutiveTimeouts = 0;
            Waiting = false;
            Used = 0;
            return;
        }
        ++State.InvalidCandidates;
        memmove(Buffer, Buffer + 1, --Used);
        while (Used && Buffer[0] != 0xAA) memmove(Buffer, Buffer + 1, --Used);
    }
    std::atomic<bool> Requested{false};
    portMUX_TYPE Lock = portMUX_INITIALIZER_UNLOCKED;
    XyeDiagnostics State, Published;
    uint8_t Buffer[32]{};
    size_t Used = 0;
    bool Waiting = false;
    uint8_t ScanAddress = 0, PendingAddress = 0, ConsecutiveTimeouts = 0;
    uint64_t DeadlineUs = 0, NextQueryUs = 0, LastRxUs = 0;
    bool PendingSet = false, CommandBusy = false;
    uint8_t SetMode = 0, SetFan = 0, SetTemp = 0, ExpectedMode = 0, ExpectedFan = 0, ExpectedTemp = 0;
    uint32_t SetId = 0;
    uint64_t SetDeadline = 0, VerificationDeadlineUs = 0;
};
