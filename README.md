# ZEPHIRuS Firmware

## Overview
ZEPHIRuS (Zonal Epidemiology Plant Health Inoculum Risk Sampler) is a BLE-enabled environmental data logger and sampler controller built on RAK Wireless hardware. It aggregates:

- External environmental data received over BLE (e.g., wind conditions)
- GPS position and timestamp

A relay-driven air sampling mechanism is activated during user-defined environmental conditions while all data are logged to a CSV file on an SD card.

## Hardware Requirements
- RAK19001 Base Board
- RAK4631 Core Module
- RAK13007 Relay Module
- RAK15002 SD Card Module
- RAK12500 GNSS (GPS) Module

## Configuration
Configuration is loaded from a `zconfig.txt` file on the SD card at startup.

Example:

```
{
"ZEPHIRuS": "A1",
"windSpeed": "3.00"
}
```

## BLE Interface
- **Device Name:** Configurable via `zconfig.txt` (e.g., `ZEPHIRuS-A1`)
- **Service:** BLE UART (Nordic UART Service via Bluefruit)

## BLE Input
The device receives environmental data from an external sensor node over BLE UART.

You can connect using standard BLE debugging tools such as:
- nRF Connect
- Bluefruit Connect

These apps allow manual data injection over BLE UART.

## Output
Data are written to a CSV file on the SD card (e.g., `ZEPHA100.csv`).

Each record includes:
- Timestamp (GPS-derived)
- External environmental data (BLE input)
  - Wind speed
  - Wind temperature
  - Maximum wind speed during sampling event
- Length of the sampling event

---
