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
BLEDfu bledfu;
BLEUart bleuart;
#define BLE_BUF_SIZE 32 // more than we need, for now
char buffer[BLE_BUF_SIZE];

// BLEUart Sensor Data
struct EnvironmentData {
  float windSpeed;
  float windGust;
  float windTemp;
  // float leafWetness;
};
EnvironmentData observed;
EnvironmentData targeted;
// TARGETED VALUES; read this from a config file
#define WIND_SPEED 3.00 // m/s
#define WIND_GUST 3.00  // m/s
#define WIND_TEMP 0.0   // *C

// Log files
File csvFile;

// GPS: position + timestamp
SFE_UBLOX_GNSS g_myGNSS;
char timestamp[19];
long latitude;
long longitude;
long altitude;

// TEMPERATURE
Adafruit_BME680 bme;

void setup() {
#if DEBUG
  // SERIAL
  Serial.begin(115200);
  // while (!Serial) { delay(100); }
  Serial.println("ZEPHIRuS - PERIPHERAL: SAMPLER");
#endif
  // LEDs
  led_init();
  // I2C
  sensor_init();
  // RELAY
  relay_init();
  // GPS
  gps_init();
  // SDCARD
  sd_init();
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
  if (bleuart.available()) {
    // Flash green LED while receiving BLEUart data
    digitalWrite(LED_GREEN, HIGH);
    // Timestamp
    gps_gettime();
    if (ble_get()) {
#if DEBUG
      Serial.println("*** SAMPLER ACTIVE ***");
#endif
      // RELAY - ENABLE FAN
      relay_enable();
      // GPS Coordinates at time of sampling
      gps_get();
      // Onboard temperature
      bme680_get();
      // Write to csv data file
      log_data();
#if DEBUG
      Serial.println("Sampling Complete.");
#endif
    } else {
      delay(200);
    }
    digitalWrite(LED_GREEN, LOW);
  }
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
  digitalWrite(LED_GREEN, HIGH);
  while (1) {
    digitalWrite(LED_BLUE, HIGH);
    delay(333);
    digitalWrite(LED_BLUE, LOW);
    delay(333);
    digitalWrite(LED_BLUE, HIGH);
    delay(333);
    digitalWrite(LED_BLUE, LOW);
    delay(4000);
  }
}

void sensor_init(void) {
  // I2C
  pinMode(WB_IO2, OUTPUT);
  digitalWrite(WB_IO2, HIGH);
  delay(1000);
  Wire.begin();
  delay(1000); // give em a sec to wake up
}

void ble_init(void) {
  Bluefruit.configPrphBandwidth(BANDWIDTH_MAX);
  Bluefruit.configPrphConn(92, BLE_GAP_EVENT_LENGTH_MIN, 16, 16);
  Bluefruit.begin(2, 0);
  Bluefruit.setTxPower(8);    // Check bluefruit.h for supported values
  Bluefruit.setName("ZEPHIRuS-SAMPLER");
  Bluefruit.Periph.setConnectCallback(connect_callback);
  Bluefruit.Periph.setDisconnectCallback(disconnect_callback);
  bledfu.begin();
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

bool ble_get(void) {
  int len = bleuart.readBytesUntil('\n', buffer, BLE_BUF_SIZE - 1);
  buffer[len] = '\0';
  char *token;
  token = strtok(buffer, ", ");
  if (token) observed.windSpeed = atof(token);
  token = strtok(NULL, ", ");
  if (token) observed.windGust = atof(token);
  token = strtok(NULL, ", ");
  if (token) observed.windTemp = atof(token);
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
    return true;
  }
  return false;
}

void connect_callback(uint16_t conn_handle) {
  BLEConnection* connection = Bluefruit.Connection(conn_handle);
  char central_name[32] = { 0 };
  connection->getPeerName(central_name, sizeof(central_name));
  Bluefruit.Advertising.stop();
#if DEBUG
  Serial.print("Connected to ");
  Serial.println(central_name);
#endif
}

void disconnect_callback(uint16_t conn_handle, uint8_t reason) {
  (void) conn_handle;
  (void) reason;
#if DEBUG
  Serial.print("Disconnected, reason = 0x");
  Serial.println(reason, HEX);
#endif
}

void relay_init(void) {
  pinMode(WB_IO4, OUTPUT);
  digitalWrite(WB_IO4, LOW);
}

void relay_enable(void) {
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
    csvFile = SD.open("ZEPHIRuS.csv", FILE_WRITE);
    if (csvFile) { 
      if (csvFile.size() == 0) {
        csvFile.println("Date,Time,Latitude,Longitude,Altitude,WindSpeed,WindGust,WindTemp");
        csvFile.flush();
      }
      return;
    }
#if DEBUG
    Serial.println("ERROR: Unable to create CSV file.");
#endif
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
    led_error();
  } else {
    g_myGNSS.setI2COutput(COM_TYPE_UBX);
    g_myGNSS.saveConfigSelective(VAL_CFG_SUBSEC_IOPORT);
    // Wait on the GPS fix for accurate timestamps
#if DEBUG
    Serial.print("Searching for GPS...");
#endif
    while (g_myGNSS.getFixType() < 3) {
      digitalToggle(LED_GREEN);
      digitalToggle(LED_BLUE);
      delay(250);
#if DEBUG
      Serial.print(".");
#endif
    }
#if DEBUG
    Serial.println("GPS fix acquired.");
#endif
  }
}

void gps_get(void) {
  latitude = g_myGNSS.getLatitude();
  longitude = g_myGNSS.getLongitude();
  altitude = g_myGNSS.getAltitude();
#if DEBUG
  Serial.print("Lat: ");
  Serial.print(latitude / 10000000.0, 7);
  Serial.print(" Long: ");
  Serial.print(longitude / 10000000.0, 7);
  Serial.print(" degrees, Alt: ");
  Serial.println(altitude);
#endif
}

void gps_gettime(void) {
  sprintf(timestamp,
          "%d-%02d-%02d,%02d:%02d:%02d",
          g_myGNSS.getYear(), g_myGNSS.getMonth(), g_myGNSS.getDay(),
          g_myGNSS.getHour(), g_myGNSS.getMinute(), g_myGNSS.getSecond());
}

void bme680_init(void) {
  if (!bme.begin(0x76)) {
#if DEBUG
    Serial.println("WARNING: BME680 not found.");
#endif
    // led_error();
  }
  bme.setTemperatureOversampling(BME680_OS_8X);
  // save power
  bme.setGasHeater(0, 0);
}

void bme680_get(void) {
  bme.performReading();
#if DEBUG
  Serial.print("Temperature = ");
  Serial.print(bme.temperature);
  Serial.print(" *C, ");
  Serial.print(bme.temperature * 1.8 + 32);
  Serial.println(" *F");
#endif
}

void log_data(void) {
  csvFile.print(timestamp);
  csvFile.print(",");
  csvFile.print(latitude / 10000000.0, 7);
  csvFile.print(",");
  csvFile.print(longitude / 10000000.0, 7);
  csvFile.print(",");
  csvFile.print(altitude / 1000);
  csvFile.print(",");
  csvFile.print(observed.windSpeed);
  csvFile.print(",");
  csvFile.print(observed.windGust);
  csvFile.print(",");
  csvFile.println(observed.windTemp);
  csvFile.flush();
}
