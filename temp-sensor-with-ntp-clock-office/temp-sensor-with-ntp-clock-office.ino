//This can be used to output the date the code was compiled
const char compile_date[] = __DATE__ " " __TIME__;

/************ WIFI, OTA and MQTT INFORMATION (CHANGE THESE FOR YOUR SETUP) ******************/
//#define WIFI_SSID "" //enter your WIFI SSID
//#define WIFI_PASSWORD "" //enter your WIFI Password
//#define MQTT_SERVER "" // Enter your MQTT server address or IP.
//#define MQTT_USER "" //enter your MQTT username
//#define MQTT_PASSWORD "" //enter your password
#define MQTT_DEVICE "temp-sensor-office" // Enter your MQTT device
#define MQTT_SSL_PORT 8883 // Enter your MQTT server port.
#define MQTT_SOCKET_TIMEOUT 120
#define NTP_SERVERS "us.pool.ntp.org", "pool.ntp.org", "time.nist.gov"
#define NTP_UPDATE_INTERVAL_SEC 5*3600
#define WATCHDOG_UPDATE_INTERVAL_SEC 1
#define WATCHDOG_RESET_INTERVAL_SEC 120
#define FW_UPDATE_INTERVAL_SEC 24*3600
#define TEMP_UPDATE_INTERVAL_SEC 6
#define DISPLAY_INVERT_INTERVAL_SEC 30
#define UPDATE_SERVER "http://192.168.100.15/firmware/"
#define FIRMWARE_VERSION "-1.45"

/****************************** MQTT TOPICS (change these topics as you wish)  ***************************************/
#define TEMPERATURE "office_temperature"
#define TEMPERATURE_NAME "Office Temperature"
#define HUMIDITY "office_humidity"
#define HUMIDITY_NAME "Office Humidity"

#define MQTT_HEARTBEAT_SUB "heartbeat/#"
#define MQTT_HEARTBEAT_TOPIC "heartbeat"
#define MQTT_DISCOVERY_SENSOR_PREFIX  "homeassistant/sensor/"
#define HA_TELEMETRY                         "ha"

#define WATCHDOG_PIN 5  //  D1

#include <PubSubClient.h>
#include <DHT.h>
#include <ESP8266WiFi.h>
#include <SSD1306.h> //OLED Lib for I2C version
#include <Time.h>
#include <Ticker.h>
#include <simpleDSTadjust.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266httpUpdate.h>
#include "credentials.h" // Place credentials for wifi and mqtt in this file
#include "certificates.h" // Place certificates for mqtt in this file

/****************************** DHT 22 Calibration settings *************/

float temp_offset = -2;
float hum_offset = 0;
float h,f,h2,f2;//Added %2 for error correction

#define DHTPIN 4        // (D2)
#define DHTTYPE DHT22   // DHT 22/11 (AM2302), AM2321

// Create event timers to update NTP, temperature and invert OLED disply
Ticker ticker_time, ticker_temp, ticker_display, ticker_fw;
int32_t tick_time, tick_temp, tick_display;

// flags for ticker functions
bool readyForNtpUpdate = false;
bool readyForFwUpdate = false;
bool readyForTempUpdate = false;
bool readyForDisplayInvert = false;
bool invertDisplay = false;
bool registered = false;

// Initialize DHT
DHT dht(DHTPIN, DHTTYPE);

// Set DST and timezone for use with simpleDSTadjust
// US Eastern Time Zone (New York, Boston)
#define timezone -5 // US Eastern Time Zone
struct dstRule StartRule = {"EDT", Second, Sun, Mar, 2, 3600};    // Daylight time = UTC/GMT -4 hours
struct dstRule EndRule = {"EST", First, Sun, Nov, 2, 0};       // Standard time = UTC/GMT -5 hour

// Setup simpleDSTadjust Library rules
simpleDSTadjust dstAdjusted(StartRule, EndRule);

WiFiClientSecure espClient;
PubSubClient client(espClient);

#include "common.h"

// Init display
SSD1306     display(0x3c, 0 /*D3*/, 2 /*D4*/); //Recommended Setup: to be able to use the USB Serial Monitor, use the configuration on this line

// Store date and time in a string for display updates
String dateTime;
String lastDateTime;

void setup() {
  Serial.begin(115200);

  pinMode(WATCHDOG_PIN, OUTPUT); 
  
  dht.begin();

  display.init();
  display.setFont(ArialMT_Plain_10);
  display.setTextAlignment(TEXT_ALIGN_LEFT);
  setup_wifi();

  client.setServer(MQTT_SERVER, MQTT_SSL_PORT);
  client.setCallback(callback); //callback is the function that gets called for a topic sub
  
  checkForUpdates();
  
  Serial.println("\r\nSending NTP request ...");
  updateNTP(); // Init the NTP time

  tick_time = NTP_UPDATE_INTERVAL_SEC;
  ticker_time.attach(1, secTicker);
  tick_temp = TEMP_UPDATE_INTERVAL_SEC;
  ticker_temp.attach(1, tempTicker);
  tick_display = DISPLAY_INVERT_INTERVAL_SEC;
  ticker_display.attach(1, displayTicker);

  lastDateTime = getDateTime(0);

  Serial.print("Current Time: ");
  Serial.print(getDateTime(0));    
  Serial.print("Next NTP Update: ");
  Serial.print(getDateTime(tick_time)); 

  resetWatchdog();
 
}

void callback(char* p_topic, byte* p_payload, unsigned int p_length) {
  String strTopic;
  String payload;

  for (uint8_t i = 0; i < p_length; i++) {
    payload.concat((char)p_payload[i]);
  }

  strTopic = String((char*)p_topic);
  if (strTopic == MQTT_HEARTBEAT_TOPIC) {
    resetWatchdog();
    updateTelemetry(payload);
  }
}

void drawDHT(float h, float f) {
  display.setFont(ArialMT_Plain_10);
  display.setTextAlignment(TEXT_ALIGN_LEFT);
  display.drawString(0,0, String(dateTime));
  display.setFont(ArialMT_Plain_16);
  display.drawString(0,13, String("Temperature"));
  display.setFont(ArialMT_Plain_16);
  display.setTextAlignment(TEXT_ALIGN_LEFT);
  display.drawString(0,30,"Master: ");
  display.setTextAlignment(TEXT_ALIGN_RIGHT);
  display.drawString(120,30, String(f,1) + " *F");
  display.setFont(ArialMT_Plain_16);
  display.setTextAlignment(TEXT_ALIGN_LEFT);
  display.drawString(0,48,"Humidity: ");
  display.setTextAlignment(TEXT_ALIGN_RIGHT);
  display.drawString(120,48, String(h,1) + " %");
}

void loop() {
  char strCh[10];  
  String str;
  
  if (!client.loop()) {
    reconnect();
  }

  if(readyForFwUpdate) {
    readyForFwUpdate = false;
    checkForUpdates();
  }

  if(readyForNtpUpdate) {
    readyForNtpUpdate = false;
    updateNTP();
    Serial.print("\nUpdated time from NTP Server: ");
    Serial.println(getDateTime(0));
    Serial.print("Next NTP Update: ");
    Serial.println(getDateTime(tick_time));
  }

  if(readyForDisplayInvert) {
    readyForDisplayInvert = false;
    invertDisplay = !invertDisplay;
  }

  if(readyForTempUpdate) {
      readyForTempUpdate = false;
      h = dht.readHumidity();
      // Read temperature as Fahrenheit (isFahrenheit = true)
      f = dht.readTemperature(true);

      // Check if any reads failed and exit early (to try again).
      if (isnan(h) || isnan(f)) {
        Serial.println("Failed to read from DHT sensor!");
        //h=t=f=-1;
        f=f2;
        h=h2;
        
      }
      else { //add offsets, if any
        f = f + temp_offset;
        h = h + hum_offset;
        h2=h;//Store values encase next read fails
        f2=f;
        Serial.print("Humidity: ");
        Serial.print(h,1);
        Serial.print(" %\t");
        Serial.print("Temperature: ");
        Serial.print(f,1);
        Serial.print(" *F\n");
        str = String(f,1);
        str.toCharArray(strCh,9);
        updateSensor(TEMPERATURE, strCh);
        str = String(h,1);
        str.toCharArray(strCh,9);
        updateSensor(HUMIDITY, strCh);
        client.loop();
      }
  }

  dateTime = getDateTime(0);
  if(dateTime != lastDateTime) {
    lastDateTime = dateTime;

    display.clear();
    if (invertDisplay) {
      drawDHT(h, f);
      display.normalDisplay();
      display.display();
    }
    else {
      drawDHT(h, f);
      display.invertDisplay();
      display.display();
    }
  }
  if (! registered) {
    registerTelemetry();
    updateTelemetry("Unknown");
    createSensors(TEMPERATURE, TEMPERATURE_NAME, "temperature", "°F");
    createSensors(HUMIDITY, HUMIDITY_NAME, "humidity", "%");
    registered = true;
  }  
  
}

// Update NTP Information
void updateNTP() {

  configTime(timezone * 3600, 0, NTP_SERVERS);

  my_delay(500);
  while (!time(nullptr)) {
    Serial.print("#");
    my_delay(500);
  }
}

// NTP timer update ticker
void secTicker() {
  tick_time--;
  if(tick_time<=0)
   {
    readyForNtpUpdate = true;
    tick_time= NTP_UPDATE_INTERVAL_SEC; // Re-arm
   }
}

// Temperature update ticker
void tempTicker() {
  tick_temp--;
  if(tick_temp<=0)
   {
    readyForTempUpdate = true;
    tick_temp= TEMP_UPDATE_INTERVAL_SEC; // Re-arm
   }
}

// Invert display ticker
void displayTicker() {
  tick_display--;
  if(tick_display<=0)
   {
    readyForDisplayInvert = true;
    tick_display= DISPLAY_INVERT_INTERVAL_SEC; // Re-arm
   }
}

// Return timezone and DST adjusted string based off unixTime
String getDateTime(time_t offset) {
  char buf[30];
  char *dstAbbrev;
  time_t t = dstAdjusted.time(&dstAbbrev)+offset;
  struct tm *timeinfo = localtime (&t);

  int hour = (timeinfo->tm_hour+11)%12+1;  // take care of noon and midnight
  sprintf(buf, "%02d/%02d/%04d %02d:%02d:%02d%s\n",timeinfo->tm_mon+1, timeinfo->tm_mday, timeinfo->tm_year+1900, hour, timeinfo->tm_min, timeinfo->tm_sec, timeinfo->tm_hour>=12?"pm":"am");
  return buf;
}

void createSensors(String sensor, String sensor_name, String device_class, String unit) {
  String topic = String(MQTT_DISCOVERY_SENSOR_PREFIX) + sensor + "/config";
  String message = String("{\"name\": \"") + sensor_name +
                   String("\", \"unit_of_measurement\": \"") + unit +
                   String("\", \"unique_id\": \"") + sensor + getUUID() +
                   String("\", \"state_topic\": \"") + String(MQTT_DISCOVERY_SENSOR_PREFIX) + sensor +
                   String("/state\", \"device_class\": \"" + device_class + "\"}");
  Serial.print(F("MQTT - "));
  Serial.print(topic);
  Serial.print(F(" : "));
  Serial.println(message.c_str());

  client.publish(topic.c_str(), message.c_str(), true);  

}

void updateSensor(String sensor, String state) {
  String topic = String(MQTT_DISCOVERY_SENSOR_PREFIX) + sensor + "/state";
  
  Serial.print(F("MQTT - "));
  Serial.print(topic);
  Serial.print(F(" : "));
  Serial.println(state);
  client.publish(topic.c_str(), state.c_str(), true);

}
