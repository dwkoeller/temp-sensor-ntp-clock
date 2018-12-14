#include <PubSubClient.h>
#include <DHT.h>
#include <ESP8266WiFi.h>
#include <WiFiManager.h>
#include <SSD1306.h> //OLED Lib for I2C version
#include <Time.h>
#include <Ticker.h>
#include <simpleDSTadjust.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266httpUpdate.h>
#include "credentials.h" // Place credentials for wifi and mqtt in this file

//This can be used to output the date the code was compiled
const char compile_date[] = __DATE__ " " __TIME__;

/************ WIFI, OTA and MQTT INFORMATION (CHANGE THESE FOR YOUR SETUP) ******************/
//#define WIFI_SSID "" //enter your WIFI SSID
//#define WIFI_PASSWORD "" //enter your WIFI Password
//#define MQTT_SERVER "" // Enter your MQTT server address or IP.
//#define MQTT_USER "" //enter your MQTT username
//#define MQTT_PASSWORD "" //enter your password
#define MQTT_DEVICE "temp-sensor-office" // Enter your MQTT device
#define MQTT_PORT 1883 // Enter your MQTT server port.
#define MQTT_SOCKET_TIMEOUT 120
#define NTP_SERVERS "us.pool.ntp.org", "pool.ntp.org", "time.nist.gov"
#define NTP_UPDATE_INTERVAL_SEC 5*3600
#define WATCHDOG_UPDATE_INTERVAL_SEC 1
#define WATCHDOG_RESET_INTERVAL_SEC 120
#define FW_UPDATE_INTERVAL_SEC 24*3600
#define TEMP_UPDATE_INTERVAL_SEC 6
#define DISPLAY_INVERT_INTERVAL_SEC 30
#define UPDATE_SERVER "http://192.168.100.15/firmware/"
#define FIRMWARE_VERSION "-1.17"

/****************************** MQTT TOPICS (change these topics as you wish)  ***************************************/
#define MQTT_TEMPERATURE_PUB "sensor/office/temperature"
#define MQTT_HUMIDITY_PUB "sensor/office/humidity"
#define MQTT_VERSION_PUB "sensor/office/version"
#define MQTT_COMPILE_PUB "sensor/office/compile"
#define MQTT_HEARTBEAT_SUB "heartbeat/#"
#define MQTT_HEARTBEAT_TOPIC "heartbeat"

/****************************** DHT 22 Calibration settings *************/

float temp_offset = -2;
float hum_offset = 0;
float h,f,h2,f2;//Added %2 for error correction

#define DHTPIN 4        // (D2)
#define DHTTYPE DHT22   // DHT 22/11 (AM2302), AM2321

volatile int watchDogCount = 0;

// Create event timers to update NTP, temperature and invert OLED disply
Ticker ticker_time, ticker_temp, ticker_display, ticker_fw, ticker_watchdog;
int32_t tick_time, tick_temp, tick_display, tick_fw;

// flags for ticker functions
bool readyForNtpUpdate = false;
bool readyForFwUpdate = false;
bool readyForTempUpdate = false;
bool readyForDisplayInvert = false;
bool invertDisplay = false;

// Initialize DHT
DHT dht(DHTPIN, DHTTYPE);

// Set DST and timezone for use with simpleDSTadjust
// US Eastern Time Zone (New York, Boston)
#define timezone -5 // US Eastern Time Zone
struct dstRule StartRule = {"EDT", Second, Sun, Mar, 2, 3600};    // Daylight time = UTC/GMT -4 hours
struct dstRule EndRule = {"EST", First, Sun, Nov, 2, 0};       // Standard time = UTC/GMT -5 hour

// Setup simpleDSTadjust Library rules
simpleDSTadjust dstAdjusted(StartRule, EndRule);

// Init WiFi
WiFiClient espClient;

// Init MQTT
PubSubClient client(espClient);

// Init display
SSD1306     display(0x3c, 0 /*D3*/, 2 /*D4*/); //Recommended Setup: to be able to use the USB Serial Monitor, use the configuration on this line

// Store date and time in a string for display updates
String dateTime;
String lastDateTime;

void setup() {
  Serial.begin(115200);
  dht.begin();

  display.init();
  display.setFont(ArialMT_Plain_10);
  display.setTextAlignment(TEXT_ALIGN_LEFT);
  setup_wifi();

  client.setServer(MQTT_SERVER, MQTT_PORT);
  client.setCallback(callback); //callback is the function that gets called for a topic sub
  
  check_for_updates();
  
  Serial.println("\r\nSending NTP request ...");
  updateNTP(); // Init the NTP time

  tick_time = NTP_UPDATE_INTERVAL_SEC;
  ticker_time.attach(1, secTicker);
  tick_temp = TEMP_UPDATE_INTERVAL_SEC;
  ticker_temp.attach(1, tempTicker);
  tick_display = DISPLAY_INVERT_INTERVAL_SEC;
  ticker_display.attach(1, displayTicker);
  tick_fw = FW_UPDATE_INTERVAL_SEC;
  ticker_fw.attach(1, fwTicker);
  ticker_watchdog.attach_ms(WATCHDOG_UPDATE_INTERVAL_SEC * 1000, watchdogTicker);

  lastDateTime = getDateTime(0);

  Serial.print("Current Time: ");
  Serial.print(getDateTime(0));    
  Serial.print("Next NTP Update: ");
  Serial.print(getDateTime(tick_time)); 
  
}

void callback(char* topic, byte* payload, unsigned int length) {
  String strTopic;
  payload[length] = '\0';
  strTopic = String((char*)topic);
  if (strTopic == MQTT_HEARTBEAT_TOPIC) {
    watchDogCount = 0;
  }
}

void setup_wifi() {
  int count = 0;
  my_delay(50);

  Serial.println();
  Serial.print("Connecting to ");
  Serial.println(WIFI_SSID);
  
  WiFi.mode(WIFI_STA);
  WiFi.hostname(MQTT_DEVICE);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  while (WiFi.status() != WL_CONNECTED) {
    my_delay(250);
    Serial.print(".");
    count++;
    if(count > 50) {
      WiFiManager wifiManager;
      wifiManager.resetSettings();
      wifiManager.autoConnect();
    }
  }

  Serial.println("");
  Serial.println("WiFi connected");
  Serial.println("IP address: ");
  Serial.println(WiFi.localIP());
  
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
    check_for_updates();
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
        client.publish(MQTT_TEMPERATURE_PUB, strCh, true);
        str = String(h,1);
        str.toCharArray(strCh,9);
        client.publish(MQTT_HUMIDITY_PUB, strCh, true); 
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
}

void reconnect() {
  // Loop until we're reconnected
  while (!client.connected()) {
    Serial.print("Attempting MQTT connection...");
    
    // Attempt to connect
  if (client.connect(MQTT_DEVICE, MQTT_USER, MQTT_PASSWORD)) {
      Serial.println("MQTT broker connected");
      client.subscribe(MQTT_HEARTBEAT_SUB);
      String firmwareVer = String("Firmware Version: ") + String(FIRMWARE_VERSION);
      String compileDate = String("Build Date: ") + String(compile_date);
      client.publish(MQTT_VERSION_PUB, firmwareVer.c_str(), true);
      client.publish(MQTT_COMPILE_PUB, compileDate.c_str(), true);
    } else {
      Serial.print("MQTT broker connect failed, rc=");
      Serial.print(client.state());
      Serial.println(" trying again in 5 seconds");
      // Wait 5 seconds before retrying
      my_delay(5000);
    }
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

// FW update ticker
void fwTicker() {
  tick_fw--;
  if(tick_fw<=0)
   {
    readyForFwUpdate = true;
    tick_fw= FW_UPDATE_INTERVAL_SEC; // Re-arm
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

// Watchdog update ticker
void watchdogTicker() {
  watchDogCount++;
  if(watchDogCount >= WATCHDOG_RESET_INTERVAL_SEC) {
    Serial.println("Reset system");
    ESP.restart();  
  }
}

String WiFi_macAddressOf(IPAddress aIp) {
  if (aIp == WiFi.localIP())
    return WiFi.macAddress();

  if (aIp == WiFi.softAPIP())
    return WiFi.softAPmacAddress();

  return String("00-00-00-00-00-00");
}

void check_for_updates() {

  String clientMAC = WiFi_macAddressOf(espClient.localIP());

  Serial.print("MAC: ");
  Serial.println(clientMAC);
  clientMAC.replace(":", "-");
  String filename = clientMAC.substring(9);
  String firmware_URL = String(UPDATE_SERVER) + filename + String(FIRMWARE_VERSION);
  String current_firmware_version_URL = String(UPDATE_SERVER) + filename + String("-current_version");

  HTTPClient http;

  http.begin(current_firmware_version_URL);
  int httpCode = http.GET();
  
  if ( httpCode == 200 ) {

    String new_firmware_version = http.getString();
    new_firmware_version.trim();
    
    Serial.print( "Current firmware version: " );
    Serial.println( FIRMWARE_VERSION );
    Serial.print( "Available firmware version: " );
    Serial.println( new_firmware_version );
    
    if(new_firmware_version.substring(1).toFloat() > String(FIRMWARE_VERSION).substring(1).toFloat()) {
      Serial.println( "Preparing to update" );
      String new_firmware_URL = String(UPDATE_SERVER) + filename + new_firmware_version + ".bin";
      Serial.println(new_firmware_URL);
      t_httpUpdate_return ret = ESPhttpUpdate.update( new_firmware_URL );

      switch(ret) {
        case HTTP_UPDATE_FAILED:
          Serial.printf("HTTP_UPDATE_FAILD Error (%d): %s", ESPhttpUpdate.getLastError(), ESPhttpUpdate.getLastErrorString().c_str());
          break;

        case HTTP_UPDATE_NO_UPDATES:
          Serial.println("HTTP_UPDATE_NO_UPDATES");
         break;
      }
    }
    else {
      Serial.println("Already on latest firmware");  
    }
  }
  else {
    Serial.print("GET RC: ");
    Serial.println(httpCode);
  }
}

void my_delay(unsigned long ms) {
  uint32_t start = micros();

  while (ms > 0) {
    yield();
    while ( ms > 0 && (micros() - start) >= 1000) {
      ms--;
      start += 1000;
    }
  }
}
