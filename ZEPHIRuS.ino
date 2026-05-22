/************************************************************************
* ZEPHIRuS: Zonal Epidemiology Plant Health Inoculum Risk Sampler       *
*                                                                       *
* Copyright (c) 2026 Jason Toney                                        *
*                                                                       *
* This program is free software: you can redistribute it and/or modify  *
* it under the terms of the GNU General Public License as published by  *
* the Free Software Foundation, either version 3 of the License, or     *
* (at your option) any later version.                                   *
*                                                                       *
* This program is distributed in the hope that it will be useful,       *
* but WITHOUT ANY WARRANTY; without even the implied warranty of        *
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
* GNU General Public License for more details.                          *
*                                                                       *
* You should have received a copy of the GNU General Public License     *
* along with this program. If not, see <https://www.gnu.org/licenses/>. *
*                                                                       *
* Expected hardware components:                                         *
*   BASE:   RAK WIRELESS 19001                                          *
*   CORE:   RAK WIRELESS 4631                                           *
*   RELAY:  RAK WIRELESS 13007                                          *
*   SDCARD: RAK WIRELESS 15002                                          *
*   GPS:    RAK WIRELESS 12500                                          *
*   TEMP:   RAK WIRELESS 1906                                           *
************************************************************************/

#include <bluefruit.h>
#include <Wire.h>
#include <SparkFun_u-blox_GNSS_Arduino_Library.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BME680.h>
#include <Adafruit_SleepyDog.h>
#include <ArduinoJson.h>
#include "SD.h"

#define DEBUG 1

// BLUETOOTH
BLEDis bledis;
BLEUart bleuart;
char bleName[12];
const char* zephName;
#define BLE_BUF_SIZE 20  // default BLEUart packet size
char bleMsg[BLE_BUF_SIZE];

// BLEUart Sensor Data
struct EnvironmentData {
  float windSpeed;
  float windTemp;
  // float leafWetness;
};
EnvironmentData observed = {};
EnvironmentData targeted = {};
EnvironmentData pendingData = {};
volatile bool newDataAvailable = false;
float maxWindSpeed = 0;

// Log files
File csvFile;
File logFile;
char msgBuf[128];

// GPS: position + timestamp
SFE_UBLOX_GNSS g_myGNSS;
char timestamp[20];
char gpsLoc[55];
unsigned long lastFix = 0;

// TEMPERATURE
Adafruit_BME680 bme;

// RELAY: timer
bool samplerActive = false;
unsigned long startTime = 0;  // milliseconds
uint16_t sampleLength = 0;    // seconds
#if DEBUG
uint16_t sampleCount = 0;
#endif

// WATCHDOG: Non-blocking timer
unsigned long lastWatchdogPet = 0;
#define WATCHDOG_INTERVAL 5000

void setup() {
#if DEBUG
  // SERIAL
  serial_init();
#endif
  // LEDs
  led_init();
  // I2C
  sensor_init();
  // RELAY
  relay_init();
  // SDCARD
  sd_init();
  // GPS
  gps_init();
  // TEMPERATURE
  bme680_init();
  // BLUETOOTH
  ble_init();
  // WATCHDOG
  Watchdog.enable(10000);
  lastWatchdogPet = millis();
  // ALL CLEAR
  logFile.println("BOOT SUCCESSFUL.");
  logFile.flush();
}

void loop() {
  // Process new BLE data immediately
  if (newDataAvailable) {
    newDataAvailable = false;
    // Make a copy to avoid strtok corruption issues
    char msgCopy[BLE_BUF_SIZE];
    strncpy(msgCopy, bleMsg, sizeof(msgCopy) - 1);
    char* token;
    token = strtok(msgCopy, ",");
    if (token == NULL || strlen(token) <= 0) { return; }
    pendingData.windSpeed = atof(token);
    token = strtok(NULL, ",");
    if (token == NULL || strlen(token) <= 0) { return; }
    pendingData.windTemp = atof(token);
    observed = pendingData;
    if (observed.windSpeed > maxWindSpeed) { maxWindSpeed = observed.windSpeed; }
#if DEBUG
    Serial.print("WindSpeed: ");
    Serial.print(observed.windSpeed);
    Serial.print(", WindTemp: ");
    Serial.println(observed.windTemp);
#endif
    // Handle relay and perform all heavy I/O here (GPS, BME680, SD card)
    relay_handler(false);
  }
  // Only pet watchdog if 5 seconds have elapsed
  unsigned long now = millis();
  if (now - lastWatchdogPet >= WATCHDOG_INTERVAL) {
    Watchdog.reset();
    lastWatchdogPet = now;
  }
  // Log GPS coordinates every 12 hours
  if (now - lastFix >= 12 * 60 * 60 * 1000) { gps_get(); }
}

#if DEBUG
void serial_init(void) {
  Serial.begin(115200);
  delay(2000);
  // while (!Serial) { delay(100); }
  Serial.println("ZEPHIRuS - PERIPHERAL: SAMPLER");
}
#endif

void led_init(void) {
  pinMode(LED_GREEN, OUTPUT);
  pinMode(LED_BLUE, OUTPUT);
  for (uint8_t i = 0; i < 20; i++) {
    digitalToggle(LED_GREEN);
    delay(100);
    digitalToggle(LED_BLUE);
    delay(100);
  }
}

void led_error(const char* errMsg) {
  digitalWrite(LED_GREEN, HIGH);
  if (csvFile) { csvFile.close(); }
  if (logFile) {
    logFile.println(errMsg);
    logFile.flush();
    logFile.close();
  }
  while (1) {
    digitalWrite(LED_BLUE, HIGH);
    delay(333);
    digitalWrite(LED_BLUE, LOW);
    delay(333);
    digitalWrite(LED_BLUE, HIGH);
    delay(333);
    digitalWrite(LED_BLUE, LOW);
    delay(4000);
#if DEBUG
    Serial.print("ERROR: ");
    Serial.println(errMsg);
#endif
  }
}

void sensor_init(void) {
  // I2C
  pinMode(WB_IO2, OUTPUT);
  digitalWrite(WB_IO2, HIGH);
  delay(1000);
  Wire.begin();
  delay(1000);  // give em a sec to wake up
}

void relay_init(void) {
  pinMode(WB_IO4, OUTPUT);
  digitalWrite(WB_IO4, LOW);
}

void sd_init(void) {
  if (!SD.begin()) { led_error("No SD Card found."); }
#if DEBUG
  Serial.println("SD Card mounted.\n");
#endif
  // Load configuration
  load_config();
  char csvFilename[12 + 1];
  for (uint8_t i = 0; i <= 99; i++) {
    snprintf(csvFilename, sizeof(csvFilename), "ZEPH%s%02d.csv", zephName, i);
    if (!SD.exists(csvFilename)) {
      break;
    }
  }
  csvFile = SD.open(csvFilename, FILE_WRITE);
  if (!csvFile) { led_error("Unable to create CSV file."); }
  if (csvFile.size() == 0) {
    csvFile.println("Date,Time,Temperature,Humidity,WindSpeed,WindTemp,MaxSpeed,Length");
    csvFile.flush();
  }
  logFile = SD.open("ZEPHIRuS.txt", FILE_WRITE);
  if (!logFile) { led_error("Unable to create LOG file."); }
  logFile.println("==========================================\nZEPHIRuS\n");
}

void load_config(void) {
  File zfile = SD.open("zconfig.txt");
  if (!zfile) { led_error("Unable to read zconfig.txt."); }
  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, zfile);
  zfile.close();
  if (error) { led_error("Unable to read json configuration."); }
  if ((!doc.containsKey("ZEPHIRuS") || strlen(doc["ZEPHIRuS"]) != 2)) { led_error("Config error: 'ZEPHIRuS'"); }
  if (!doc.containsKey("windSpeed")) { led_error("Config error: 'windSpeed'"); }
  zephName = doc["ZEPHIRuS"];
  snprintf(bleName, sizeof(bleName), "ZEPHIRuS-%s", zephName);
  targeted.windSpeed = doc["windSpeed"];
}

void gps_init(void) {
  if (!g_myGNSS.begin()) { led_error("GPS not found."); }
  g_myGNSS.setI2COutput(COM_TYPE_UBX);
  g_myGNSS.saveConfigSelective(VAL_CFG_SUBSEC_IOPORT);
  // Wait on the GPS fix for accurate timestamps
#if DEBUG
  Serial.print("Searching for GPS...");
  uint16_t fixTime = 0;
#endif
  while (g_myGNSS.getFixType() == 0) {
    digitalToggle(LED_GREEN);
    digitalToggle(LED_BLUE);
    delay(1000);
#if DEBUG
    Serial.print(".");
    fixTime++;
    if (fixTime == 30) { break; }  // don't loop forever
#endif
  }
  // make sure LEDs are off
  digitalWrite(LED_GREEN, LOW);
  digitalWrite(LED_BLUE, LOW);
#if DEBUG
  Serial.print("GPS fix acquired in ");
  Serial.print(fixTime);
  Serial.println(" seconds.");
#endif
  // log timestamp + coordinates to boot log
  gps_get();
}

void bme680_init(void) {
  if (!bme.begin(0x76)) { led_error("BME680 not found."); }
  bme.setTemperatureOversampling(BME680_OS_8X);
  bme.setHumidityOversampling(BME680_OS_2X);
  // save power
  bme.setGasHeater(0, 0);
}

void ble_init(void) {
  Bluefruit.begin(1, 0);
  Bluefruit.setTxPower(4);  // Check bluefruit.h for supported values
  Bluefruit.setName(bleName);
  Bluefruit.Periph.setConnectCallback(connect_callback);
  Bluefruit.Periph.setDisconnectCallback(disconnect_callback);
  bleuart.setRxCallback(bleuart_rx_callback);
  // Device Info
  bledis.setManufacturer("Mahaffee Lab");
  bledis.setModel(bleName);
  bledis.begin();
  bleuart.begin();
  startAdv();
}

void startAdv(void) {
  Bluefruit.Advertising.addFlags(BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE);
  Bluefruit.Advertising.addTxPower();
  Bluefruit.Advertising.addService(bleuart);
  Bluefruit.ScanResponse.addName();
  Bluefruit.Advertising.restartOnDisconnect(true);
  Bluefruit.Advertising.setInterval(32, 244);
  Bluefruit.Advertising.setFastTimeout(30);
  Bluefruit.Advertising.start(0);
}

void connect_callback(uint16_t conn_handle) {
#if DEBUG
  BLEConnection* connection = Bluefruit.Connection(conn_handle);
  char central_name[32] = { 0 };
  connection->getPeerName(central_name, sizeof(central_name));
  Serial.print("Connected to ");
  Serial.println(central_name);
#endif
}

void disconnect_callback(uint16_t conn_handle, uint8_t reason) {
#if DEBUG
  (void)conn_handle;
  (void)reason;
  Serial.print("Disconnected, reason = 0x");
  Serial.println(reason, HEX);
#endif
  // Clear observed data
  observed = {};
  // Force sampler to shutdown if it is running
  relay_handler(true);
}

void bleuart_rx_callback(uint16_t conn_handle) {
  // Flash green LED while receiving BLEUart data
  if (!samplerActive) { digitalWrite(LED_GREEN, HIGH); }
  // Read BLEUart data
  uint8_t len = bleuart.available();
  if (len < 0) { return; }
  memset(bleMsg, 0, BLE_BUF_SIZE);  // clear the msg buffer
  uint8_t i = 0;
  while (bleuart.available()) { bleMsg[i++] = bleuart.read(); }
  bleMsg[len] = '\0';
  newDataAvailable = true;
  if (!samplerActive) { digitalWrite(LED_GREEN, LOW); }
}

void relay_handler(bool override) {
  if (!samplerActive && sampling_conditions()) {
    // LED on, when sampler active
    digitalWrite(LED_GREEN, HIGH);
    samplerActive = true;
    startTime = millis();
    // Relay ON
    digitalWrite(WB_IO4, HIGH);
    // Onboard temperature/humidity
    bme680_get();
    log_data();
#if DEBUG
    sampleCount++;
    Serial.println("Sampler Active ... ");
#endif
  }
  if (samplerActive && (!sampling_conditions() || override)) {
    samplerActive = false;
    // Relay OFF
    digitalWrite(WB_IO4, LOW);
    sampleLength = (millis() - startTime) / 1000;
    log_data();
#if DEBUG
    Serial.print(" ... Sampling complete after ");
    Serial.print(sampleLength);
    Serial.println(" seconds.");
    Serial.print("Max wind speed: ");
    Serial.println(maxWindSpeed);
    Serial.print("Sample count: ");
    Serial.println(sampleCount);
#endif
    maxWindSpeed = 0;
    // LED off, when sampler inactive
    digitalWrite(LED_GREEN, LOW);
  }
}

bool sampling_conditions(void) {
  return observed.windSpeed >= targeted.windSpeed;
}

void gps_gettime(void) {
  snprintf(timestamp,
           sizeof(timestamp),
           "%d-%02d-%02d,%02d:%02d:%02d",
           g_myGNSS.getYear(), g_myGNSS.getMonth(), g_myGNSS.getDay(),
           g_myGNSS.getHour(), g_myGNSS.getMinute(), g_myGNSS.getSecond());
}

void gps_get(void) {
  gps_gettime();
  lastFix = millis();
  snprintf(gpsLoc,
           sizeof(gpsLoc),
           "Lat: %.7f Long: %.7f °, Alt: %d m",
           g_myGNSS.getLatitude() / 10000000.0,
           g_myGNSS.getLongitude() / 10000000.0,
           g_myGNSS.getAltitude() / 1000);
  g_myGNSS.powerSaveMode();
  logFile.println(timestamp);
  logFile.println(gpsLoc);
  logFile.flush();
#if DEBUG
  Serial.println(timestamp);
  Serial.println(gpsLoc);
#endif
}

void bme680_get(void) {
  bme.performReading();
#if DEBUG
  Serial.print("Temperature = ");
  Serial.print(bme.temperature);
  Serial.print(" °C, Humidity = ");
  Serial.print(bme.humidity);
  Serial.println("%");
#endif
}

void log_data(void) {
  if (samplerActive) {
    gps_gettime();
    snprintf(msgBuf,
             sizeof(msgBuf),
             "%s,%.1f,%.1f,%.2f,%.2f",
             timestamp,
             bme.temperature,
             bme.humidity,
             observed.windSpeed,
             observed.windTemp);
  } else {
    snprintf(msgBuf,
             sizeof(msgBuf),
             ",%.2f,%d\n",
             maxWindSpeed,
             sampleLength);
  }
  csvFile.print(msgBuf);
  csvFile.flush();
}
