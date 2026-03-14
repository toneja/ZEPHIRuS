/******************************************************************
* ZEPHIRuS: Zonal Epidemiology Plant Health Inoculum Risk Sampler *
*                                                                 *
* Expected hardware components:                                   *
*   BASE:   RAK WIRELESS 19001                                    *
*   CORE:   RAK WIRELESS 4631                                     *
*   RELAY:  RAK WIRELESS 13007                                    *
*   SDCARD: RAK WIRELESS 15002                                    *
*   GPS:    RAK WIRELESS 12500                                    *
*   TEMP:   RAK WIRELESS 1906                                     *
******************************************************************/

#include <bluefruit.h>
#include <Wire.h>
#include <SparkFun_u-blox_GNSS_Arduino_Library.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BME680.h>
#include "SD.h"

#define DEBUG 1

// BLUETOOTH
BLEUart bleuart;
uint8_t zephirusClient = BLE_CONN_HANDLE_INVALID;
#define BLE_BUF_SIZE 32 // more than we need, for now
char buffer[BLE_BUF_SIZE];

// BLEUart Sensor Data
struct EnvironmentData {
  float windTemp;
  float windSpeed;
  float windGust;
  // float leafWetness;
};
EnvironmentData observed;
EnvironmentData targeted;
// TARGETED VALUES; read this from a config file
#define WIND_SPEED 3.00 // m/s
#define WIND_GUST 3.00  // m/s
#define WIND_TEMP 0.0   // *C

// Log file
File logFile;

// GPS
SFE_UBLOX_GNSS g_myGNSS;

// TEMPERATURE
Adafruit_BME680 bme;

void setup() {
#if DEBUG
  // SERIAL
  Serial.begin(115200);
  while (!Serial) { delay(100); }
  Serial.println("ZEPHIRuS - PERIPHERAL: SAMPLER");
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
  // Target Values
  targeted.windSpeed = WIND_SPEED;
  targeted.windGust = WIND_GUST;
  targeted.windTemp = WIND_TEMP;
}

void loop() {
  if (bleuart.available() && ble_get()) {
    logFile.println("*** BLEUART WAKEUP ***");
#if DEBUG
    Serial.println("*** BLEUART WAKEUP ***");
#endif
    // RELAY - ENABLE FAN
    relay_enable();
    // GPS Coordinates at time of sampling
    gps_get();
    // Onboard temperature
    bme680_get();
    logFile.println("Sampling Complete. Nothing more to do.");
    logFile.flush();
    logFile.close();
#if DEBUG
    Serial.println("Sampling Complete. Nothing more to do.");
#endif
    teardown();
    led_complete();
  }
  delay(1000);
}

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

void led_error(void) {
  while (1) {
    digitalToggle(LED_GREEN);
    delay(500);
    digitalToggle(LED_BLUE);
    delay(500);
  }
}

void led_complete(void) {
  digitalWrite(LED_GREEN, LOW);
  while (1) {
    digitalWrite(LED_BLUE, HIGH);
    delay(1000);
    digitalWrite(LED_BLUE, LOW);
    delay(10000);
  }
}

void sensor_init(void) {
  // 3V3_S
  pinMode(WB_IO2, OUTPUT);
  digitalWrite(WB_IO2, LOW);
  delay(1000);
  digitalWrite(WB_IO2, HIGH);
  // I2C
  Wire.begin();
}

void teardown(void) {
  Bluefruit.Advertising.stop();
  Bluefruit.disconnect(zephirusClient);
  Bluefruit.setTxPower(-40);
  g_myGNSS.end();
  SD.end();
  Wire.end();
  digitalWrite(WB_IO2, LOW);
#if DEBUG
  Serial.end();
#endif
}

void ble_init(void) {
  Bluefruit.configPrphBandwidth(BANDWIDTH_MAX);
  Bluefruit.begin();
  Bluefruit.setTxPower(8);    // Check bluefruit.h for supported values
  Bluefruit.setName("ZEPHIRuS-SAMPLER");
  Bluefruit.Periph.setConnectCallback(connect_callback);
  Bluefruit.Periph.setDisconnectCallback(disconnect_callback);
  bleuart.begin();
  startAdv();
}

void startAdv(void) {
  Bluefruit.Advertising.addFlags(BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE);
  Bluefruit.Advertising.addTxPower();
  Bluefruit.Advertising.addService(bleuart);
  Bluefruit.ScanResponse.addName();
  Bluefruit.Advertising.restartOnDisconnect(false);
  Bluefruit.Advertising.setInterval(32, 244);
  Bluefruit.Advertising.setFastTimeout(30);
  Bluefruit.Advertising.start(0);
}

bool ble_get(void) {
  int len = bleuart.readBytesUntil('\n', buffer, BLE_BUF_SIZE - 1);
  buffer[len] = '\0';
  sscanf(buffer,
        "%f, %f, %f",
        &observed.windSpeed,
        &observed.windGust,
        &observed.windTemp);
#if DEBUG
  Serial.print("WindSpeed: ");
  Serial.print(observed.windSpeed);
  Serial.print(", WindGust: ");
  Serial.print(observed.windGust);
  Serial.print(", WindTemp: ");
  Serial.println(observed.windTemp);
#endif
  if ((observed.windSpeed >= targeted.windSpeed) &&
      (observed.windGust >= targeted.windGust) &&
      (observed.windTemp >= targeted.windTemp)) {
    logFile.println("WindSpeed, WindGust, WindTemp");
    logFile.println(buffer);
    logFile.flush();
    return true;
  }
  return false;
}

void connect_callback(uint16_t conn_handle) {
  BLEConnection* connection = Bluefruit.Connection(conn_handle);
  char central_name[32] = { 0 };
  connection->getPeerName(central_name, sizeof(central_name));
  zephirusClient = conn_handle;
  Bluefruit.Advertising.stop();
  logFile.print("Connected to ");
  logFile.println(central_name);
  logFile.flush();
#if DEBUG
  Serial.print("Connected to ");
  Serial.println(central_name);
#endif
}

void disconnect_callback(uint16_t conn_handle, uint8_t reason) {
  (void) conn_handle;
  (void) reason;
  logFile.print("Disconnected, reason = 0x");
  logFile.println(reason, HEX);
  logFile.flush();
#if DEBUG
  Serial.print("Disconnected, reason = 0x");
  Serial.println(reason, HEX);
#endif
  if (reason == BLE_HCI_CONNECTION_TIMEOUT) { Bluefruit.Advertising.start(0); }
}

void relay_init(void) {
  pinMode(WB_IO4, OUTPUT);
  digitalWrite(WB_IO4, LOW);
}

void relay_enable(void) {
  logFile.println("Waking Fan...");
  logFile.flush();
#if DEBUG
  Serial.println("Waking Fan...");
#endif
  digitalWrite(WB_IO4, HIGH);
  delay(5000);
  digitalWrite(WB_IO4, LOW);
}

void sd_init(void) {
  if (SD.begin()) {
#if DEBUG
    Serial.println("SD Card mounted.\n");
#endif
    logFile = SD.open("ZEPHIRuS.txt", FILE_WRITE);
    if (logFile) {
      logFile.println("ZEPHIRuS - PERIPHERAL: SAMPLER");
      logFile.flush();
      return;
    } else {
#if DEBUG
      Serial.println("ERROR: Unable to create LOG file.");
#endif
    }
  } else {
#if DEBUG
    Serial.println("ERROR: No SD Card found.\n");
#endif
  }
  led_error();
}

void gps_init(void) {
  if (!g_myGNSS.begin()) {
#if DEBUG
    Serial.println("ERROR: GPS not found.");
#endif
    logFile.println("ERROR: GPS not found.");
    logFile.flush();
    led_error();
  }
  g_myGNSS.setI2COutput(COM_TYPE_UBX);
  g_myGNSS.saveConfigSelective(VAL_CFG_SUBSEC_IOPORT);
}

void gps_get(void) {
  long latitude = g_myGNSS.getLatitude();
  long longitude = g_myGNSS.getLongitude();
  logFile.print("Lat: ");
  logFile.print(latitude);
  logFile.print(" Long: ");
  logFile.print(longitude);
  logFile.println(" (degrees * 10^-7)");
  logFile.flush();
#if DEBUG
  Serial.print("Lat: ");
  Serial.print(latitude);
  Serial.print(" Long: ");
  Serial.print(longitude);
  Serial.println(" (degrees * 10^-7)");
#endif
}

void bme680_init(void) {
  if (!bme.begin(0x76)) {
#if DEBUG
    Serial.println("ERROR: BME680 not found.");
#endif
    logFile.println("ERROR: BME680 not found.");
    logFile.flush();
    led_error();
  }
  bme.setTemperatureOversampling(BME680_OS_8X);
  bme.setGasHeater(0, 0);
}

void bme680_get(void) {
  bme.performReading();
  logFile.print("Temperature = ");
  logFile.print(bme.temperature);
  logFile.print(" *C, ");
  logFile.print(bme.temperature * 1.8 + 32);
  logFile.println(" *F");
  logFile.flush();
#if DEBUG
  Serial.print("Temperature = ");
  Serial.print(bme.temperature);
  Serial.print(" *C, ");
  Serial.print(bme.temperature * 1.8 + 32);
  Serial.println(" *F");
#endif
}
