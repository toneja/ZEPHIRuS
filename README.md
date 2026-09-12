# ZEPHIRuS Firmware

## Overview
ZEPHIRuS (Zonal Epidemiology Plant Health Inoculum Risk Sampler) is a BLE-enabled environmental data logger and air-sampler controller built on RAK Wireless hardware. It receives external environmental data (typically wind measurements), pairs that with onboard sensors (GPS + BME680 temperature), and drives up to four relay-controlled sampling mechanisms. All activity is logged to files on an SD card.

Key features:
- External environmental input via BLE UART (or Serial1 when compiled for Trisonica mode)
- GPS timestamps and periodic coordinate logging
- Onboard temperature from BME680
- Battery voltage monitoring to prevent sampling when voltage is low
- Four relay outputs (MonkMakes MOSFETTI) with targeting by wind-speed thresholds
- CSV and TXT logs on an SD card
- Optional SSD1306 OLED display for status

## Hardware
- RAK19001 Base Board
- RAK4631 Core Module
- RAK15002 SD Card Module
- RAK12500 GNSS (GPS) Module
- MonkMakes MOSFETTI 4-way Switch
- Generic 0-25V DC Voltage Sensor Module
- RAK1921 OLED Display (optional)
- RAK13002 I/O Module (optional)

## Firmware mode options
- By default the firmware expects environmental data over BLE UART (Bluefruit).
- If compiled with TRISONICA = 1 it instead listens for newline-terminated data on Serial1 from a Trisonica anemometer.

## Configuration (zconfig.txt)
At startup the firmware reads `zconfig.txt` from the SD card and validates it. Required JSON keys:
- "ZEPHIRuS": two-character device ID string (e.g., "AA")
- "windSpeeds": array of 4 numeric values (one per relay)

Example:
{
  "ZEPHIRuS": "AA",
  "windSpeeds": [1.0, 2.0, 3.0, 4.0]
}

The device name advertised over BLE is `ZEPHIRuS-<ID>` (for ID "AA" this becomes `ZEPHIRuS-AA`).

## BLE / External-input format
- Service: BLE UART (Nordic UART via Bluefruit)
- Expected payload (BLE UART or Serial1 for TRISONICA mode) is a comma-separated line with three numeric values:
  windSpeed,windDir,windTemp
  Example: `3.25,180.0,22.8`
- The firmware parses tokens with simple strtok/atof, so avoid extra spaces or missing fields.

Packet size: the firmware uses a small buffer (ENV_BUF_SIZE) — BLE mode defaults to 20 bytes.

External beacon example:
- For an example BLE beacon that publishes anemometer/environment data compatible with ZEPHIRuS, see @toneja/atmos22_logger: https://github.com/toneja/atmos22_logger. That project demonstrates an external anemometer-connected BLE beacon which transmits the comma-separated payload (windSpeed, windDir, windTemp) the sampler expects.

## Sampling logic
Sampling targets are defined as four wind-speed "bins" (the `windSpeeds` array). The firmware assigns each relay to one bin:

- Relays 1-3 (indices 0-2) sample when the measured wind speed falls into the half-open interval [bin_i, bin_{i+1}), i.e., greater than or equal to the bin's value and strictly less than the next bin.
- Relay 4 (index 3) samples when the measured wind speed is greater than or equal to its bin value with no upper bound (i.e., [bin_3, +∞)).

Example with windSpeeds = [1.0, 2.0, 3.0, 4.0]:
- Relay 1: 1.0 ≤ windSpeed < 2.0
- Relay 2: 2.0 ≤ windSpeed < 3.0
- Relay 3: 3.0 ≤ windSpeed < 4.0
- Relay 4: windSpeed ≥ 4.0

When conditions change from one bin to another, the current relay is turned off and the new relay (if any) is enabled. The firmware tracks per-relay sample counts and the duration (seconds) of each sampling event.

## Files on SD card
- CSV files: named `ZEPH<id><NN>.csv` where `<id>` is the two-character ZEPHIRuS ID and `NN` is 00-99.
  Example: `ZEPHAA00.csv`
- Log file: `ZEPH_LOG.txt` (boot, status, and GPS coordinate logs)

CSV header (created on new file):
Date,Time(UTC),Temp(F),WindSpeed(m/s),WindDir,WindTemp(C),Length(s)

Notes:
- The firmware writes a timestamp that includes a comma between date and time (so Date and Time are separate CSV columns).
- When a sampling event begins the firmware writes a row with timestamp, onboard temperature (°F), and the external wind values. When the sampling event ends it appends the sample length (seconds) in the Length column.

## GPS / Timestamping
- GPS is used to provide timestamps in the format `YYYY-MM-DD,HH:MM:SS`.
- The firmware logs coordinates and timestamps to ZEPH_LOG.txt; it also refreshes the GPS timestamp used in CSV rows.
- The firmware attempts to get a 3D fix at startup (with a short break-out to avoid blocking indefinitely when DEBUG is enabled).

## Battery monitoring
- The analog battery monitor reads a scaled voltage and enforces a minimum voltage (`MIN_VBAT` in firmware) to prevent sampler activation when battery is too low.
- If battery voltage is below threshold the sampler will not engage and firmware logs an error.

## Display and user controls
- An optional SSD1306 128x64 display shows:
  - Onboard temperature (°F)
  - Battery voltage
  - Sample counts per channel
  - "WAITING FOR BLUETOOTH" (or "TRISONICA" in serial mode) when no external sensor is connected
- A physical switch (WB_SW1) toggles the display on/off (with debouncing via interrupt).

## LEDs, watchdog, and debug
- Green/Blue LEDs indicate status, sampling activity and errors.
- A software watchdog is enabled and regularly petted to avoid lockups.
- If compiled with DEBUG enabled, the firmware prints detailed status to Serial at 115200 baud.

## Notes & tips
- The `windSpeeds` array must contain exactly 4 values (one per relay). The firmware will fail to start and log a config error if this validation fails.
- BLE connections can be tested with apps such as nRF Connect or Bluefruit Connect by writing the three-value CSV string to the UART characteristic.
- Ensure the SD card is inserted and formatted; missing or unmountable SD card will cause boot to halt with an error.

## Versioning
The firmware embeds a VERSION string (date-style), printed to logs at boot.
