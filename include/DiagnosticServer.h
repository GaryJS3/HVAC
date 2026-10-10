#pragma once
#include <WebServer.h>
#include <WiFi.h>
#include <esp_timer.h>
#include "TelemetryStore.h"
#include "SafetySupervisor.h"
#include "XyeQueryController.h"
#include "ControllerLease.h"
#include "ControllerConfig.h"
#include "RelayService.h"
#include "NetworkSetup.h"
#include "SetupPage.h"

class DiagnosticServer
{
public:
    DiagnosticServer(TelemetryStore &Store, XyeQueryController &Xye, NetworkSetup &Network) : Store(Store), Xye(Xye), Network(Network) {}
    void Begin()
    {
        const char *Headers[] = {"Authorization"};
        Server.collectHeaders(Headers, 1);
        BootId = Network.BootId;
        Server.on("/api/proxy", HTTP_GET, [this]() { Server.send(200, "application/json", Relay.Json(Network.Connected())); });
        Server.on("/api/controller", HTTP_GET, [this]() { ControllerStatus(); });
        Server.on("/api/controller/heartbeat", HTTP_POST, [this]()
        {
            if (!Authorize()) return;
            Lease.Renew(esp_timer_get_time()); ControllerStatus();
        });
        Server.on("/api/controller/release", HTTP_POST, [this]()
        {
            if (!Authorize()) return;
            Lease.Revoke(); ControllerStatus();
        });
        Server.on("/api/xye/set", HTTP_POST, [this]()
        {
            if (!Authorize()) return;
            const uint64_t Now = esp_timer_get_time();
            if (!Lease.Active(Now) || Relay.Errors.load() || Server.arg("boot_id") != String(BootId))
            { Server.send(409, "application/json", "{\"error\":\"fresh controller lease and current boot required\"}"); return; }
            const String Mode = Server.arg("mode"), Fan = Server.arg("fan"), Temp = Server.arg("temperature_f");
            int M = Mode == "off" ? 0 : Mode == "cool" ? 0x88 : Mode == "heat" ? 0x84 : Mode == "dry" ? 0x82 : Mode == "fan" ? 0x81 : -1;
            int F = Fan == "auto" ? 0x80 : Fan == "high" ? 1 : Fan == "medium" ? 2 : Fan == "low" ? 4 : -1;
            int T = Temp.toInt();
            if (M < 0 || F < 0 || T < 60 || T > 86 || Temp != String(T))
            { Server.send(400, "application/json", "{\"error\":\"invalid mode, fan or whole Fahrenheit temperature (60..86)\"}"); return; }
            uint32_t Id = ++NextCommand;
            bool Ok = Xye.Set(M, F, T, Id, Lease.Deadline() < Now + 2000000 ? Lease.Deadline() : Now + 2000000);
            Server.send(Ok ? 202 : 409, "application/json", Ok ? String("{\"accepted\":true,\"command_id\":") + Id + "}" : "{\"error\":\"XYE unavailable or command pending\"}");
        });
        Server.on("/", [this]() { Server.send_P(200, "text/html", SetupPage); });
        Server.on("/api/network", HTTP_GET, [this]()
        {
            String J = String("{\"ssid\":\"") + Escape(Network.Ssid) + "\",\"controller_host\":\"" + Escape(Network.ControllerHost) + "\",\"controller_port\":" + Network.ControllerPort +
                ",\"key_configured\":" + (Network.Token.length() >= 16 ? "true" : "false") + ",\"wifi_connected\":" + (WiFi.status() == WL_CONNECTED ? "true" : "false") +
                ",\"registered\":" + (Network.Connected() ? "true" : "false") + ",\"ip\":\"" + WiFi.localIP().toString() + "\"}";
            Server.send(200, "application/json", J);
        });
        Server.on("/api/network", HTTP_POST, [this]()
        {
            if (Network.Token.length() >= 16) { if (!Authorize()) return; }
            else if (Server.client().localIP() != WiFi.softAPIP()) { Server.send(403, "application/json", "{\"error\":\"Initial setup requires diagnostic Wi-Fi\"}"); return; }
            String Port = Server.arg("port"); long N = Port.toInt();
            if (N < 1 || N > 65535 || Port != String(N) || !Network.Save(Server.arg("ssid"), Server.arg("password"), Server.arg("host"), N, Server.arg("token"), Server.arg("open") == "1"))
            { Server.send(400, "application/json", "{\"error\":\"Invalid settings or storage unavailable. Device key needs 16 characters.\"}"); return; }
            Server.send(200, "application/json", "{\"saved\":true}"); RestartAt = millis() + 1000;
        });
        Server.on("/api/status", HTTP_GET, [this]() { Status(); });
        Server.on("/api/frames", HTTP_GET, [this]() { Frames(); });
        Server.on("/api/bytes", HTTP_GET, [this]() { Bytes(); });
        Server.on("/api/xye", HTTP_GET, [this]() { XyeStatus(); });
        Server.on("/api/xye/enable", HTTP_POST, [this]()
        {
            if (!Authorize()) return;
            bool Ok = Xye.Arm(true);
            Server.send(Ok ? 202 : 409, "application/json", Ok ? "{\"enable_requested\":true}" : "{\"error\":\"XYE query build required\"}");
        });
        Server.on("/api/xye/disable", HTTP_POST, [this]()
        {
            if (!Authorize()) return;
            Xye.Arm(false);
            Server.send(202, "application/json", "{\"disable_requested\":true}");
        });
        Server.begin();
    }
    void Poll() { Server.handleClient(); if (RestartAt && int32_t(millis()-RestartAt)>=0) ESP.restart(); }
private:
    static String Escape(const String &S) { String R; for (size_t I=0; I<S.length(); ++I) { char C=S[I]; if(C=='"'||C=='\\') R+='\\'; if(uint8_t(C)<32) R+=' '; else R+=C; } return R; }
    bool Authorize()
    {
        if (Network.Token.length() >= 16 && Server.header("Authorization") == String("Bearer ") + Network.Token) return true;
        Server.send(401, "application/json", "{\"error\":\"controller token required\"}"); return false;
    }
    void ControllerStatus()
    {
        const bool Active = Network.Connected() || Lease.Active(esp_timer_get_time());
        String Json = String("{\"boot_id\":") + BootId + ",\"lease_active\":" + (Active ? "true" : "false") +
            ",\"lease_seconds\":10,\"proxy_enabled\":" + (Relay.Armed.load() ? "true" : "false") +
            ",\"state\":\"" + (Relay.Armed.load() ? (Relay.FaultActive() ? "relay_fault" : (Active ? "supervised_passthrough" : "failover_passthrough")) : (Active ? "supervised_monitor" : "standalone_monitor")) +
            "\",\"forwarded_bytes\":" + Relay.ForwardedBytes.load() + ",\"relay_errors\":" + Relay.Errors.load() +
            ",\"capacity_override_supported\":" + (RelayService::Compiled ? "true" : "false") + ",\"ceiling\":" + Relay.CeilingJson() + "}";
        Server.send(200, "application/json", Json);
    }
    void XyeStatus()
    {
        Xye.Snapshot(XyeSnapshot);
        auto &S = XyeSnapshot;
        String Json = String("{\"compiled\":") + (XyeQueryController::Compiled ? "true" : "false") + ",\"ready\":" + (S.Ready ? "true" : "false") + ",\"armed\":" + (S.Armed ? "true" : "false");
        Json += String(",\"address_validated\":") + (S.AddressValidated ? "true" : "false") + ",\"address\":" + (S.AddressValidated ? String(S.Address) : String("null"));
        const char *Names[] = {"queries", "replies", "timeouts", "invalid_candidates", "tx_errors", "updated_us", "failure_streak", "retry_delay_ms"};
        uint64_t Values[] = {S.Queries, S.Replies, S.Timeouts, S.InvalidCandidates, S.TxErrors, S.UpdatedUs, S.FailureStreak, S.RetryDelayMs};
        for (unsigned I = 0; I < sizeof(Values) / sizeof(Values[0]); ++I) Json += String(",\"") + Names[I] + "\":" + String(static_cast<unsigned long long>(Values[I]));
        bool Valid = S.UpdatedUs != 0;
        Json += String(",\"valid\":") + (Valid ? "true" : "false") + ",\"stale\":" + (SafetySupervisor::Stale(S.UpdatedUs, esp_timer_get_time()) ? "true" : "false");
        Json += ",\"master_address\":" + String(XyeQueryController::MasterAddress);
        Json += ",\"command_id\":" + String(S.CommandId) + ",\"command_state\":" + String(S.CommandState);
        const char *Fields[] = {"mode", "fan", "setpoint_raw", "error_flags", "protection_flags"};
        unsigned V[] = {S.Status.Mode, S.Status.Fan, S.Status.SetpointRaw, S.Status.ErrorFlags, S.Status.ProtectionFlags};
        for (unsigned I = 0; I < 5; ++I) Json += String(",\"") + Fields[I] + "\":" + (Valid ? String(V[I]) : String("null"));
        Json += ",\"recent_rx_hex\":\"";
        for (size_t I = 0; I < S.Bytes.Count; ++I)
        {
            auto &C = S.Bytes.At(I);
            for (size_t J = 0; J < C.Length; ++J)
            {
                char Hex[3]; snprintf(Hex, sizeof(Hex), "%02X", C.Bytes[J]); Json += Hex;
            }
        }
        Json += "\"}";
        Server.send(200, "application/json", Json);
    }
    void Bytes()
    {
        String Json;
        Json.reserve(12000);
        Json = "{\"timestamp_basis\":\"monotonic_uart_drain_us\",\"ports\":[";
        for (unsigned Port = 0; Port < 2; ++Port)
        {
            Store.SnapshotBytes(Port, ByteSnapshot);
            if (Port) Json += ',';
            Json += "{\"port\":" + String(Port) + ",\"chunk_evictions\":" + String(static_cast<unsigned long long>(ByteSnapshot.Evictions)) + ",\"chunks\":[";
            for (size_t I = 0; I < ByteSnapshot.Count; ++I)
            {
                if (I) Json += ',';
                const auto &C = ByteSnapshot.At(I);
                Json += "{\"sequence\":" + String(static_cast<unsigned long long>(C.Sequence)) + ",\"drain_us\":" + String(static_cast<unsigned long long>(C.DrainUs));
                Json += ",\"length\":" + String(C.Length) + ",\"hex\":\"";
                for (unsigned J = 0; J < C.Length; ++J)
                {
                    char Hex[3];
                    snprintf(Hex, sizeof(Hex), "%02X", C.Bytes[J]);
                    Json += Hex;
                }
                Json += "\"}";
            }
            Json += "]}";
        }
        Json += "]}";
        Server.send(200, "application/json", Json);
    }
    void Status()
    {
        Store.Snapshot(Snapshot);
        uint64_t Now = esp_timer_get_time();
        String Json;
        Json.reserve(12000);
        Xye.Snapshot(XyeSnapshot);
        Json = String("{\"receive_only\":") + (XyeSnapshot.Armed || Relay.Armed.load() ? "false" : "true") + ",\"xye_enabled\":" + (XyeQueryController::Compiled ? "true" : "false") + ",\"proxy_enabled\":" + (Relay.Armed.load() ? "true" : "false") + ",\"uart_ready\":true,\"uptime_us\":" + String(static_cast<unsigned long long>(Now));
        Json += ",\"ip\":\"" + WiFi.localIP().toString() + "\",\"ap_ip\":\"" + WiFi.softAPIP().toString() + "\",\"history_evictions\":" + String(static_cast<unsigned long long>(Snapshot.HistoryEvictions));
        Json += ",\"ports\":[";
        for (unsigned P = 0; P < 2; ++P)
        {
            if (P) Json += ',';
            auto &H = Snapshot.Health[P];
            Json += "{\"port\":" + String(P) + ",\"stale\":" + (SafetySupervisor::Stale(H.LastValidUs, Now) ? "true" : "false");
            Json += String(",\"segment\":\"") + (P == 0 ? "odu" : "idu") + "\",\"rx_gpio\":" + String(P == 0 ? HardwareConfig::OduRx : HardwareConfig::IduRx);
            const char *Names[] = {"bytes", "valid_frames", "crc_failures", "framing_errors", "parity_errors", "rx_overruns", "buffer_full", "known_dropped_bytes", "drop_events", "queue_saturations", "last_byte_us", "last_valid_us", "uart_framing_errors", "parser_truncations"};
            uint64_t Values[] = {H.Bytes, H.Frames, H.CrcFailures, H.FramingErrors, H.ParityErrors, H.RxOverruns, H.BufferFull, H.DroppedBytes, H.DropEvents, H.QueueSaturations, H.LastByteUs, H.LastValidUs, H.UartFramingErrors, H.ParserTruncations};
            for (unsigned I = 0; I < sizeof(Values) / sizeof(Values[0]); ++I)
                Json += String(",\"") + Names[I] + "\":" + String(static_cast<unsigned long long>(Values[I]));
            Json += ",\"telemetry\":{";
            for (size_t I = 0; I < static_cast<size_t>(Field::Count); ++I)
            {
                if (I) Json += ',';
                auto &M = Snapshot.Latest[P].Values[I];
                Json += String("\"") + S1S2Decoder::Name(static_cast<Field>(I)) + "\":{\"value\":";
                Json += M.Valid ? String(M.Value, 3) : "null";
                Json += String(",\"valid\":") + (M.Valid ? "true" : "false") + ",\"stale\":" + (SafetySupervisor::Stale(M.UpdatedUs, Now) ? "true" : "false");
                Json += ",\"updated_us\":" + String(static_cast<unsigned long long>(M.UpdatedUs));
                Json += String(",\"confidence\":\"") + (M.Certainty == Confidence::ReferenceConfirmed ? "reference_confirmed" : M.Certainty == Confidence::Probable ? "probable" : "unknown") + "\"}";
            }
            Json += "}}";
        }
        Json += "]}";
        Server.send(200, "application/json", Json);
    }
    void Frames()
    {
        Store.Snapshot(Snapshot);
        String Json;
        Json.reserve(22000);
        Json = "{\"timestamp_basis\":\"monotonic_uart_drain_us\",\"frames\":[";
        for (size_t I = 0; I < Snapshot.Count; ++I)
        {
            if (I) Json += ',';
            auto &F = Snapshot.Frames[I];
            Json += "{\"port\":" + String(F.Port) + ",\"first_us\":" + String(static_cast<unsigned long long>(F.FirstUs));
            Json += ",\"last_us\":" + String(static_cast<unsigned long long>(F.LastUs));
            Json += ",\"length\":" + String(F.Length);
            Json += String(",\"result\":\"") + (F.Result == FrameResult::Valid ? "valid" : F.Result == FrameResult::CrcFailure ? "crc_failure" : "truncated") + "\"";
            if (F.Length >= 4)
                Json += ",\"address\":" + String((F.Bytes[1] << 8) | F.Bytes[2]) + ",\"command\":" + String(F.Bytes[3]);
            Json += ",\"hex\":\"";
            for (unsigned J = 0; J < F.Length; ++J)
            {
                char Hex[3];
                snprintf(Hex, sizeof(Hex), "%02X", F.Bytes[J]);
                Json += Hex;
            }
            Json += "\"}";
        }
        Json += "]}";
        Server.send(200, "application/json", Json);
    }
    TelemetryStore &Store;
    XyeQueryController &Xye;
    NetworkSetup &Network;
    uint32_t RestartAt = 0;
    XyeDiagnostics XyeSnapshot;
    WebServer Server{80};
    ControllerLease Lease;
    uint32_t BootId = 0, NextCommand = 0;
    DiagnosticSnapshot Snapshot;
    ByteHistory ByteSnapshot;
};
