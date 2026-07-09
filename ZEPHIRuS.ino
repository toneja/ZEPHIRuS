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
*   OLED:   RAK WIRELESS 1921 (Optional)                                *
*   VBAT:   Generic 0-25V DC Voltage Sensor Module                      *
************************************************************************/

#include <bluefruit.h>
#include <Wire.h>
#include <SparkFun_u-blox_GNSS_Arduino_Library.h>
#include <Adafruit_SleepyDog.h>
#include <ArduinoJson.h>
#include <U8g2lib.h>
#include "SD.h"

#define DEBUG 0
#define VERSION 20260704  // Date last modified

// DISPLAY
U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R2);  // R2 = Rotate display 180°
bool displayActive;
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
const char* zephName;
#define BLE_BUF_SIZE 20  // default BLEUart packet size
char bleMsg[BLE_BUF_SIZE];

// BLEUart Sensor Data
struct EnvironmentData {
  float windSpeed;
  float windDir;
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

// RELAY: timer
bool samplerActive = false;
unsigned long startTime = 0;  // milliseconds
uint16_t sampleLength = 0;    // seconds
uint16_t sampleCount = 0;

// WATCHDOG: Non-blocking timer
unsigned long lastWatchdogPet = 0;
#define WATCHDOG_INTERVAL 5000

// VBAT: monitor battery voltage
#define ANALOG_PIN1 WB_A1  // AIN1 pin
#define MIN_VBAT 10.5
#define MAX_COUNTS 4095.0  // 12-bits
#define MAX_VINPUT 3.0
#define DIVIDER_RATIO 5.0
float voltMagic = MAX_COUNTS / MAX_VINPUT / DIVIDER_RATIO;
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
  // RELAY
  relay_init();
  // SDCARD
  sd_init();
  // GPS
  gps_init();
  // BLUETOOTH
  ble_init();
  // WATCHDOG
  Watchdog.enable(10000);
  lastWatchdogPet = millis();
  // ALL CLEAR
  if (displayActive) {
    if (Bluefruit.connected()) {
      oled_update(true);
    } else {
      oled_update(false);
    }
  }
  logFile.println("BOOT SUCCESSFUL.");
  logFile.flush();
}

void loop() {
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
#if DEBUG
      Serial.printf("WindSpeed: %.2f, WindDir: %.1f, WindTemp: %.1f\n", observed.windSpeed, observed.windDir, observed.windTemp);
#endif
      // Handle relay and perform all I/O here
      relay_handler(false);
      // LED off, when sampler inactive
      if (!samplerActive) {
        delay(50);
        digitalWrite(LED_GREEN, LOW);
      }
    } else {
      // Force sampler to shutdown if it is running
      if (samplerActive) { relay_handler(true); }
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
  if (!u8g2.begin()) {
    displayActive = false;
  } else {
    displayActive = true;
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
}

void draw_logo(uint8_t x, uint8_t y) {
  u8g2.drawCircle(x + 32, y + 32, 31);  // Outer circle
  u8g2.drawCircle(x + 32, y + 32, 30);  // Thicken the border
  // Letter Z
  u8g2.drawLine(x + 18, y + 18, x + 46, y + 18);  // top
  u8g2.drawLine(x + 46, y + 18, x + 18, y + 46);  // diagonal
  u8g2.drawLine(x + 18, y + 46, x + 46, y + 46);  // bottom
}

void oled_update(bool connected) {
  u8g2.clearBuffer();
  // Device name
  u8g2.drawStr((u8g2.getDisplayWidth() - u8g2.getStrWidth(bleName)) / 2, 10, bleName);
  if (connected) {
    // FW Version
    memset(displayMsg, 0, sizeof(displayMsg));
    snprintf(displayMsg, sizeof(displayMsg), "VERSION: %d", VERSION);
    u8g2.drawStr(0, 20, displayMsg);
    // Battery voltage
    memset(displayMsg, 0, sizeof(displayMsg));
    snprintf(displayMsg, sizeof(displayMsg), "BATTERY: %.2fV", voltage);
    u8g2.drawStr(0, 30, displayMsg);
    // Sampler status
    memset(displayMsg, 0, sizeof(displayMsg));
    snprintf(displayMsg, sizeof(displayMsg), "SAMPLER: %s", samplerActive ? "ACTIVE" : "INACTIVE");
    u8g2.drawStr(0, 40, displayMsg);
    // Sample count
    memset(displayMsg, 0, sizeof(displayMsg));
    snprintf(displayMsg, sizeof(displayMsg), "SAMPLES: %d", sampleCount);
    u8g2.drawStr(0, 50, displayMsg);
#if DEBUG
    u8g2.drawStr(0, 60, "DEBUGGING: ON");
#else
    u8g2.drawStr(0, 60, "DEBUGGING: OFF");
#endif
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
  if (displayActive) {
    u8g2.clearBuffer();
    // Center the text
    u8g2.drawStr(46, 25, "ERROR:");
    u8g2.drawStr((u8g2.getDisplayWidth() - u8g2.getStrWidth(err)) / 2, 35, err);
    u8g2.drawXBM(60, 45, EMOJI_WIDTH, EMOJI_HEIGHT, frowny);
    u8g2.sendBuffer();
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

void relay_init(void) {
  pinMode(WB_IO4, OUTPUT);
  digitalWrite(WB_IO4, LOW);
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
    snprintf(csvFilename, sizeof(csvFilename), "ZEPH%s%02d.csv", zephName, i);
    if (!SD.exists(csvFilename)) { break; }
  }
  csvFile = SD.open(csvFilename, FILE_WRITE);
  if (!csvFile) { error("CSV FILE", "Unable to create CSV file."); }
  if (csvFile.size() == 0) {
    csvFile.println("Date,Time,WindSpeed,WindDir,WindTemp,MaxSpeed,Length");
    csvFile.flush();
  }
  logFile = SD.open("ZEPHIRuS.txt", FILE_WRITE);
  if (!logFile) { error("LOG FILE", "Unable to create LOG file."); }
  logFile.printf("==========================================\n%s VERSION %d\n", bleName, VERSION);
  logFile.printf("Battery voltage: %.2f\nTargeted wind speed: %.2f m/s\n", voltage, targeted.windSpeed);
  logFile.flush();
}

void load_config(void) {
  File zfile = SD.open("zconfig.txt");
  if (!zfile) { error("CONFIG FILE", "Unable to read zconfig.txt."); }
  JsonDocument doc;
  DeserializationError jsonError = deserializeJson(doc, zfile);
  zfile.close();
  if (jsonError) { error("JSON CONFIG", "Unable to read json configuration."); }
  if (!doc.containsKey("ZEPHIRuS") || strlen(doc["ZEPHIRuS"]) != 2) { error("ZEPHIRuS NAME", "Config error: 'ZEPHIRuS'"); }
  if (!doc.containsKey("windSpeed")) { error("WINDSPEED", "Config error: 'windSpeed'"); }
  zephName = doc["ZEPHIRuS"];
  snprintf(bleName, sizeof(bleName), "ZEPHIRuS-%s", zephName);
  targeted.windSpeed = doc["windSpeed"];
#if DEBUG
  Serial.printf("%s - Targeted wind speed: %.2f m/s\n", bleName, targeted.windSpeed);
#endif
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
  Serial.printf("Connected to %s\n", central_name);
#endif
  if (displayActive) { oled_update(true); }
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
  if (samplerActive) { relay_handler(true); }
  if (displayActive) { oled_update(false); }
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

void relay_handler(bool override) {
  if (override) { digitalWrite(LED_GREEN, LOW); }  // make sure green LED is off when overriding relay
  if (!samplerActive && sampling_conditions()) {
    samplerActive = true;
    sampleCount++;
    startTime = millis();
#if DEBUG
    Serial.println("Sampler Active ... ");
    // Loop LEDs
    led_loop(5);
#else
    // Relay ON
    digitalWrite(WB_IO4, HIGH);
#endif
    log_data();
  }
  if (samplerActive && (!sampling_conditions() || override)) {
    samplerActive = false;
    sampleLength = (millis() - startTime) / 1000;
#if DEBUG
    Serial.printf(" ... Sampling complete after %d seconds.\n", sampleLength);
    Serial.printf("Max wind speed: %.2f\n", maxWindSpeed);
    Serial.printf("Sample count: %d\n", sampleCount);
    // Loop LEDs
    led_loop(5);
#else
    // Relay OFF
    digitalWrite(WB_IO4, LOW);
#endif
    log_data();
    maxWindSpeed = 0;
  }
  // Refresh display data
  if (displayActive) {
    if (Bluefruit.connected()) {
      oled_update(true);
    } else {
      oled_update(false);
    }
  }
}

bool sampling_conditions(void) {
  return observed.windSpeed >= targeted.windSpeed && observed.windSpeed < targeted.windSpeed + 1;
}

void log_data(void) {
  if (samplerActive) {
    gps_gettime();
    snprintf(msgBuf,
             sizeof(msgBuf),
             "%s,%.2f,%.1f,%.1f",
             timestamp,
             observed.windSpeed,
             observed.windDir,
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
