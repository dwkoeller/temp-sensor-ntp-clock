//This can be used to output the date the code was compiled
const char compile_date[] = __DATE__ " " __TIME__;

/************ WIFI, OTA and MQTT INFORMATION (CHANGE THESE FOR YOUR SETUP) ******************/
//#define WIFI_SSID "" //enter your WIFI SSID
//#define WIFI_PASSWORD "" //enter your WIFI Password
//#define MQTT_SERVER "" // Enter your MQTT server address or IP.
//#define MQTT_USER "" //enter your MQTT username
//#define MQTT_PASSWORD "" //enter your password
#define MQTT_DEVICE "temp-sensor-outdoors" // Enter your MQTT device
#define MQTT_SSL_PORT 8883 // Enter your MQTT server port.
#define MQTT_SOCKET_TIMEOUT 120
#define NTP_SERVERS "us.pool.ntp.org", "pool.ntp.org", "time.nist.gov"
#define NTP_UPDATE_INTERVAL_SEC 5*3600
#define WATCHDOG_UPDATE_INTERVAL_SEC 1
#define WATCHDOG_RESET_INTERVAL_SEC 120
#define FW_UPDATE_INTERVAL_SEC 24*3600
#define TEMP_UPDATE_INTERVAL_SEC 6
#define FIRMWARE_VERSION "-2.01"

/****************************** MQTT TOPICS (change these topics as you wish)  ***************************************/
#define TEMPERATURE "outdoor_temperature"
#define TEMPERATURE_NAME "Outdoor Temperature"
#define HUMIDITY "outdoor_humidity"
#define HUMIDITY_NAME "Outdoor Humidity"

#define MQTT_HEARTBEAT_SUB "heartbeat/#"
#define MQTT_HEARTBEAT_TOPIC "heartbeat"
#define MQTT_UPDATE_REQUEST "update"
#define MQTT_DISCOVERY_SENSOR_PREFIX  "homeassistant/sensor/"
#define HA_TELEMETRY                         "ha"

#define WATCHDOG_PIN 5  //  D1

#include <PubSubClient.h>
#include <DHT.h>
#include <ESP8266WiFi.h>
#include <Ticker.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266httpUpdate.h>
#include "credentials.h" // Place credentials for wifi and mqtt in this file
#include "certificates.h" // Place certificates for mqtt in this file

/****************************** DHT 22 Calibration settings *************/

float temp_offset = 5;
float hum_offset = 0;
float h,f,h2,f2;//Added %2 for error correction

#define DHTPIN 4        // (D2)
#define DHTTYPE DHT22   // DHT 22/11 (AM2302), AM2321

// Create event timers to update NTP, temperature and invert OLED disply
Ticker ticker_temp, ticker_fw;
int32_t tick_temp;

// flags for ticker functions
bool readyForFwUpdate = false;
bool readyForTempUpdate = false;
bool registered = false;

// Initialize DHT
DHT dht(DHTPIN, DHTTYPE);

WiFiClientSecure espClient;
PubSubClient client(espClient);

#include "common.h"


void setup() {
  Serial.begin(115200);

  pinMode(WATCHDOG_PIN, OUTPUT); 
  
  dht.begin();

  setup_wifi();

  IPAddress result;
  int err = WiFi.hostByName(MQTT_SERVER, result) ;
  if(err == 1){
        Serial.print("MQTT Server IP address: ");
        Serial.println(result);
        MQTTServerIP = result.toString();
  } else {
        Serial.print("Error code: ");
        Serial.println(err);
  }  

  client.setBufferSize(512);
  client.setServer(MQTT_SERVER, MQTT_SSL_PORT);
  client.setCallback(callback); //callback is the function that gets called for a topic sub
  
  checkForUpdates();
  
  Serial.println("\r\nSending NTP request ...");

  tick_temp = TEMP_UPDATE_INTERVAL_SEC;
  ticker_temp.attach(1, tempTicker);

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
    if (payload.equals(String(MQTT_UPDATE_REQUEST))) {
      checkForUpdates();
    }    
  }    
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
  if (! registered) {
    registerTelemetry();
    updateTelemetry("Unknown");
    createSensors(TEMPERATURE, TEMPERATURE_NAME, "temperature", "°F");
    createSensors(HUMIDITY, HUMIDITY_NAME, "humidity", "%");
    registered = true;
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
