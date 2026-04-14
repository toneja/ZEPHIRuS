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
#include <ArduinoJson.h>
#include "SD.h"

#define DEBUG 1

// BLUETOOTH
BLEDfu bledfu;
BLEUart bleuart;
char bleName[12] = "ZEPHIRuS-XX";
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

// RELAY: timer
unsigned long startTime = 0;    // milliseconds
uint16_t sampleLength = 0; // seconds
bool samplerActive = false;

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
  // CONFIG
  load_config();
  // TEMPERATURE
  bme680_init();
  // BLUETOOTH
  ble_init();
}

void loop() {
  if (bleuart.available()) {
    // Flash green LED while receiving BLEUart data
    digitalWrite(LED_GREEN, HIGH);
    // Timestamp + Coordinates
    gps_get();
    // Onboard temperature/humidity
    bme680_get();
    // Read BLEUart data
    ble_get();
    // Relay: handle sampler controller
    relay_handler();
    // LED off, when sampler inactive
    if (!samplerActive) { digitalWrite(LED_GREEN, LOW); }
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
  Bluefruit.setName(bleName);
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

void ble_get(void) {
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
}

void connect_callback(uint16_t conn_handle) {
  BLEConnection* connection = Bluefruit.Connection(conn_handle);
  char central_name[32] = { 0 };
  connection->getPeerName(central_name, sizeof(central_name));
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

void relay_handler(void){
  if (!samplerActive && sampling_conditions()) {
    samplerActive = true;
    startTime = millis();
    // Relay ON
    digitalWrite(WB_IO4, HIGH);
    log_data();
#if DEBUG
    Serial.println("Sampler Active ... ");
#endif
  }
  if (samplerActive && !sampling_conditions()) {
    samplerActive = false;
    // Relay OFF
    digitalWrite(WB_IO4, LOW);
    sampleLength = (millis() - startTime) / 1000;
    log_data();
#if DEBUG
    Serial.print(" ... Sampling complete after ");
    Serial.print(sampleLength);
    Serial.println(" seconds.");
#endif
  }
}

bool sampling_conditions(void) {
  return ((observed.windSpeed >= targeted.windSpeed) &&
          (observed.windGust >= targeted.windGust) &&
            (observed.windTemp >= targeted.windTemp));
}

void sd_init(void) {
  if (SD.begin()) {
#if DEBUG
    Serial.println("SD Card mounted.\n");
#endif
    csvFile = SD.open("ZEPHIRuS.csv", FILE_WRITE);
    if (csvFile) { 
      if (csvFile.size() == 0) {
        csvFile.println("Date,Time,Latitude,Longitude,Altitude,Temperature,Humidity,WindSpeed,WindGust,WindTemp,Length");
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

void load_config(void) {
  File zfile = SD.open("zconfig.txt");
  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, zfile);
  zfile.close();
  if (error) {
#if DEBUG
    Serial.println("ERROR: unable to read configuration file.");
#endif
    led_error();
  }
  if (!doc.containsKey("ZEPHIRuS")) {
#if DEBUG
    Serial.println("ERROR: config missing 'ZEPHIRuS'");
#endif
    led_error();
  }
  if (!doc.containsKey("windSpeed")) {
#if DEBUG
    Serial.println("ERROR: config missing 'windSpeed'");
#endif
    led_error();
  }
  if (!doc.containsKey("windGust")) {
#if DEBUG
    Serial.println("ERROR: config missing 'windGust'");
#endif
    led_error();
  }
  if (!doc.containsKey("windTemp")) {
#if DEBUG
    Serial.println("ERROR: config missing 'windTemp'");
#endif
    led_error();
  }
  const char* zeph = doc["ZEPHIRuS"];
  bleName[9] = zeph[0];
  bleName[10] = zeph[1];
  targeted.windSpeed = doc["windSpeed"];
  targeted.windGust = doc["windGust"];
  targeted.windTemp = doc["windTemp"];
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
  sprintf(timestamp,
          "%d-%02d-%02d,%02d:%02d:%02d",
          g_myGNSS.getYear(), g_myGNSS.getMonth(), g_myGNSS.getDay(),
          g_myGNSS.getHour(), g_myGNSS.getMinute(), g_myGNSS.getSecond());
  latitude = g_myGNSS.getLatitude();
  longitude = g_myGNSS.getLongitude();
  altitude = g_myGNSS.getAltitude();
#if DEBUG
  Serial.print("Lat: ");
  Serial.print(latitude / 10000000.0, 7);
  Serial.print(" Long: ");
  Serial.print(longitude / 10000000.0, 7);
  Serial.print(" °, Alt: ");
  Serial.print(altitude / 1000);
  Serial.println(" m");
#endif
}

void bme680_init(void) {
  if (!bme.begin(0x76)) {
#if DEBUG
    Serial.println("ERROR: BME680 not found.");
#endif
    led_error();
  }
  bme.setTemperatureOversampling(BME680_OS_8X);
  bme.setHumidityOversampling(BME680_OS_2X);
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
  if (samplerActive) {
    csvFile.print(timestamp);
    csvFile.print(",");
    csvFile.print(latitude / 10000000.0, 7);
    csvFile.print(",");
    csvFile.print(longitude / 10000000.0, 7);
    csvFile.print(",");
    csvFile.print(altitude / 1000);
    csvFile.print(",");
    csvFile.print(bme.temperature);
    csvFile.print(",");
    csvFile.print(bme.humidity);
    csvFile.print(",");
    csvFile.print(observed.windSpeed);
    csvFile.print(",");
    csvFile.print(observed.windGust);
    csvFile.print(",");
    csvFile.print(observed.windTemp);
  } else {
    csvFile.print(",");
    csvFile.println(sampleLength);
  }
  csvFile.flush();
}
