/*
  CO2 Ampel - PCB v2.0 + v1.0
  By: FabLab Karlsruhe e.V., Nils Roßmann
  https://github.com/fablab-ka/co2-ampel/
  Date: 14.11.2020
  License: MIT
*/

/* TODO
    adjust temperature offset due to new case
    check humidity and warn if lower than 40%
    create a class for mqtt, wifi etc.
    ntp+ssl cert check?
*/

#include <Wire.h>
#include "SparkFun_SCD30_Arduino_Library.h" // https://github.com/sparkfun/SparkFun_SCD30_Arduino_Library (1.0.8)
#include <Adafruit_BMP280.h> // https://github.com/adafruit/Adafruit_BMP280_Library (2.1.0)
#if defined(ESP8266)
  #include <ESP8266WiFi.h>
#elif defined(ESP32)
  #include <WiFi.h>
#endif

#include "ConfigManager.h"
#include "config.h"
#include "mqtt.h"
#include "wifi.h"
#include "buzzer.h"
#include "led.h"
#include "co2ampel.h"

SCD30 airSensor;

uint16_t green_th;
uint16_t yellow_thL;
uint16_t yellow_thH;
uint16_t red_th;

Color currentCO2Color = GREEN;


Adafruit_BMP280 bmp; // use I2C interface
Adafruit_Sensor *bmp_temp = bmp.getTemperatureSensor();
Adafruit_Sensor *bmp_pressure = bmp.getPressureSensor();



uint32_t mqttLastSend = 0;
uint32_t mqttLastTry = 0;
uint32_t scd30LastUpdate = 0;
bool bmpEnabled = BMP_ENABLED;
uint8_t recalibrateSwitch = 0;
uint32_t lastSwitchChange = 0;

bool bmpReady = false;
bool scd30Ready = false;

// ConfigManager
ConfigManager configManager;
uint8_t configStatus;


/* functions */

void selftest() {
  ledSetColor(WHITE);
  delay(5000);
  bool ok = true;
  sensors_event_t pressure_event;
  uint16_t co2;

  if ((!bmpReady && BMP_ENABLED) || !scd30Ready) {
    ok = false;
  } else {
    while (!digitalRead(GPIO_SCD30_RDY) || ! airSensor.dataAvailable()) {
      delay(500);
    }
    co2 = airSensor.getCO2();
    if (co2 < 300) {
      Serial.print("co2 too low: ");
      Serial.println(co2);
      ok = false;
    }
    if (bmpReady) {
      bmp_pressure->getEvent(&pressure_event);
      if (pressure_event.pressure < 700) {
        Serial.print("pressure too low ");
        Serial.println(pressure_event.pressure);
        ok = false;
      }
    }
  }
  uint16_t beepEnabled = configManager.getUintValue("selftest_buzzer", SELFTEST_BUZZER);

  if (ok) {
    if (beepEnabled) {
      beepOK(configManager);
    }
    ledBlink(GREEN, DARK, 2500);
  } else {
    if (beepEnabled) {
      beepFailure(configManager);
    }
    if (!bmpReady) {
      ledBlink(BLUE, RED, 5000);
    } else if (pressure_event.pressure < 700) {
      ledBlink(BLUE, DARK, 5000);
    } else if (co2 < 350) {
      ledBlink(YELLOW, DARK, 5000);
    }
  }
}

void scd30ForceRecalibration(uint16_t concentration) {
  bool ok = airSensor.setForcedRecalibrationFactor(concentration);
  Serial.print("calibration status: ");
  Serial.println(ok);
  if (ok) {
    ledBlink(GREEN, DARK, 2000);
  } else {
    ledBlink(RED, DARK, 2000);
  }
}

void setConfig(String& jsonString) {
  uint8_t err = configManager.writeConfig(jsonString, "/config.json");
  if (err != 0) {
    Serial.println("failed writing new config.json");
  } else {
    Serial.println("saved new config.json to flash");
  }
  while (1) // freez and reboot due to watchdog
    ;
}

void startCalibration() {
  ledSetColor(YELLOW2);
  delay(600000); // wait 10min
  bool ok = airSensor.setForcedRecalibrationFactor(410);
  Serial.print("calibration status: ");
  Serial.println(ok);
  if (ok) {
    ledBlink(GREEN, DARK, 2000);
    ledSetColor(GREEN2);
  } else {
    ledBlink(RED2, DARK, 2000);
    ledSetColor(RED);
  }
  while (1) {
    delay(1000);
  }
}


/**************************** setup() *************************************************/

void setup() {
  Serial.begin(115200);
  setupLED();

  // init IOs
  #ifdef GPIO_SWITCH
  pinMode(GPIO_SWITCH, INPUT);
  #endif


  pinMode(GPIO_BUZZER, OUTPUT);
  digitalWrite(GPIO_BUZZER, 0);

  delay(500);
  Serial.println("Start CO2-Ampel");

  configStatus = configManager.readConfig("/config.json");
  ledSetBrightnes(configManager.getUintValue("led_brightness", LED_BRIGHTNES));

  // check if mqtt credentials are set
  if (configStatus == 0) {
    if (configManager.getCharValue("mqtt_password", "") == "" || configManager.getCharValue("mqtt_username", "") == "") {
      configStatus = 1;
      Serial.println("mqtt username and password not set in config.json");
    }
  }

  if (configStatus != 0) {
    Serial.println("could not read config.json, try reading backup.json");
    configStatus = configManager.readConfig("/backup.json");
    if (configStatus == 0) {
      uint8_t err = configManager.writeConfig("/config.json");
      if (err) {
        Serial.println("could not restore config.json");
      } else {
        Serial.println("restored config.json with backup.json");
      }
    }
  }

  green_th = configManager.getUintValue("th_green", TH_GREEN);
  Serial.print("green_th: ");
  Serial.println(green_th);
  yellow_thL = configManager.getUintValue("th_yellow_low", TH_YELLOW_LOW);
  yellow_thH = configManager.getUintValue("th_yellow_high", TH_YELLOW_HIGH);
  red_th = configManager.getUintValue("th_red", TH_RED);

  if (configStatus == 0 && configManager.getUintValue("wifi_enabled", WIFI_ENABLED)) {
    setupWifi(configManager);
    setupMQTT(configManager);
  }

  Wire.begin();

  // Init SCD30
  if (airSensor.begin(Wire, SCD30_AUTOCALIBRATION) == false) {
    Serial.println("Air sensor not detected. Please check wiring. Freezing...");
    ledBlink(YELLOW, RED, 5000);
    while (1) // freez and reboot due to watchdog
      ;
  } else {
    scd30Ready = true;
  }
  airSensor.setAmbientPressure(1005);
  airSensor.setMeasurementInterval(SCD30_MEASUREMENT_INTERVAL);
  airSensor.setTemperatureOffset(SCD30_TEMP_OFFSET);

  // Init bmp280
  if (!bmp.begin(BMP280_ADDRESS_ALT, BMP280_CHIPID) && !bmp.begin(BMP280_ADDRESS, BMP280_CHIPID)) {
    Serial.println(F("Could not find a BMP280 sensor, check wiring!"));
    bmpEnabled = 0;
  }
  if (bmpEnabled) {
    // Default settings from datasheet.
    bmp.setSampling(Adafruit_BMP280::MODE_NORMAL,    // Operating Mode.
                    Adafruit_BMP280::SAMPLING_X1,     // Temp. oversampling
                    Adafruit_BMP280::SAMPLING_X1,    // Pressure oversampling
                    Adafruit_BMP280::FILTER_OFF,      // Filtering.
                    Adafruit_BMP280::STANDBY_MS_4000); // Standby time.
    bmpReady = true;
  }
  if (configManager.getUintValue("selftest_enabled", SELFTEST_ENABLED)) {
    selftest();
  }
}

/**************************** loop() *************************************************/

void loop() {
  if (configStatus == 0 && configManager.getUintValue("wifi_enabled", WIFI_ENABLED)) {
    loopWifi(configManager);
    loopMQTT(configManager);
  }

  #ifdef GPIO_SWITCH
  // check recalibration switch
  if (millis() < 30000 && abs(millis() - lastSwitchChange) > 500) {
    int switchState = digitalRead(GPIO_SWITCH);
    if (switchState == 1 && recalibrateSwitch == 0) {
      recalibrateSwitch++;
      lastSwitchChange = millis();
      Serial.println("switch is on at start");
    }
    if (switchState == 0 && recalibrateSwitch == 1) {
      recalibrateSwitch++;
      lastSwitchChange = millis();
      Serial.println("switch is off");
    }
    if (switchState == 1 && recalibrateSwitch == 2) {
      recalibrateSwitch++;
      lastSwitchChange = millis();
      Serial.println("switch is on again");
    }
  }
  if (recalibrateSwitch >= 3) {
    Serial.println("start Calibration");
    recalibrateSwitch = 0;
    startCalibration();
  }
  #endif

  int co2Available = digitalRead(GPIO_SCD30_RDY);

  if (abs(millis() - scd30LastUpdate) > SCD30_MEASUREMENT_INTERVAL * 50000) {
    Serial.println("didn't received co2 updates for some time");
    airSensor.sendCommand(0xD304);
    delay(1000);
    while (1) // freez and reboot due to watchdog
      ;
  }

  if (co2Available && airSensor.dataAvailable()) {
    scd30LastUpdate = millis();
    uint16_t co2 = airSensor.getCO2();
    Serial.print(co2);
    Serial.print(" ");
    double hum = airSensor.getHumidity();
    Serial.print(hum);
    Serial.print(" ");
    double temp = airSensor.getTemperature();
    Serial.print(temp);
    Serial.print(" ");

    sensors_event_t temp_event, pressure_event;
    double pressure = 0.0;
    double temp2 = 0.0;
    if (bmpEnabled) {
      bmp_temp->getEvent(&temp_event);
      bmp_pressure->getEvent(&pressure_event);
      temp2 = temp_event.temperature;
      Serial.print(temp2);
      Serial.print(" ");
      pressure = pressure_event.pressure;
      Serial.print(pressure);
      Serial.print(" ");
    }

    Color co2Color = GREEN;
    if (currentCO2Color == GREEN && co2 >= yellow_thL || currentCO2Color == YELLOW && co2 > green_th || currentCO2Color == RED && co2 <= yellow_thH) {
      co2Color = YELLOW;
      Serial.print("yellow ");
    }
    if (co2 >= red_th || currentCO2Color == RED && co2 > yellow_thH) {
      co2Color = RED;
      Serial.print("red ");
    }
    float RssI = WiFi.RSSI();
    Serial.print(RssI);
    Serial.println();

    //if(color != currentCO2Color)
    ledSetColor(co2Color);
    if (co2Color == GREEN && currentCO2Color != co2Color) {
      beep(configManager, 1);
      //  airSensor.setTemperatureOffset(SCD30_TEMP_OFFSET);
    }
    if (co2Color == YELLOW && currentCO2Color == GREEN) {
      beep(configManager, 2);
      // airSensor.setTemperatureOffset(SCD30_TEMP_OFFSET+3);
    }
    if (co2Color == RED && currentCO2Color != co2Color) {
      beep(configManager, 3);
      // airSensor.setTemperatureOffset(SCD30_TEMP_OFFSET+1.5);
    }
    currentCO2Color = co2Color;

    if (configStatus == 0 && configManager.getUintValue("wifi_enabled", WIFI_ENABLED)) {
      if (abs(millis() - mqttLastSend) >= configManager.getUintValue("mqtt_interval", MQTT_INTERVAL)) {
        mqttLastSend = millis();
        publishValues(configManager, co2, hum, temp, pressure, temp2, co2Color, RssI, millis()
                     );
      }
    }

    if (bmpEnabled) {
      Serial.print("set pressure: ");
      Serial.println(pressure);
      airSensor.setAmbientPressure(pressure);
    }
  }
}
