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
*   SDCARD: RAK WIRELESS 15002                                          *
*   GPS:    RAK WIRELESS 12500                                          *
*   TEMP:   RAK WIRELESS 1906                                           *
*   RELAY:  MonkMakes MOSFETTI 4-way Switch                             *
*   VBAT:   Generic 0-25V DC Voltage Sensor Module                      *
*   OLED:   RAK WIRELESS 1921 (Optional)                                *
*   IO:     RAK WIRELESS 13002 (Optional)                               *
************************************************************************/

#include <bluefruit.h>
#include <Wire.h>
#include <SparkFun_u-blox_GNSS_Arduino_Library.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BME680.h>
#include <Adafruit_SleepyDog.h>
#include <ArduinoJson.h>
#include <U8g2lib.h>
#include "SD.h"

#define DEBUG 1
#define VERSION 20260720  // Date last modified

// DISPLAY
U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R2);  // R2 = Rotate display 180°
char displayMsg[32];
#define EMOJI_WIDTH 8
#define EMOJI_HEIGHT 8
const unsigned char smiley[] PROGMEM = {
  0x3C,
  0x42,
  0xA5,
  0x81,
  0xA5,
  0x99,
  0x42,
  0x3C
};
const unsigned char frowny[] PROGMEM = {
  0x3C,
  0x42,
  0xA5,
  0x81,
  0x99,
  0xA5,
  0x42,
  0x3C
};

// BLUETOOTH
BLEDis bledis;
BLEUart bleuart;
char bleName[12];
const char* zephID;
#define BLE_BUF_SIZE 20  // default BLEUart packet size
char bleMsg[BLE_BUF_SIZE];
char central_name[32];

// BLEUart Sensor Data
struct EnvironmentData {
  float windSpeed;
  float windDir;
  float windTemp;
  // float leafWetness;
};
EnvironmentData observed = {};
EnvironmentData pendingData = {};
volatile bool newDataAvailable = false;
float maxWindSpeed = 0;

// Log files
File csvFile;
File logFile;

// GPS: position + timestamp
SFE_UBLOX_GNSS g_myGNSS;
char timestamp[20];
char gpsLoc[55];
unsigned long lastFix = 0;

// TEMPERATURE
Adafruit_BME680 bme;

// RELAY: MOSFETTI 4-way switch
#define RELAY_COUNT 4
#define RELAY_PIN1 WB_IO1
#define RELAY_PIN2 WB_IO3
#define RELAY_PIN3 WB_IO4
#define RELAY_PIN4 WB_IO5
const uint8_t relayPins[] = { RELAY_PIN1, RELAY_PIN2, RELAY_PIN3, RELAY_PIN4 };
uint8_t samplerActive = 0;    // 0 = All relays OFF
unsigned long startTime = 0;  // milliseconds
uint16_t sampleLength = 0;    // seconds
uint16_t sampleCount[RELAY_COUNT] = {};
uint8_t targeted[RELAY_COUNT] = {};

// WATCHDOG: Non-blocking timer
unsigned long lastWatchdogPet = 0;
#define WATCHDOG_INTERVAL 5000

// VBAT: monitor battery voltage
#define ANALOG_PIN1 WB_A1  // AIN1 pin
#define MIN_VBAT 10.5
#define MAX_COUNTS 4095.0  // 12-bits
#define MAX_VINPUT 3.0
#define DIVIDER_RATIO 5.0
uint16_t voltMagic = MAX_COUNTS / MAX_VINPUT / DIVIDER_RATIO;
float voltage;

void setup() {
  // I2C
  sensor_init();
  // DISPLAY
  oled_init();
  // LEDs
  led_init();
#if DEBUG
  // SERIAL
  serial_init();
#endif
  // VBAT
  vbat_init();
  // TRISONICA - testing
  sonic_init();
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
  oled_update();
  logFile.println("BOOT SUCCESSFUL.");
  logFile.flush();
}

void loop() {
  // Dump Trisonica data to Serial
#if DEBUG
  while (Serial1.available()) { Serial.write(Serial1.read()); }
#endif
  // Process new BLE data immediately
  if (newDataAvailable) {
    newDataAvailable = false;
    // Check battery level before activating sampler
    if (vbat_get()) {
      // Flash green LED while handling BLEUart data
      digitalWrite(LED_GREEN, HIGH);
      // Make a copy to avoid strtok corruption issues
      char msgCopy[BLE_BUF_SIZE];
      strncpy(msgCopy, bleMsg, sizeof(msgCopy) - 1);
      char* token;
      token = strtok(msgCopy, ",");
      if (token == NULL || strlen(token) <= 0) { return; }
      pendingData.windSpeed = atof(token);
      token = strtok(NULL, ",");
      if (token == NULL || strlen(token) <= 0) { return; }
      pendingData.windDir = atof(token);
      token = strtok(NULL, ",");
      if (token == NULL || strlen(token) <= 0) { return; }
      pendingData.windTemp = atof(token);
      observed = pendingData;
      if (observed.windSpeed > maxWindSpeed) { maxWindSpeed = observed.windSpeed; }
      bme680_get();  // Onboard temperature
#if DEBUG
      Serial.printf("WindSpeed: %.2f, WindDir: %.1f, WindTemp: %.1f\n", observed.windSpeed, observed.windDir, observed.windTemp);
#endif
      // Handle relay and perform all I/O here
      relay_handler();
      // LED off, when sampler inactive
      if (!samplerActive) {
        delay(50);
        digitalWrite(LED_GREEN, LOW);
      }
    } else {
      // Force sampler to shutdown if it is running
      if (samplerActive) { disable_relay(true); }
    }
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

void sensor_init(void) {
  // I2C
  pinMode(WB_IO2, OUTPUT);
  digitalWrite(WB_IO2, HIGH);
  delay(1000);
  Wire.begin();
  delay(1000);  // give em a sec to wake up
}

void oled_init(void) {
  u8g2.begin();
  u8g2.setContrast(0);  // Minimum brightness [0-255]
  u8g2.setFont(u8g2_font_6x10_tf);
  // Display the logo
  u8g2.clearBuffer();
  draw_logo(32, 0);
  u8g2.sendBuffer();
  delay(3000);
  // Welcome message
  u8g2.clearBuffer();
  u8g2.drawStr(16, 15, "ZEPHIRuS SAMPLER");
  u8g2.drawStr(16, 30, "PLEASE WAIT.....");
  u8g2.drawXBM(60, 45, EMOJI_WIDTH, EMOJI_HEIGHT, smiley);
  u8g2.sendBuffer();
}

void draw_logo(uint8_t x, uint8_t y) {
  u8g2.drawCircle(x + 32, y + 32, 31);  // Outer circle
  u8g2.drawCircle(x + 32, y + 32, 30);  // Thicken the border
  // Letter Z
  u8g2.drawLine(x + 18, y + 18, x + 46, y + 18);  // top
  u8g2.drawLine(x + 46, y + 18, x + 18, y + 46);  // diagonal
  u8g2.drawLine(x + 18, y + 46, x + 46, y + 46);  // bottom
}

void oled_update() {
  u8g2.clearBuffer();
  // Device name + Onboard temperature
  memset(displayMsg, 0, sizeof(displayMsg));
  snprintf(displayMsg, sizeof(displayMsg), "%s   %.1fF", bleName, (bme.temperature * 1.8) + 32);
  u8g2.drawStr((u8g2.getDisplayWidth() - u8g2.getStrWidth(displayMsg)) / 2, 10, displayMsg);
  if (Bluefruit.connected()) {
    // FW Version
    memset(displayMsg, 0, sizeof(displayMsg));
    snprintf(displayMsg, sizeof(displayMsg), "CENTRAL  %s", central_name);
    u8g2.drawStr(0, 20, displayMsg);
    // Battery voltage
    memset(displayMsg, 0, sizeof(displayMsg));
    snprintf(displayMsg, sizeof(displayMsg), "BATTERY  %.2fV", voltage);
    u8g2.drawStr(0, 30, displayMsg);
    // Sampler status
    memset(displayMsg, 0, sizeof(displayMsg));
    snprintf(displayMsg, sizeof(displayMsg), "SAMPLER  %s", samplerActive ? "ACTIVE" : "INACTIVE");
    u8g2.drawStr(0, 40, displayMsg);
    // Sample count
    memset(displayMsg, 0, sizeof(displayMsg));
    snprintf(displayMsg, sizeof(displayMsg), "SAMPLES  A=%d B=%d", sampleCount[0], sampleCount[1]);
    u8g2.drawStr(0, 50, displayMsg);
    memset(displayMsg, 0, sizeof(displayMsg));
    snprintf(displayMsg, sizeof(displayMsg), "COUNTED  C=%d D=%d", sampleCount[2], sampleCount[3]);
    u8g2.drawStr(0, 60, displayMsg);
  } else {
    u8g2.drawStr(25, 25, "WAITING FOR A");
    u8g2.drawStr(4, 35, "BLUETOOTH CONNECTION");
  }
  u8g2.sendBuffer();
}

void led_init(void) {
  pinMode(LED_GREEN, OUTPUT);
  pinMode(LED_BLUE, OUTPUT);
  led_loop(10);
}

void led_loop(int count) {
  for (uint8_t i = 0; i < count * 2; i++) {
    digitalToggle(LED_GREEN);
    delay(100);
    digitalToggle(LED_BLUE);
    delay(100);
  }
}

#if DEBUG
void serial_init(void) {
  Serial.begin(115200);
  delay(2000);
  // while (!Serial) { delay(100); }
  Serial.printf("ZEPHIRuS - SAMPLER VERSION %d\n", VERSION);
}
#endif

void error(const char* err, const char* errMsg) {
  digitalWrite(LED_GREEN, HIGH);
  if (csvFile) { csvFile.close(); }
  if (logFile) {
    logFile.println(errMsg);
    logFile.flush();
    logFile.close();
  }
  u8g2.clearBuffer();
  // Center the text
  u8g2.drawStr(46, 25, "ERROR:");
  u8g2.drawStr((u8g2.getDisplayWidth() - u8g2.getStrWidth(err)) / 2, 35, err);
  u8g2.drawXBM(60, 45, EMOJI_WIDTH, EMOJI_HEIGHT, frowny);
  u8g2.sendBuffer();
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
    Serial.printf("ERROR: %s\n", errMsg);
#endif
  }
}

void vbat_init(void) {
  pinMode(ANALOG_PIN1, INPUT);
  analogReference(AR_INTERNAL_3_0);  // 3.0 Volts
  analogReadResolution(12);          // 12-bit resolution
  if (!vbat_get()) { error("BATTERY LEVEL", "Battery voltage is too low to activate sampler mechanism."); }
}

bool vbat_get(void) {
  int rawValue = analogRead(ANALOG_PIN1);
  voltage = rawValue / voltMagic;
#if DEBUG
  Serial.printf("Raw vbat input: %d | Voltage: %.2f\n", rawValue, voltage);
#else
  // Don't draw the battery down below safe threshold
  if (voltage <= MIN_VBAT) { return false; }
#endif
  return true;
}

void sonic_init(void) {
  Serial1.begin(115200);
#if DEBUG
  Serial.println("Listening for Trisonica data on Serial1.");
#endif
}

void relay_init(void) {
  pinMode(RELAY_PIN1, OUTPUT);
  pinMode(RELAY_PIN2, OUTPUT);
  pinMode(RELAY_PIN3, OUTPUT);
  pinMode(RELAY_PIN4, OUTPUT);
  digitalWrite(RELAY_PIN1, LOW);
  digitalWrite(RELAY_PIN2, LOW);
  digitalWrite(RELAY_PIN3, LOW);
  digitalWrite(RELAY_PIN4, LOW);
}

void sd_init(void) {
  // Check if card is inserted
  pinMode(WB_IO6, INPUT_PULLUP);
  if (!digitalRead(WB_IO6) == LOW) { error("SDCARD MISSING", "No SD Card inserted."); }
  if (!SD.begin()) { error("SDCARD FS", "Unable to mount the SD Card."); }
#if DEBUG
  Serial.println("SD Card mounted.");
#endif
  // Load configuration
  load_config();
  char csvFilename[12 + 1];
  for (uint8_t i = 0; i <= 99; i++) {
    snprintf(csvFilename, sizeof(csvFilename), "ZEPH%s%02d.csv", zephID, i);
    if (!SD.exists(csvFilename)) { break; }
  }
  csvFile = SD.open(csvFilename, FILE_WRITE);
  if (!csvFile) { error("CSV FILE", "Unable to create CSV file."); }
  if (csvFile.size() == 0) {
    csvFile.println("Date,Time,Temp,WindSpeed,WindDir,WindTemp,MaxSpeed,Length");
    csvFile.flush();
  }
  logFile = SD.open("ZEPHIRuS.txt", FILE_WRITE);
  if (!logFile) { error("LOG FILE", "Unable to create LOG file."); }
  logFile.printf("==========================================\n");
  logFile.printf("%s VERSION %d\nBattery voltage: %.2f\n", bleName, VERSION, voltage);
  logFile.printf("Targeted wind speeds: %d, %d, %d, %d m/s\n", targeted[0], targeted[1], targeted[2], targeted[3]);
  logFile.flush();
}

void load_config(void) {
  File zfile = SD.open("zconfig.txt");
  if (!zfile) { error("CONFIG FILE", "Unable to read zconfig.txt."); }
  JsonDocument doc;
  DeserializationError jsonError = deserializeJson(doc, zfile);
  zfile.close();
  if (jsonError) { error("JSON CONFIG", "Unable to read json configuration."); }
  if (!doc.containsKey("ZEPHIRuS") || strlen(doc["ZEPHIRuS"]) != 2) { error("ZEPHIRuS ID", "Config error: 'ZEPHIRuS'"); }
  if (!doc.containsKey("windSpeeds") || doc["windSpeeds"].size() != RELAY_COUNT) { error("WINDSPEEDS", "Config error: 'windSpeeds'"); }
  zephID = doc["ZEPHIRuS"];
  snprintf(bleName, sizeof(bleName), "ZEPHIRuS-%s", zephID);
  JsonArray array = doc["windSpeeds"];
  uint8_t i = 0;
  for (JsonVariant value : array) {
    targeted[i] = value.as<int>();
#if DEBUG
    Serial.printf("Targeted wind speed [%d]: %d m/s\n", i + 1, targeted[i]);
#endif
    i++;
  }
}

void gps_init(void) {
  if (!g_myGNSS.begin()) { error("GPS", "GPS not found."); }
  g_myGNSS.setI2COutput(COM_TYPE_UBX);
  g_myGNSS.saveConfigSelective(VAL_CFG_SUBSEC_IOPORT);
  // Wait on the GPS fix for accurate timestamps
#if DEBUG
  Serial.print("Searching for GPS...");
  uint16_t fixTime = 0;
#endif
  while (g_myGNSS.getFixType() < 3) {
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
  Serial.printf("GPS fix acquired in %d seconds.\n", fixTime);
#endif
  // log timestamp + coordinates to boot log
  gps_get();
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
  logFile.printf("%s\n%s\n", timestamp, gpsLoc);
  logFile.flush();
#if DEBUG
  Serial.printf("%s\n%s\n", timestamp, gpsLoc);
#endif
}

void bme680_init(void) {
  if (!bme.begin(0x76)) { error("TEMP SENSOR", "BME680 not found."); }
  bme.setTemperatureOversampling(BME680_OS_8X);
  // save power
  bme.setGasHeater(0, 0);
  bme680_get();
}

void bme680_get(void) {
  bme.performReading();
#if DEBUG
  Serial.printf("Onboard temperature %.1f°F\n", (bme.temperature * 1.8) + 32);
#endif
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
  memset(central_name, 0, sizeof(central_name));
  connection->getPeerName(central_name, sizeof(central_name));
  Serial.printf("Connected to %s\n", central_name);
#endif
  oled_update();
}

void disconnect_callback(uint16_t conn_handle, uint8_t reason) {
#if DEBUG
  (void)conn_handle;
  (void)reason;
  Serial.printf("Disconnected, reason = %#X\n", reason);
#endif
  // Clear observed data
  observed = {};
  // Force sampler to shutdown if it is running
  if (samplerActive) { disable_relay(true); }
  oled_update();
}

void bleuart_rx_callback(uint16_t conn_handle) {
  // Read BLEUart data
  uint8_t len = bleuart.available();
  if (len < 0) { return; }
  memset(bleMsg, 0, BLE_BUF_SIZE);  // clear the msg buffer
  uint8_t i = 0;
  while (bleuart.available()) { bleMsg[i++] = bleuart.read(); }
  bleMsg[len] = '\0';
  newDataAvailable = true;
}

void relay_handler(void) {
  for (uint8_t relay = 0; relay < RELAY_COUNT; relay++) {
    if (sampling_conditions(relay)) {
      if (samplerActive && samplerActive != relayPins[relay]) { disable_relay(false); }
      if (!samplerActive) { enable_relay(relay); }
    } else {
      if (samplerActive == relayPins[relay]) { disable_relay(false); }
    }
  }
  // Refresh display data
  oled_update();
}

bool sampling_conditions(uint8_t relay) {
  if (relay < RELAY_COUNT - 1) {
    // upper bound is the next sampling target
    return observed.windSpeed >= targeted[relay] && observed.windSpeed < targeted[relay + 1];
  }
  // final sampler in the relay samples with no upper bound
  return observed.windSpeed >= targeted[relay];
}

void enable_relay(uint8_t relay) {
  samplerActive = relayPins[relay];
  startTime = millis();
#if DEBUG
  Serial.printf("Sampler [%d] Active ... \n", relay + 1);
  // Loop LEDs
  led_loop(2);
#else
  // RELAY ON
  digitalWrite(samplerActive, HIGH);
#endif
  sampleCount[relay]++;
  log_data();
}

void disable_relay(bool override) {
  sampleLength = (millis() - startTime) / 1000;
#if DEBUG
  Serial.printf(" ... Sampling complete after %d seconds.\n", sampleLength);
  Serial.printf("Max wind speed: %.2f\n", maxWindSpeed);
  // Loop LEDs
  led_loop(2);
#else
  // Relay OFF
  digitalWrite(samplerActive, LOW);
#endif
  if (override) { digitalWrite(LED_GREEN, LOW); }
  samplerActive = 0;
  log_data();
  maxWindSpeed = 0;
}

void log_data(void) {
  if (samplerActive) {
    gps_gettime();
    csvFile.printf("%s,%.1f,%.2f,%.1f,%.1f",
                   timestamp,
                   (bme.temperature * 1.8) + 32,
                   observed.windSpeed,
                   observed.windDir,
                   observed.windTemp);
  } else {
    csvFile.printf(",%.2f,%d\n",
                   maxWindSpeed,
                   sampleLength);
  }
  csvFile.flush();
}
