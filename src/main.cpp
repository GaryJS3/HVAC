#include <Arduino.h>
#include <WiFi.h>
#include <esp_log.h>
#include "HardwareConfig.h"
#include "Rs485Port.h"
#include "DiagnosticServer.h"
#if __has_include("LocalConfig.h")
#include "LocalConfig.h"
#else
#include "LocalConfig.example.h"
#endif

TelemetryStore Store;
RelayService Relay;
Rs485Port Odu(UART_NUM_1, 0, Store), Idu(UART_NUM_2, 1, Store);
XyeQueryController Xye;
DiagnosticServer Diagnostics(Store, Xye);
#if !HVAC_XYE_QUERIES
SerialCapture SerialLog;
void SerialTask(void *)
{
    for (;;)
    {
        SerialLog.Poll(Store);
        vTaskDelay(1);
    }
}
#endif

void XyeTask(void *)
{
    for (;;)
    {
        Xye.Poll();
        vTaskDelay(1);
    }
}

void ReceiveTask(void *)
{
    for (;;)
    {
        Idu.Poll();
        Odu.Poll();
        Store.Publish();
        vTaskDelay(1);
    }
}
void setup()
{
    for (int Pin : {HardwareConfig::XyeEn, HardwareConfig::IduEn, HardwareConfig::OduEn})
    {
        digitalWrite(Pin, HardwareConfig::ReceiveLevel);
        pinMode(Pin, OUTPUT);
    }
    // UART0 stays on USB in the default build; query builds omit Serial entirely.
#if !HVAC_XYE_QUERIES
    Serial.begin(115200);
    if (!SerialLog.Begin()) return;
#endif
    bool Ready = Idu.Begin(HardwareConfig::IduRx) && Odu.Begin(HardwareConfig::OduRx) && Relay.Begin();
    TaskHandle_t Task = nullptr;
    if (!Ready || xTaskCreatePinnedToCore(ReceiveTask, "S1S2Receive", 8192, nullptr, 3, &Task, 1) != pdPASS)
    {
#if !HVAC_XYE_QUERIES
        Serial.println("UART/task initialization failed; diagnostics disabled. EN remains LOW.");
#endif
        return;
    }
    if (XyeQueryController::Compiled) esp_log_level_set("*", ESP_LOG_NONE);
    if (XyeQueryController::Compiled && (!Xye.Begin() ||
        xTaskCreatePinnedToCore(XyeTask, "XyeQueries", 4096, nullptr, 2, nullptr, 1) != pdPASS)) return;
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP("HVAC-Diagnostics", HVAC_AP_PASSWORD);
    if (strlen(HVAC_WIFI_SSID)) WiFi.begin(HVAC_WIFI_SSID, HVAC_WIFI_PASSWORD);
    Diagnostics.Begin();
#if !HVAC_XYE_QUERIES
    Serial.print("Receive only. Diagnostic AP: ");
    Serial.println(WiFi.softAPIP());
    xTaskCreatePinnedToCore(SerialTask, "SerialCapture", 4096, nullptr, 1, nullptr, 1);
#endif
}
void loop()
{
    Diagnostics.Poll();
    delay(2);
}
