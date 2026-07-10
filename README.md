# ZEPHIRuS Firmware

## Overview
ZEPHIRuS (Zonal Epidemiology Plant Health Inoculum Risk Sampler) is a BLE-enabled environmental data logger and air sampler controller built on RAK Wireless and other hardware. It aggregates:

- External environmental data received over BLE (e.g., wind conditions)
- GPS position and timestamp

Up to 4 relay-driven air sampling mechanisms are activated during user-defined environmental conditions while all data are logged to CSV and TXT files on an SD card.

## Hardware Requirements
- RAK19001 Base Board
- RAK4631 Core Module
- RAK15002 SD Card Module
- RAK12500 GNSS (GPS) Module
- MonkMakes MOSFETTI 4-way Switch
- Generic 0-25V DC Voltage Sensor Module
- RAK1921 OLED Display (Optional)
- RAK13002 I/O Module (Optional)

## Configuration
Configuration is loaded from a `zconfig.txt` file on the SD card at startup.

Example:

```
{
"ZEPHIRuS": "AA",
"windSpeeds": [1, 2, 3, 4]
}
```

## BLE Interface
- **Device Name:** Configurable via `zconfig.txt` (e.g., `ZEPHIRuS-AA`)
- **Service:** BLE UART (Nordic UART Service via Bluefruit)

## BLE Input
The device receives environmental data from an external sensor node over BLE UART.

You can connect using standard BLE debugging tools such as:
- nRF Connect
- Bluefruit Connect

These apps allow manual data injection over BLE UART.

## Output
Data are written to CSV and TXT files on the SD card (e.g., `ZEPHAA[00-99].csv` and `ZEPHIRuS.txt`).

Each CSV record includes:
- Timestamp (GPS-derived)
- External environmental data (BLE input)
  - Wind speed
  - Wind direction
  - Wind temperature
- Length of the sampling event

---
