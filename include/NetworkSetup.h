#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include "ControllerConfig.h"
#include "TelemetryStore.h"
#include "XyeQueryController.h"
#include "SafetySupervisor.h"
#include "RelayService.h"
#include <atomic>
#if !__has_include("LocalConfig.h")
#include "LocalConfig.example.h"
#endif

class NetworkSetup
{
public:
    NetworkSetup(TelemetryStore &Store, XyeQueryController &Xye) : Store(Store), Xye(Xye) {}
    uint32_t BootId = 0;
    std::atomic<uint32_t> LastContact{0}, RejectedCommand{0};
    bool Connected() const { uint32_t Last = LastContact.load(); return Last && uint32_t(millis() - Last) < 10000; }
    String Ssid, Password, ControllerHost, Token;
    uint16_t ControllerPort = 8080;

    void Begin()
    {
        Preferences P;
        if (P.begin("hvac-net", true))
        {
            Ssid = P.getString("ssid", HVAC_WIFI_SSID);
            Password = P.getString("password", HVAC_WIFI_PASSWORD);
            ControllerHost = P.getString("host", "");
            ControllerPort = P.getUShort("port", 8080);
            Token = P.getString("token", HVAC_CONTROLLER_TOKEN);
            P.end();
        }
        else { Ssid = HVAC_WIFI_SSID; Password = HVAC_WIFI_PASSWORD; Token = HVAC_CONTROLLER_TOKEN; }
        ApplySaved();
        BootId = esp_random();
        WiFi.mode(WIFI_AP_STA);
        WiFi.softAP("HVAC-Diagnostics", HVAC_AP_PASSWORD);
        WiFi.setAutoReconnect(true);
        if (Ssid.length()) WiFi.begin(Ssid.c_str(), Password.c_str());
        xTaskCreatePinnedToCore(Worker, "ControllerLink", 8192, this, 1, nullptr, 0);
    }
    bool Save(const String &NewSsid, const String &NewPassword, const String &Host, uint16_t Port, const String &NewToken, bool ClearPassword)
    {
        const String Key = NewToken.length() ? NewToken : Token;
        if (NewSsid.length() > 32 || NewPassword.length() > 63 || Host.length() > 253 || Key.length() < 16 || Key.length() > 128 || !Port) return false;
        for (size_t I = 0; I < Key.length(); ++I) if (uint8_t(Key[I]) < 33 || uint8_t(Key[I]) > 126) return false;
        for (size_t I = 0; I < Host.length(); ++I)
            if (!(isAlphaNumeric(Host[I]) || Host[I] == '.' || Host[I] == '-')) return false;
        const String Pass = ClearPassword ? String("") : NewPassword.length() ? NewPassword : Password;
        if (Pass.length() && Pass.length() < 8) return false;
        Preferences P;
        if (!P.begin("hvac-net", false)) return false;
        // One NVS blob prevents mixed generations if power fails during save.
        String Config = NewSsid + "\n" + Pass + "\n" + Host + "\n" + String(Port) + "\n" + Key;
        if (NewSsid.indexOf('\n') >= 0 || Pass.indexOf('\n') >= 0 || Key.indexOf('\n') >= 0 || Key.indexOf('\r') >= 0) { P.end(); return false; }
        bool Ok = P.putString("pending", Config) == Config.length();
        P.end(); return Ok;
    }
    void ApplySaved()
    {
        Preferences P;
        if (!P.begin("hvac-net", true)) return;
        String C = P.getString("pending", ""); P.end();
        if (!C.length()) return;
        int A = C.indexOf('\n'), B = C.indexOf('\n', A+1), D = C.indexOf('\n', B+1), E = C.indexOf('\n', D+1);
        if (A < 0 || B < 0 || D < 0 || E < 0) return;
        Ssid = C.substring(0, A); Password = C.substring(A+1, B);
        ControllerHost = C.substring(B+1, D); ControllerPort = C.substring(D+1, E).toInt(); Token = C.substring(E+1);
    }
private:

    TelemetryStore &Store;
    XyeQueryController &Xye;
    DiagnosticSnapshot Snapshot;
    XyeDiagnostics XyeSnapshot;
    uint32_t LastCommand = 0;
    static String Bool(bool V) { return V ? "true" : "false"; }
    String Payload()
    {
        Store.Snapshot(Snapshot); Xye.Snapshot(XyeSnapshot);
        const uint64_t Now = esp_timer_get_time();
        String J; J.reserve(32000);
        J = String("{\"device_id\":\"") + WiFi.macAddress() + "\",\"boot_id\":" + BootId + ",\"device\":{\"lease_active\":" + Bool(Connected()) +
            ",\"proxy_enabled\":" + Bool(Relay.Armed.load()) + ",\"relay_errors\":" + Relay.Errors.load() +
            ",\"forwarded_bytes\":" + Relay.ForwardedBytes.load() + ",\"rejected_command_id\":" + RejectedCommand.load() +
            ",\"state\":\"" + (Relay.Armed.load() ? (Relay.Errors.load() ? "relay_fault" : Connected() ? "supervised_passthrough" : "failover_passthrough") : Connected() ? "supervised_monitor" : "standalone_monitor") +
            "\",\"capacity_override_supported\":false,\"proxy\":" + Relay.Json(Connected()) + "},\"status\":{\"uptime_us\":" + String((unsigned long long)Now) + ",\"ports\":[";
        for (unsigned P=0; P<2; ++P)
        {
            if (P) J += ',';
            const auto &H = Snapshot.Health[P];
            J += String("{\"port\":") + P + ",\"stale\":" + Bool(SafetySupervisor::Stale(H.LastValidUs, Now));
            const char *Names[] = {"bytes","valid_frames","crc_failures","framing_errors","parity_errors","rx_overruns","buffer_full","known_dropped_bytes","drop_events","queue_saturations","last_byte_us","last_valid_us","uart_framing_errors","parser_truncations"};
            uint64_t Values[] = {H.Bytes,H.Frames,H.CrcFailures,H.FramingErrors,H.ParityErrors,H.RxOverruns,H.BufferFull,H.DroppedBytes,H.DropEvents,H.QueueSaturations,H.LastByteUs,H.LastValidUs,H.UartFramingErrors,H.ParserTruncations};
            for(unsigned I=0; I<sizeof(Values)/sizeof(Values[0]); ++I) J += String(",\"")+Names[I]+"\":"+String((unsigned long long)Values[I]);
            J += ",\"telemetry\":{";
            for(size_t I=0; I<static_cast<size_t>(Field::Count); ++I)
            {
                if(I) J += ',';
                const auto &M=Snapshot.Latest[P].Values[I];
                J += String("\"")+S1S2Decoder::Name(static_cast<Field>(I))+"\":{\"value\":"+(M.Valid?String(M.Value,3):String("null"))+
                    ",\"valid\":"+Bool(M.Valid)+",\"stale\":"+Bool(SafetySupervisor::Stale(M.UpdatedUs,Now))+",\"updated_us\":"+String((unsigned long long)M.UpdatedUs)+
                    ",\"confidence\":\""+(M.Certainty==Confidence::ReferenceConfirmed?"reference_confirmed":M.Certainty==Confidence::Probable?"probable":"unknown")+"\"}";
            }
            J += "}}";
        }
        auto &X=XyeSnapshot;
        J += String("]},\"xye\":{\"valid\":")+Bool(X.UpdatedUs!=0)+",\"stale\":"+Bool(SafetySupervisor::Stale(X.UpdatedUs,Now))+",\"armed\":"+Bool(X.Armed)+
            ",\"address\":"+(X.AddressValidated?String(X.Address):String("null"))+",\"replies\":"+String((unsigned long long)X.Replies)+",\"timeouts\":"+String((unsigned long long)X.Timeouts)+
            ",\"command_id\":"+X.CommandId+",\"command_state\":"+X.CommandState+",\"mode\":"+X.Status.Mode+",\"fan\":"+X.Status.Fan+",\"setpoint_raw\":"+X.Status.SetpointRaw+",\"error_flags\":"+X.Status.ErrorFlags+",\"protection_flags\":"+X.Status.ProtectionFlags+",\"updated_us\":"+String((unsigned long long)X.UpdatedUs)+",\"queries\":"+String((unsigned long long)X.Queries)+",\"tx_errors\":"+String((unsigned long long)X.TxErrors)+",\"compiled\":"+Bool(XyeQueryController::Compiled)+"},\"frames\":[";
        for(size_t I=0; I<Snapshot.Count; ++I)
        {
            if(I) J += ',';
            const auto &F=Snapshot.Frames[I];
            J += String("{\"port\":")+F.Port+",\"first_us\":"+String((unsigned long long)F.FirstUs)+",\"last_us\":"+String((unsigned long long)F.LastUs)+
                ",\"result\":\""+(F.Result==FrameResult::Valid?"valid":F.Result==FrameResult::CrcFailure?"crc_failure":"truncated")+"\",\"hex\":\"";
            for(unsigned K=0; K<F.Length; ++K) { char Hex[3]; snprintf(Hex,sizeof(Hex),"%02X",F.Bytes[K]); J += Hex; }
            J += "\"}";
        }
        return J+"]}";
    }
    static bool Number(const String &Text, uint32_t &Out)
    {
        if(!Text.length() || Text.length()>10) return false;
        uint64_t V=0;
        for(size_t I=0; I<Text.length(); ++I) { if(Text[I]<'0'||Text[I]>'9') return false; V=V*10+Text[I]-'0'; }
        if(V>UINT32_MAX) return false; Out=V; return true;
    }
    void Exchange()
    {
        HTTPClient Http;
        Http.setConnectTimeout(1500); Http.setTimeout(1500);
        if(!Http.begin("http://"+ControllerHost+":"+String(ControllerPort)+"/api/devices/register")) return;
        const char *Headers[]={"X-HVAC-Lease","X-HVAC-Boot","X-HVAC-Command","X-HVAC-Id","X-HVAC-Mode","X-HVAC-Fan","X-HVAC-Temp","X-HVAC-Enabled","X-HVAC-Source"};
        Http.collectHeaders(Headers,9);
        Http.addHeader("Authorization","Bearer "+Token); Http.addHeader("Content-Type","application/json");
        int Code=Http.POST(Payload());
        if(Code==200 && Http.header("X-HVAC-Lease")=="1" && Http.header("X-HVAC-Boot")==String(BootId))
        {
            LastContact.store(millis());
            String Kind=Http.header("X-HVAC-Command"); uint32_t Id=0;
            if(Kind.length() && Number(Http.header("X-HVAC-Id"),Id) && Id && Id!=LastCommand)
            {
                LastCommand=Id; bool Ok=false; uint32_t M,F,T,E;
                if(Kind=="xye_polling" && Number(Http.header("X-HVAC-Enabled"),E) && E<=1) Ok=Xye.Arm(E==1);
                if(Kind=="proxy" && Number(Http.header("X-HVAC-Enabled"),E) && E<=1) Ok=Relay.Arm(E==1,Id);
                if(Kind=="proxy_probe" && Number(Http.header("X-HVAC-Source"),E) && E<=1) Ok=Relay.Probe(E,Id);
                if(Kind=="climate" && !Relay.Errors.load() && Number(Http.header("X-HVAC-Mode"),M) && Number(Http.header("X-HVAC-Fan"),F) && Number(Http.header("X-HVAC-Temp"),T) && M<=255 && F<=255 && T>=60 && T<=86)
                    Ok=Xye.Set(M,F,T,Id,esp_timer_get_time()+2000000);
                if(!Ok) RejectedCommand.store(Id);
            }
        }
        Http.end();
    }
    static void Worker(void *Context)
    {
        auto &Self=*static_cast<NetworkSetup *>(Context);
        for(;;)
        {
            if(WiFi.status()==WL_CONNECTED && Self.ControllerHost.length() && Self.Token.length()>=16) Self.Exchange();
            vTaskDelay(pdMS_TO_TICKS(2000));
        }
    }
};
