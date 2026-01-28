/**********************************************************************************
* ZEPHYRUS: Zonal Epidemiologically-driven Plant Health Yield Risk Uptake Sampler *
*                                                                                 *
* Expected hardware components:                                                   *
*   BASE:   RAK WIRELESS 19001                                                    *
*   CORE:   RAK WIRELESS 4631                                                     *
*   RELAY:  RAK WIRELESS 13007                                                    *
*   GPS:    RAK WIRELESS 12500                                                    *
*   TEMP:   RAK WIRELESS 1906                                                     *
**********************************************************************************/

#include <bluefruit.h>
#include <Wire.h>
#include <SparkFun_u-blox_GNSS_Arduino_Library.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BME680.h>

#define DEBUG 1

BLEUart bleuart;

SFE_UBLOX_GNSS g_myGNSS;

Adafruit_BME680 bme;

uint8_t zephyrusClient = 0;

void setup() {
#if DEBUG
  // SERIAL
  Serial.begin(115200);
  while (!Serial) { delay(100); }
  Serial.println("ZEPHYRUS - PERIPHERAL: SAMPLER");
#endif
  // LEDs
  led_init();
  // I2C
  Wire.begin();
  // BLUETOOTH
  ble_init();
  // RELAY
  relay_init();
  // GPS
  gps_init();
  // TEMPERATURE
  bme680_init();
}

void loop() {
  if (bleuart.available()) {
#if DEBUG
    Serial.println("*** BLEUART WAKEUP ***");
#endif
    // Read data from BLEUART
    ble_get();
    // RELAY - ENABLE FAN
    relay_enable();
    // GPS Coordinates at time of sampling
    gps_get();
    // Onboard temperature
    bme680_get();
    // Disconnect BLE
    Bluefruit.disconnect(zephyrusClient);
#if DEBUG
    Serial.println("Sampling Complete. Nothing more to do.");
#endif
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

void ble_init(void) {
  Bluefruit.configPrphBandwidth(BANDWIDTH_MAX);
  Bluefruit.begin();
  Bluefruit.setTxPower(4);    // Check bluefruit.h for supported values
  Bluefruit.setName("ZEPHYRUS - SAMPLER");
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

void ble_get(void) {
  String buffer = "";
  while (bleuart.available()) { buffer += (char)bleuart.read(); }
#if DEBUG
  Serial.print("Wind Speed: ");
  Serial.print(buffer);
  Serial.println(" m/s");
#endif
}

void connect_callback(uint16_t conn_handle) {
  BLEConnection* connection = Bluefruit.Connection(conn_handle);
  char central_name[32] = { 0 };
  connection->getPeerName(central_name, sizeof(central_name));
  zephyrusClient = conn_handle;
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
  if (reason == 0x8) { Bluefruit.Advertising.start(0); }
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

void gps_init(void) {
  g_myGNSS.begin();
  g_myGNSS.setI2COutput(COM_TYPE_UBX);
  g_myGNSS.saveConfigSelective(VAL_CFG_SUBSEC_IOPORT);
}

void gps_get(void) {
  long latitude = g_myGNSS.getLatitude();
  long longitude = g_myGNSS.getLongitude();
#if DEBUG
  Serial.print("Lat: ");
  Serial.print(latitude);
  Serial.print(" Long: ");
  Serial.print(longitude);
  Serial.println(" (degrees * 10^-7)");
#endif
}

void bme680_init(void) {
  bme.begin(0x76);
  bme.setTemperatureOversampling(BME680_OS_8X);
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
