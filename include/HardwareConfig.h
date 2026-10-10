#pragma once

namespace HardwareConfig
{
constexpr int XyeRx = 34, XyeTx = 25, XyeEn = 26;
// Verified cable routing: UART1 module faces ODU, UART2 module faces IDU.
constexpr int OduRx = 35, OduTx = 27, OduEn = 32;
constexpr int IduRx = 33, IduTx = 14, IduEn = 13;
constexpr unsigned Baud = 4800;
// LOW is only safe after bench verification of the actual breakout.
constexpr int ReceiveLevel = 0;
constexpr unsigned StaleMs = 15000;
}
