#include <ESP8266WiFi.h>
#include <EEPROM.h>
#include <ESP8266WebServer.h>
#include <PubSubClient.h>
#include <ESP8266mDNS.h>
#include <ESP8266HTTPUpdateServer.h>
#include <Ticker.h>

const String ver = "v1.0";

struct DeviceData {
  double voltage = 0.0;
  uint64_t wattSeconds[32] = { 0 };
  uint64_t prevWattSeconds[32] = { 0 };
  uint32_t deltaWattSeconds[32] = { 0 };
  uint64_t polWattSeconds[32] = { 0 };
  uint64_t prevPolWattSeconds[32] = { 0 };
  float amps[32] = { 0 };
  uint16_t seconds = 0;
  uint16_t prevSeconds = 0;
  String serialNumber = "";
  uint16_t watts[32] = { 0 };
  uint16_t netWatts[32] = { 0 };
  float kwh[32] = { 0.0 };
  float totalKwh[32] = { 0.0 };
  float temp[8] = { 0.0 };
  uint64_t pulse[4] = { 0.0 };
};

struct EcmSettings {
  bool gotSettings = false;
  uint8_t ch1Set[2] = { 0 };
  uint8_t ch2Set[2] = { 0 };
  uint8_t ptSet[2] = { 0 };
  uint8_t sendInterval = 0;
  double firmwareVersion = 0.0;
  String serialNumber = "";
  bool auxX2[5] = { false };
  uint8_t aux5Option = 0;
};

DeviceData deviceData;

EcmSettings ecmSettings;

String gemSerial = "";
uint8_t deviceType = 0;

// Replace with your SSID and password
char ssid[32] = "";
char password[32] = "";
char apName[32] = "BrultechAP";
char apPassword[32] = "brultech";

// EEPROM memory addresses
const int eepromSize = 512;
const int ssidAddress = 0;
const int passwordAddress = 33;
const int isFirstRunAddress = 66;
const int tcpPortAddress = 68;
const int tcpIPAddress = 75;
const int tcpServerPortAddress = 100;
const int mqttPortAddress = 200;
const int mqttServerAddress = 207;
const int mqttUserAddress = 257;
const int mqttPassAddress = 277;
const int loginUserAddress = 297;
const int loginPassAddress = 317;
const int baudAddress = 350;
const int ipConfigAddress = 370;
const byte isFirstRunValue = 0xAA;

// Serial buffer
const int MAX_DATA_LENGTH = 1024;  // Set the maximum length of the data
char buffer[MAX_DATA_LENGTH];      // Declare the array to store the data
int dataLength = 0;                // Declare a variable to keep track of the length of the data

// Structure to hold the IP address configuration
struct IPAddressConfig {
  bool isConfigured = false;  // Flag indicating if configuration is stored
  IPAddress ip = IPAddress(192, 168, 1, 100);
  IPAddress gateway = IPAddress(192, 168, 1, 1);
  IPAddress subnet = IPAddress(255, 255, 255, 0);
  IPAddress dns1 = IPAddress(8, 8, 8, 8);  // DNS server 1
  IPAddress dns2 = IPAddress(8, 8, 4, 4);  // DNS server 2
};

WiFiClient client;
WiFiClient clientTwo;

ESP8266WebServer server(80);
ESP8266HTTPUpdateServer httpUpdater;
PubSubClient mqttClient(clientTwo);

String tcpIp = "";
int tcpPort = 0;

int tcpServerPort = 8000;
WiFiServer ecmServer(5555);
int baud = 19200;

String mqttServer = "";
String mqttUser = "";
String mqttPass = "";
String mqttClientID = "";
int mqttPort = 1883;

uint8_t mac[6];

String loginUser = "";
String loginPass = "";

const int LED_PIN = 5;    // GPIO5
const int RESET_PIN = 0;  // GPIO0
Ticker ticker;
bool ledState = false;

const int UPDATE_SIZE_UNKNOWN = -1;

unsigned long startTime = 0;
unsigned long endTime = 0;
unsigned long elapsedTime = 0;

void ICACHE_RAM_ATTR resetToAP() {
  if (digitalRead(RESET_PIN) == LOW) {
    ticker.attach(1, toggleLED);  // Start the thread
    resetMemory();
    delay(2000);
    ticker.detach();              // Stop the ticker
    digitalWrite(LED_PIN, HIGH);  // Turn off the LED
    ESP.restart();
  }
}

void toggleLED() {
  static bool ledState = false;
  ledState = !ledState;
  digitalWrite(LED_PIN, ledState);
}

void setup() {
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  ticker.attach(1, toggleLED);  // Start the thread

  WiFi.macAddress(mac);

  // Start EEPROM
  EEPROM.begin(eepromSize);

  // Check if this is the first run
  byte isFirstRun = EEPROM.read(isFirstRunAddress);
  if (isFirstRun != isFirstRunValue) {
    resetMemory();
  }

  // Read settings from EEPROM
  EEPROM.get(ssidAddress, ssid);
  EEPROM.get(passwordAddress, password);

  EEPROM.get(baudAddress, baud);

  // Read the stored IP address configuration
  IPAddressConfig storedIPConfig;
  EEPROM.get(ipConfigAddress, storedIPConfig);

  // Check if configuration is stored
  if (storedIPConfig.isConfigured) {
    // Set the static IP address and DNS configuration
    WiFi.config(
      storedIPConfig.ip,
      storedIPConfig.gateway,
      storedIPConfig.subnet,
      storedIPConfig.dns1,
      storedIPConfig.dns2);
  }

  if (baud != 19200 && baud != 115200) {
    baud = 19200;
  }

  // Start serial port
  Serial.begin(baud);
  Serial.flush();
  Serial.setTimeout(300);

  for (int i = 0; i < 4; i++) {
    // Read each byte of the IP address from EEPROM
    byte b = EEPROM.read(tcpIPAddress + i);
    // Add the byte to the IP string
    tcpIp += String(b);
    if (i < 3) {
      // Add the dot separator between bytes
      tcpIp += ".";
    }
  }


  loginUser = getString(loginUserAddress);
  loginPass = getString(loginPassAddress);

  mqttServer = getString(mqttServerAddress);
  mqttUser = getString(mqttUserAddress);
  mqttPass = getString(mqttPassAddress);

  EEPROM.get(mqttPort, mqttPortAddress);

  EEPROM.get(tcpPortAddress, tcpPort);
  EEPROM.get(tcpServerPortAddress, tcpServerPort);

  // Connect to saved network
  if (strcmp(ssid, "") != 0 && strcmp(password, "") != 0) {
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, password);

    int x = 0;

    while (WiFi.status() != WL_CONNECTED) {
      delay(1000);

      if (x > 20) {
        break;
      }

      x++;
    }
  }

  // If not connected, start in Access Point mode
  if (WiFi.status() != WL_CONNECTED) {
    // Start WiFi in Access Point mode
    WiFi.mode(WIFI_AP);
    WiFi.softAP(apName, apPassword);

    // Start web server
    server.on("/main", handleAP);
    server.on("/config", handleConfig);
  } else {
    ticker.detach();             // Stop the ticker
    digitalWrite(LED_PIN, LOW);  // Turn off the LED
    server.on("/main", handleStationMode);
    server.on("/config", handleConfig);
    server.on("/login-settings", handleLoginSettings);
    server.on("/serial-to-tcp", handleSerialToTcp);
    server.on("/ecm-settings", handleECMSettings);
    server.on("/serial-to-tcp-server", handleSerialToTcpServer);
    server.on("/mqtt", handleMqtt);
    server.on("/baud", handleBaud);
    server.on("/ip-config", handleIPConfig);
    server.on("/send-ha", handleHA);
    server.on("/data", handleData);
    server.on("/serial-debug", handleSerialDebug);
    server.on("/reboot", handleReboot);
    //server.on("/update", HTTP_POST, handleUpdate);
    MDNS.begin("esp8266");
    delay(1000);
  }

  server.on("/", handleRoot);
  server.on("/login", handleLogin);
  //here the list of headers to be recorded
  const char* headerkeys[] = { "User-Agent", "Cookie", "Content-Type", "Content-Length", "Update-Size" };
  size_t headerkeyssize = sizeof(headerkeys) / sizeof(char*);

  //ask server to track these headers
  server.collectHeaders(headerkeys, headerkeyssize);
  httpUpdater.setup(&server, loginUser, loginPass);
  server.begin();

  ecmServer.stop();
  ecmServer.begin(tcpServerPort);

  pinMode(RESET_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(RESET_PIN), resetToAP, FALLING);
}

void handleReboot() {
  // Root webpage
  String html = getHTMLHeader(0);
  html += "<div><h2>Rebooting the ESP8266, please wait..</h2></div>";
  html += "</body></html>";
  server.send(200, "text/html", html);

  ESP.restart();
}

void resetMemory() {
  for (int i = 0; i < eepromSize; i++) {
    EEPROM.write(i, 0);
  }
  EEPROM.write(isFirstRunAddress, isFirstRunValue);
  EEPROM.commit();
}

void loop() {
  MDNS.update();
  server.handleClient();
  startTime = micros();

  WiFiClient ecmClient = ecmServer.available();

  if (ecmClient) {
    ecmClient.setTimeout(100);
    while (ecmClient.connected()) {
      if (ecmClient.available()) {
        dataLength = ecmClient.readBytes(buffer, sizeof(buffer));  // Read all available data from WiFi and store it in the buffer

        Serial.write(buffer, dataLength);  // Send the entire buffer to the ECM-1240
      }

      if (Serial.available()) {
        dataLength = Serial.readBytes(buffer, sizeof(buffer));  // Read all available data from serial and store it in the buffer

        ecmClient.write(buffer, dataLength);  // Send the entire buffer to the server
      }
    }

    ecmClient.stop();
  } else if (client.connected()) {
    // Handle the data passthru
    if (Serial.available()) {
      dataLength = Serial.readBytes(buffer, sizeof(buffer));  // Read all available data from serial and store it in the buffer

      client.write(buffer, dataLength);  // Send the entire buffer to the server

      handlePacket();
    }

    if (client.available()) {
      dataLength = client.readBytes(buffer, sizeof(buffer));  // Read all available data from WiFi and store it in the buffer

      Serial.write(buffer, dataLength);  // Send the entire buffer to the ECM-1240
    }
  } else {
    if (!client.connected() && isValidIP(tcpIp) && tcpPort > 1024 && tcpPort < 65536) {
      client.connect(tcpIp.c_str(), tcpPort);
      client.setTimeout(100);
    } else {
      if (Serial.available()) {
        dataLength = Serial.readBytes(buffer, sizeof(buffer));  // Read all available data from serial and store it in the buffer

        handlePacket();
      }
    }
  }

  delay(10);
}

void handlePacket() {
  if (dataLength >= 65) {
    // Validate the first three bytes and last 2 bytes
    for (int y = 0; y < dataLength; y++) {
      if ((y + 63) < dataLength) {
        if (buffer[y] == 0xFE && buffer[y + 1] == 0xFF && buffer[y + 2] == 0x03 && buffer[y + 62] == 0xFF && buffer[y + 63] == 0xFE) {
          memcpy(deviceData.prevWattSeconds, deviceData.wattSeconds, sizeof(deviceData.prevWattSeconds));
          memcpy(deviceData.prevPolWattSeconds, deviceData.polWattSeconds, sizeof(deviceData.prevPolWattSeconds));

          deviceData.prevSeconds = deviceData.seconds;

          // Extract the voltage value as an unsigned integer
          deviceData.voltage = static_cast<float>((buffer[y + 3] << 8) | buffer[y + 4]) / 10;
          deviceData.seconds = ((uint16_t)buffer[y + 39] << 16) | ((uint16_t)buffer[y + 38] << 8) | (uint16_t)buffer[y + 37];


          // Extract the 5/4-byte value as an unsigned integer
          deviceData.wattSeconds[0] = ((uint64_t)buffer[y + 9] << 32) | ((uint64_t)buffer[y + 8] << 24) | ((uint64_t)buffer[y + 7] << 16) | ((uint64_t)buffer[y + 6] << 8) | (uint64_t)buffer[y + 5];
          deviceData.wattSeconds[1] = ((uint64_t)buffer[y + 14] << 32) | ((uint64_t)buffer[y + 13] << 24) | ((uint64_t)buffer[y + 12] << 16) | ((uint64_t)buffer[y + 11] << 8) | (uint64_t)buffer[y + 10];
          deviceData.wattSeconds[2] = ((uint64_t)buffer[y + 43] << 24) | ((uint64_t)buffer[y + 42] << 16) | ((uint64_t)buffer[y + 41] << 8) | (uint64_t)buffer[y + 40];
          deviceData.wattSeconds[3] = ((uint64_t)buffer[y + 47] << 24) | ((uint64_t)buffer[y + 46] << 16) | ((uint64_t)buffer[y + 45] << 8) | (uint64_t)buffer[y + 44];
          deviceData.wattSeconds[4] = ((uint64_t)buffer[y + 51] << 24) | ((uint64_t)buffer[y + 50] << 16) | ((uint64_t)buffer[y + 49] << 8) | (uint64_t)buffer[y + 48];
          deviceData.wattSeconds[5] = ((uint64_t)buffer[y + 55] << 24) | ((uint64_t)buffer[y + 54] << 16) | ((uint64_t)buffer[y + 53] << 8) | (uint64_t)buffer[y + 52];
          deviceData.wattSeconds[6] = ((uint64_t)buffer[y + 59] << 24) | ((uint64_t)buffer[y + 58] << 16) | ((uint64_t)buffer[y + 57] << 8) | (uint64_t)buffer[y + 56];

          deviceData.polWattSeconds[0] = ((uint64_t)buffer[y + 19] << 32) | ((uint64_t)buffer[y + 18] << 24) | ((uint64_t)buffer[y + 17] << 16) | ((uint64_t)buffer[y + 16] << 8) | (uint64_t)buffer[y + 15];
          deviceData.polWattSeconds[1] = ((uint64_t)buffer[y + 24] << 32) | ((uint64_t)buffer[y + 23] << 24) | ((uint64_t)buffer[y + 22] << 16) | ((uint64_t)buffer[y + 21] << 8) | (uint64_t)buffer[y + 20];

          deviceData.serialNumber = String((uint16_t)buffer[y + 32]) + String(((buffer[y + 29] << 8) | buffer[y + 30]));

          if (deviceData.prevSeconds != 0) {
            processPacket();
          }

          break;
        }
      }

      if ((y + 400) < dataLength) {
        if (buffer[y] == 0xFE && buffer[y + 1] == 0xFF && buffer[y + 2] == 0x07 && buffer[y + 426] == 0xFF && buffer[y + 427] == 0xFE) {

          memcpy(deviceData.prevWattSeconds, deviceData.wattSeconds, sizeof(deviceData.prevWattSeconds));
          memcpy(deviceData.prevPolWattSeconds, deviceData.polWattSeconds, sizeof(deviceData.prevPolWattSeconds));

          deviceData.prevSeconds = deviceData.seconds;

          // Extract the voltage value as an unsigned integer
          deviceData.voltage = static_cast<float>((buffer[y + 3] << 8) | buffer[y + 4]) / 10;
          deviceData.seconds = ((uint16_t)buffer[y + 395] << 16) | ((uint16_t)buffer[y + 394] << 8) | (uint16_t)buffer[y + 393];

          int v = 0;
          for (int z = 0; z < 32; z++) {
            deviceData.wattSeconds[z] = ((uint64_t)buffer[y + (z * 5) + 9] << 32) | ((uint64_t)buffer[y + (z * 5) + 8] << 24) | ((uint64_t)buffer[y + (z * 5) + 7] << 16) | ((uint64_t)buffer[y + (z * 5) + 6] << 8) | (uint64_t)buffer[y + (z * 5) + 5];
            deviceData.polWattSeconds[z] = ((uint64_t)buffer[y + (z * 5) + 169] << 32) | ((uint64_t)buffer[y + (z * 5) + 168] << 24) | ((uint64_t)buffer[y + (z * 5) + 167] << 16) | ((uint64_t)buffer[y + (z * 5) + 166] << 8) | (uint64_t)buffer[y + (z * 5) + 165];
            deviceData.amps[z] = static_cast<float>((uint16_t)buffer[y + (z * 2) + 330] << 8 | (uint16_t)buffer[y + (z * 2) + 329]) / 50;

            if (z < 8) {
              deviceData.temp[z] = static_cast<float>((uint16_t)buffer[y + (z * 2) + 409] << 8 | (uint16_t)buffer[y + (z * 2) + 408]) / 2;
              if (z < 4) {
                deviceData.pulse[z] = ((uint64_t)buffer[y + (z * 4) + 399] << 24) | ((uint64_t)buffer[y + (z * 4) + 398] << 16) | ((uint64_t)buffer[y + (z * 4) + 397] << 8) | (uint64_t)buffer[y + (z * 4) + 396];
              }
            }
          }

          String serial = String(((buffer[y + 325] << 8) | buffer[y + 326]));
          String id = String((uint16_t)buffer[y + 328]);

          if (id.length() < 3) {
            while (id.length() < 3) {
              id = "0" + id;
            }
          }

          if (serial.length() < 5) {
            while (serial.length() < 5) {
              serial = "0" + serial;
            }
          }

          deviceData.serialNumber = id + serial;

          if (deviceData.prevSeconds != 0) {
            processPacket();
          }

          break;
        }
      }
    }
  }
}

void processPacket() {
  uint16_t secDiff = 0;

  if (deviceData.prevSeconds > deviceData.seconds) {
    secDiff = deviceData.seconds + 256 ^ 3 - deviceData.prevSeconds;
  } else {
    secDiff = deviceData.seconds - deviceData.prevSeconds;
  }

  uint64_t wattSecDiff = 0;
  uint64_t polWattSecDiff = 0;

  uint8_t numChan = 7;
  uint8_t wsMulti = 5;

  if (deviceType == 2) {
    numChan = 32;
  }

  for (int x = 0; x < numChan; x++) {

    if (x == 2 && deviceType == 1) {
      wsMulti = 4;
    }

    if (deviceData.prevWattSeconds[x] > deviceData.wattSeconds[x]) {
      deviceData.deltaWattSeconds[x] = (deviceData.wattSeconds[x] + 256 ^ wsMulti - deviceData.prevWattSeconds[x]);
    } else {
      deviceData.deltaWattSeconds[x] = (deviceData.wattSeconds[x] - deviceData.prevWattSeconds[x]);
    }

    if (x < 2 || deviceType == 2) {
      if (deviceData.prevPolWattSeconds[x] > deviceData.polWattSeconds[x]) {
        polWattSecDiff = (deviceData.polWattSeconds[x] + 256 ^ wsMulti - deviceData.prevPolWattSeconds[x]);
      } else {
        polWattSecDiff = (deviceData.polWattSeconds[x] - deviceData.prevPolWattSeconds[x]);
      }

      deviceData.netWatts[x] = static_cast<float>(deviceData.deltaWattSeconds[x] - (2 * polWattSecDiff)) / secDiff;
    }

    deviceData.watts[x] = deviceData.deltaWattSeconds[x] / secDiff;
    deviceData.kwh[x] = static_cast<float>(deviceData.deltaWattSeconds[x]) / 3600000;
    deviceData.totalKwh[x] += deviceData.kwh[x];
  }

  mqttPost();
}


String serialDebug() {
  String html = "<h3>Serial Debug:</h3><br><b>Buffer Length:</b> " + String(dataLength) + "<br><br><b>Packet:</b><br><br>";
  char temp[3];  // Buffer to hold the formatted byte

  for (int i = 0; i < dataLength; i++) {
    sprintf(temp, "%02X", buffer[i]);
    html += String(temp) + " ";
  }

  return html;
}

void mqttPost() {
  if (WiFi.status() == WL_CONNECTED) {
    mqttClient.setServer(mqttServer.c_str(), mqttPort);

    String deviceName = "ECM1240-";
    uint8_t numChan = 7;

    if (deviceType == 2) {
      deviceName = "GEM-";
      numChan = 32;
    }

    if (mqttClientID == "") {
      mqttClientID = deviceName + String(mac[3], HEX) + String(mac[4], HEX) + String(mac[5], HEX);
    }

    if (!mqttClient.connect(mqttClientID.c_str(), mqttUser.c_str(), mqttPass.c_str())) {
      Serial.println("Failed to connect to MQTT broker");
      return;
    }

    // Publish topics
    mqttClient.publish((deviceName + String(deviceData.serialNumber) + "/v").c_str(), String(deviceData.voltage).c_str());

    for (int i = 0; i < numChan; i++) {
      mqttClient.publish((deviceName + String(deviceData.serialNumber) + "/c" + String(i + 1) + "/watt").c_str(), String(deviceData.watts[i]).c_str());
      mqttClient.publish((deviceName + String(deviceData.serialNumber) + "/c" + String(i + 1) + "/kwh").c_str(), String(deviceData.kwh[i], 5).c_str());
      mqttClient.publish((deviceName + String(deviceData.serialNumber) + "/c" + String(i + 1) + "/total_kwh").c_str(), String(deviceData.totalKwh[i], 5).c_str());
      mqttClient.publish((deviceName + String(deviceData.serialNumber) + "/c" + String(i + 1) + "/ws").c_str(), String(deviceData.wattSeconds[i]).c_str());
      mqttClient.publish((deviceName + String(deviceData.serialNumber) + "/c" + String(i + 1) + "/pws").c_str(), String(deviceData.polWattSeconds[i]).c_str());
      mqttClient.publish((deviceName + String(deviceData.serialNumber) + "/c" + String(i + 1) + "/dws").c_str(), String(deviceData.deltaWattSeconds[i]).c_str());

      if (deviceType == 2) {
        mqttClient.publish((deviceName + String(deviceData.serialNumber) + "/c" + String(i + 1) + "/amp").c_str(), String(deviceData.amps[i]).c_str());

        if (i < 8 && deviceType == 2) {
          mqttClient.publish((deviceName + String(deviceData.serialNumber) + "/t" + String(i + 1) + "/value").c_str(), String(deviceData.temp[i]).c_str());

          if (i < 4) {
            mqttClient.publish((deviceName + String(deviceData.serialNumber) + "/p" + String(i + 1) + "/value").c_str(), String(deviceData.pulse[i]).c_str());
          }
        }
      }
    }

    // Disconnect from MQTT broker
    mqttClient.disconnect();
  }
}

void handleHA() {
  if (WiFi.status() == WL_CONNECTED) {
    mqttClient.setServer(mqttServer.c_str(), mqttPort);

    String html = getHTMLHeader(0);

    String deviceName = "ECM1240-";
    uint8_t numChan = 7;

    if (deviceType == 2) {
      deviceName = "GEM-";
      numChan = 32;
    }

    if (mqttClientID == "") {
      mqttClientID = deviceName + String(mac[3], HEX) + String(mac[4], HEX) + String(mac[5], HEX);
    }

    if (!mqttClient.connect(mqttClientID.c_str(), mqttUser.c_str(), mqttPass.c_str())) {
      html += "<div><h3>MQTT couldn't connect, please check your settings.</h3></div></html></body>";
      server.send(200, "text/html", html);
      return;
    }
    html += "<div><h3>MQTT Values Sent:</h3>";
    String payload = "{\"unique_id\": \"" + deviceData.serialNumber + "v\", \"name\":\"" + deviceName + deviceData.serialNumber + " Volts\",\"state_topic\":\"" + deviceName + deviceData.serialNumber + "/v\",\"unit_of_measurement\":\"V\", \"state_class\": \"measurement\", \"dev\":{\"ids\":\"" + deviceData.serialNumber + "\",\"name\":\"" + deviceName + deviceData.serialNumber + "\",\"sw\":\"esp8266-custom\",\"mdl\":\"ECM-1240\",\"mf\":\"BrulTech Research Inc.\"}}";
    String topic = "homeassistant/sensor/" + deviceName + deviceData.serialNumber + "/volts/config";
    if (!mqttClient.publish(topic.c_str(), payload.c_str(), true)) {
      html += "Not Sent: " + String(sizeof(payload)) + " " + String(MQTT_MAX_PACKET_SIZE) + "<br><br>";
    }

    html += payload + "<br><br>" + topic + "<br><br>";

    for (int x = 0; x < numChan; x++) {
      payload = "{\"unique_id\": \"" + deviceData.serialNumber + "ch" + (x + 1) + "kwh\", \"name\":\"" + deviceName + deviceData.serialNumber + " CH" + (x + 1) + " kWh\",\"state_topic\":\"" + deviceName + deviceData.serialNumber + "/c" + (x + 1) + "/kwh\",\"unit_of_measurement\":\"kWh\", \"state_class\": \"measurement\", \"dev\":{\"ids\":\"" + deviceData.serialNumber + "\",\"name\":\"" + deviceName + deviceData.serialNumber + "\",\"sw\":\"esp8266-custom\",\"mdl\":\"ECM-1240\",\"mf\":\"BrulTech Research Inc.\"}}";
      topic = "homeassistant/sensor/" + deviceName + deviceData.serialNumber + "/ch" + (x + 1) + "_kwh/config";
      if (!mqttClient.publish(topic.c_str(), payload.c_str(), true)) {
        html += "Not Sent:<br><br>";
      }

      payload = "{\"unique_id\": \"" + deviceData.serialNumber + "ch" + (x + 1) + "_total_kwh\", \"name\":\"" + deviceName + deviceData.serialNumber + " CH" + (x + 1) + " Total kWh\",\"state_topic\":\"" + deviceName + deviceData.serialNumber + "/c" + (x + 1) + "/total_kwh\",\"unit_of_measurement\":\"kWh\", \"device_class\": \"energy\", \"state_class\": \"total_increasing\", \"last_reset\": \"1970-01-01T00:00:00+00:00\", \"dev\":{\"ids\":\"" + deviceData.serialNumber + "\",\"name\":\"" + deviceName + deviceData.serialNumber + "\",\"sw\":\"esp8266-custom\",\"mdl\":\"ECM-1240\",\"mf\":\"BrulTech Research Inc.\"}}";
      topic = "homeassistant/sensor/" + deviceName + deviceData.serialNumber + "/ch" + (x + 1) + "_total_kwh/config";
      if (!mqttClient.publish(topic.c_str(), payload.c_str(), true)) {
        html += "Not Sent:<br><br>";
      }


      html += payload + "<br><br>" + topic + "<br><br>";

      payload = "{\"unique_id\": \"" + deviceData.serialNumber + "ch" + (x + 1) + "ws\", \"name\":\"" + deviceName + deviceData.serialNumber + " CH" + (x + 1) + " WattSeconds\",\"state_topic\":\"" + deviceName + deviceData.serialNumber + "/c" + (x + 1) + "/dws\",\"unit_of_measurement\":\"WS\", \"state_class\": \"measurement\", \"dev\":{\"ids\":\"" + deviceData.serialNumber + "\",\"name\":\"" + deviceName + deviceData.serialNumber + "\",\"sw\":\"esp8266-custom\",\"mdl\":\"ECM-1240\",\"mf\":\"BrulTech Research Inc.\"}}";
      topic = "homeassistant/sensor/" + deviceName + deviceData.serialNumber + "/ch" + (x + 1) + "_ws/config";
      if (!mqttClient.publish(topic.c_str(), payload.c_str(), true)) {
        html += "Not Sent:<br><br>";
      }

      html += payload + "<br><br>" + topic + "<br><br>";

      payload = "{\"unique_id\": \"" + deviceData.serialNumber + "ch" + (x + 1) + "w\", \"name\":\"" + deviceName + deviceData.serialNumber + " CH" + (x + 1) + " Watts\",\"state_topic\":\"" + deviceName + deviceData.serialNumber + "/c" + (x + 1) + "/watt\",\"unit_of_measurement\":\"W\", \"state_class\": \"measurement\", \"dev\":{\"ids\":\"" + deviceData.serialNumber + "\",\"name\":\"" + deviceName + deviceData.serialNumber + "\",\"sw\":\"esp8266-custom\",\"mdl\":\"ECM-1240\",\"mf\":\"BrulTech Research Inc.\"}}";
      topic = "homeassistant/sensor/" + deviceName + deviceData.serialNumber + "/ch" + (x + 1) + "_watts/config";
      if (!mqttClient.publish(topic.c_str(), payload.c_str(), true)) {
        html += "Not Sent:<br><br>";
      }

      html += payload + "<br><br>" + topic + "<br><br>";

      if (deviceType == 2) {
        payload = "{\"unique_id\": \"" + deviceData.serialNumber + "ch" + (x + 1) + "a\", \"name\":\"" + deviceName + deviceData.serialNumber + " CH" + (x + 1) + " Amps\",\"state_topic\":\"" + deviceName + deviceData.serialNumber + "/c" + (x + 1) + "/amp\",\"unit_of_measurement\":\"A\", \"state_class\": \"measurement\", \"dev\":{\"ids\":\"" + deviceData.serialNumber + "\",\"name\":\"" + deviceName + deviceData.serialNumber + "\",\"sw\":\"esp8266-custom\",\"mdl\":\"ECM-1240\",\"mf\":\"BrulTech Research Inc.\"}}";
        topic = "homeassistant/sensor/" + deviceName + deviceData.serialNumber + "/ch" + (x + 1) + "_watts/config";
        if (!mqttClient.publish(topic.c_str(), payload.c_str(), true)) {
          html += "Not Sent:<br><br>";
        }

        html += payload + "<br><br>" + topic + "<br><br>";

        if (x < 8) {
          payload = "{\"unique_id\": \"" + deviceData.serialNumber + "t" + (x + 1) + "_value\", \"name\":\"" + deviceName + deviceData.serialNumber + " T" + (x + 1) + " Value\",\"state_topic\":\"" + deviceName + deviceData.serialNumber + "/t" + (x + 1) + "/value\",\"unit_of_measurement\":\"C\", \"state_class\": \"measurement\", \"dev\":{\"ids\":\"" + deviceData.serialNumber + "\",\"name\":\"" + deviceName + deviceData.serialNumber + "\",\"sw\":\"esp8266-custom\",\"mdl\":\"ECM-1240\",\"mf\":\"BrulTech Research Inc.\"}}";
          topic = "homeassistant/sensor/" + deviceName + deviceData.serialNumber + "/t" + (x + 1) + "_value/config";
          if (!mqttClient.publish(topic.c_str(), payload.c_str(), true)) {
            html += "Not Sent:<br><br>";
          }

          if (x < 4) {
            payload = "{\"unique_id\": \"" + deviceData.serialNumber + "p" + (x + 1) + "_value\", \"name\":\"" + deviceName + deviceData.serialNumber + " P" + (x + 1) + " Value\",\"state_topic\":\"" + deviceName + deviceData.serialNumber + "/p" + (x + 1) + "/value\",\"unit_of_measurement\":\"Pulses\", \"state_class\": \"measurement\", \"dev\":{\"ids\":\"" + deviceData.serialNumber + "\",\"name\":\"" + deviceName + deviceData.serialNumber + "\",\"sw\":\"esp8266-custom\",\"mdl\":\"ECM-1240\",\"mf\":\"BrulTech Research Inc.\"}}";
            topic = "homeassistant/sensor/" + deviceName + deviceData.serialNumber + "/p" + (x + 1) + "_value/config";
            if (!mqttClient.publish(topic.c_str(), payload.c_str(), true)) {
              html += "Not Sent:<br><br>";
            }
          }
        }
      }
    }

    // Publish the auto-discovery payload to the MQTT broker

    mqttClient.disconnect();
    html += "</div></body></html>";
    server.send(200, "text/html", html);
  }
}

void handleAP() {
  if (!isAuthenticated()) {
    sendLogin(false);
  } else {
    // Root webpage
    String html = getHTMLHeader(0);
    html += "<div><h2>Network Configuration</h2>";
    html += "<form action='/config'>";
    html += "<label>Select a network:</label> <select name='ssid'>";
    int networksFound = WiFi.scanNetworks();
    for (int i = 0; i < networksFound; i++) {
      html += "<option value='" + String(WiFi.SSID(i)) + "'>" + String(WiFi.SSID(i)) + "</option>";
    }
    html += "</select><br>";
    html += "<label>Or enter SSID:</label> <input class='full' type='text' name='custom_ssid'><br>";
    html += "<label>Password:</label> <input class='full' type='password' name='password'><br>";
    html += "<button class='button'>Connect to Network</button>";
    html += "</form></div>";
    html += "</body></html>";
    server.send(200, "text/html", html);
  }
}

void handleConfig() {
  // Save settings to EEPROM
  String ssidValue;
  if (server.arg("custom_ssid") != "") {
    // Use custom SSID if provided
    ssidValue = server.arg("custom_ssid");
  } else {
    // Use selected network
    ssidValue = server.arg("ssid");
  }

  String passwordValue = server.arg("password");

  ssidValue.toCharArray(ssid, 32);
  passwordValue.toCharArray(password, 32);

  EEPROM.put(ssidAddress, ssid);
  EEPROM.put(passwordAddress, password);

  EEPROM.commit();
  server.send(200, "text/html", "Configuration saved, connect back to your network...");

  // Reboot module
  ESP.restart();
}

void getDeviceSettings() {
  Serial.flush();
  bool tryGEM = false;

  byte data = 0xFC;  // binary 0xFC
  Serial.write(data);
  delay(100);
  Serial.write("SETRCV");
  delay(100);

  if (Serial.available()) {
    dataLength = Serial.readBytes(buffer, sizeof(buffer));  // Read all available data from serial and store it in the buffer

    if (dataLength > 32) {
      deviceType = 1;
      processECMSettings();
    } else {
      tryGEM = true;
    }
  } else {
    tryGEM = true;
  }

  if (tryGEM) {
    Serial.flush();
    Serial.write("^^^RQSSRN\r\n");
    delay(100);

    if (Serial.available()) {
      gemSerial = "";
      while (Serial.available()) {
        char c = Serial.read();  // Read a character
        gemSerial += c;          // Append the character to the string
      }

      deviceType = 2;
    }
  }
}

void processECMSettings() {
  // Validate the first three bytes and last 2 bytes
  if (buffer[0] == 0xFC && buffer[1] == 0xFC && buffer[2] == 0xFC) {
    // Extract the voltage value as an unsigned integer
    ecmSettings.gotSettings = true;

    ecmSettings.ch1Set[0] = (uint8_t)buffer[3];
    ecmSettings.ch1Set[1] = (uint8_t)buffer[4];

    ecmSettings.ch2Set[0] = (uint8_t)buffer[5];
    ecmSettings.ch2Set[1] = (uint8_t)buffer[6];

    ecmSettings.ptSet[0] = (uint8_t)buffer[7];
    ecmSettings.ptSet[1] = (uint8_t)buffer[8];

    ecmSettings.sendInterval = (uint8_t)buffer[9];

    ecmSettings.firmwareVersion = static_cast<double>((buffer[11] << 8) | buffer[12]) / 1000;

    ecmSettings.serialNumber = String((uint16_t)buffer[13]) + String(((buffer[15] << 8) | buffer[14]));
    int i = 0;
    for (i; i < 5; i++) {
      ecmSettings.auxX2[i] = (buffer[17] & (1 << i)) != 0;
    }

    if ((buffer[17] & (1 << i)) != 0) {
      ecmSettings.aux5Option = 1;
    } else if ((buffer[17] & (1 << ++i)) != 0) {
      ecmSettings.aux5Option = 3;
    } else {
      ecmSettings.aux5Option = 0;
    }
  }
}

void handleStationMode() {
  if (!isAuthenticated()) {
    sendLogin(false);
  } else {
    ecmSettings.gotSettings = false;
    getDeviceSettings();
    // Station mode webpage
    String html = getHTMLHeader(1);
    html += "<div><form action='/config'><h3>Brultech Config " + ver + "</h3></div>";
    html += "<div><form action='/config'><h3>Network Settings</h3>";
    html += "<h4>Connected to: " + String(WiFi.SSID()) + "</h4>";
    html += "<label>Select a network: <select name='ssid'>";
    int networksFound = WiFi.scanNetworks();
    for (int i = 0; i < networksFound; i++) {
      html += "<option value='" + String(WiFi.SSID(i)) + "'>" + String(WiFi.SSID(i)) + "</option>";
    }
    html += "</select>";
    html += "<label>or enter SSID:</label><input class='full' type='text' name='custom_ssid' value='" + String(ssid) + "'>";
    html += "<label>Password:</label><input class='full' type='password' name='password' value=''>";
    html += "<button class='button'>Submit</button>";
    html += "</form></div>";
    html += "<div><form action='/ip-config'><h3>IP Address Settings</h3>";
    html += "<label>IP address:</label><input class='full' type='text' name='ip' value='" + WiFi.localIP().toString() + "'>";
    html += "<label>Subnet:</label><input class='full' type='text' name='subnet' value='" + WiFi.subnetMask().toString() + "'>";
    html += "<label>Gateway:</label><input class='full' type='text' name='gateway' value='" + WiFi.gatewayIP().toString() + "'>";
    html += "<label>DNS1:</label><input class='full' type='text' name='dns1' value='" + WiFi.dnsIP(0).toString() + "'>";
    html += "<label>DNS2:</label><input class='full' type='text' name='dns2' value='" + WiFi.dnsIP(1).toString() + "'>";
    html += "<button class='button'>Save Settings</button>";
    html += "</form></div>";
    html += "<div><form action='/baud'><h3>Change Baud Rate</h3>";
    html += "19200: <input type='radio' name='baud' value='19200'";

    if (baud == 19200) {
      html += " checked='checked'";
    }

    html += "> 115200: <input type='radio' name='baud' value='115200'";

    if (baud == 115200) {
      html += " checked='checked'";
    }

    html += "><button class='button'>Save Baudrate</button>";
    html += "</form></div>";
    html += "<div><form action='/serial-to-tcp'><h3>Serial to TCP Client</h3>";
    html += "<label>IP address:</label><input class='full' type='text' name='ip' value=''>";
    html += "<label>Port:</label><input class='full' type='number' name='port' value=''>";
    html += "<button class='button'>Connect</button>";
    html += "</form></div>";
    html += "<div><form action='/serial-to-tcp-server'><h3>TCP Server Connection</h3>";
    html += "<label>Port:</label><input class='full' type='number' name='port' value='" + String(tcpServerPort) + "'>";
    html += "<button class='button'>Save</button>";
    html += "</form></div>";
    html += "<div><form action='/mqtt'><h3>MQTT Server Connection</h3>";
    html += "<label>IP address/Domain:</label><input class='full' type='text' name='ip' value='" + String(mqttServer) + "'>";
    html += "<label>Port:</label><input class='full' type='number' name='port' value='" + String(mqttPort) + "'>";
    html += "<label>User:</label><input class='full' type='text' name='user' value='" + String(mqttUser) + "'>";
    html += "<label>Password:</label><input class='full' type='password' name='pass' value=''>";
    html += "<button class='button'>Save</button>";
    html += "</form><form action='/send-ha'><h3>Home-Assistant Config</h3>";
    html += "<button class='button'>Send Config</button>";
    html += "</form></div>";
    html += "<div><form action='/login-settings'><h3>Login Information</h3>";
    html += "<label>User:</label><input class='full' type='text' name='user' value='" + String(loginUser) + "'>";
    html += "<label>Password:</label><input class='full' type='password' name='pass' value=''>";
    html += "<button class='button'>Save</button>";
    html += "</form></div>";

    if (deviceType == 1) {
      html += "<div><form action='/ecm-settings'><h3>ECM Settings</h3>Type is a fine-tune value that increases the sensed value with each tick (255 Max). Range halves the sensed value with each increase.";
      html += "<label>Settings Retrieved?</label>" + boolToText(ecmSettings.gotSettings, false);
      html += "<label>Serial Number:</label>" + ecmSettings.serialNumber;
      html += "<label>Firmware Version:</label>" + String(ecmSettings.firmwareVersion, 4);
      html += "<label>Packet Send Interval:</label><input name='packet_send' class='small' type='number' min='1' max='255' value='" + String(ecmSettings.sendInterval) + "'> (Max 255)";
      html += "<label>Channel 1 Config:</label>Type: <input name='ch1type' class='small' type='number' min='1' max='255' value='" + String(ecmSettings.ch1Set[0]) + "'> Range: <input class='small' name='ch1range' type='number' min='1' max='255' value='" + String(ecmSettings.ch1Set[1]) + "'>";
      html += "<label>Channel 2 Config:</label>Type: <input name='ch2type' class='small' type='number' min='1' max='255' value='" + String(ecmSettings.ch2Set[0]) + "'> Range: <input class='small' name='ch2range' type='number' min='1' max='255' value='" + String(ecmSettings.ch2Set[1]) + "'>";
      html += "<label>PT (Voltage) Config:</label>Type: <input name='pttype' class='small' type='number' min='1' max='255' value='" + String(ecmSettings.ptSet[0]) + "'>  Range: <input name='ptrange' class='small' type='number' min='1' max='255' value='" + String(ecmSettings.ptSet[1]) + "'>";
      html += "<label>AUX Channel Double:</label>AUX1:  <input type='checkbox' name='aux1x2' " + boolToText(ecmSettings.auxX2[0], true) + "> AUX2: <input type='checkbox' name='aux2x2' " + boolToText(ecmSettings.auxX2[1], true) + "> AUX3: <input type='checkbox' name='aux3x2' " + boolToText(ecmSettings.auxX2[2], true) + "> AUX4:  <input type='checkbox' name='aux4x2' " + boolToText(ecmSettings.auxX2[3], true) + "> AUX5: <input type='checkbox' name='aux5x2' " + boolToText(ecmSettings.auxX2[4], true) + " ><br>";
      html += "<label>Aux 5 Options:</label>Power: <input name='aux5option' " + aux5Opt(0) + " type='radio' value='0'>  Pulse: <input name='aux5option' " + aux5Opt(1) + " type='radio' value='1'> DC: <input name='aux5option' " + aux5Opt(3) + " type='radio' value='3'>";
      html += "<button class='button'>Update Settings</button></form></div>";
    } else if (deviceType == 2) {
      html += "<div><b>GreenEye Monitor Detected:</b> " + String(gemSerial) + "<br><br><br>";

      if (tcpServerPort > 0) {
        html += "<a class='button' href='http://" + WiFi.localIP().toString() + ":" + tcpServerPort + "/'>Click Here for Setup</a>";
      } else {
        html += "Setup TCP Server for setup.";
      }

      html += "</div>";
    } else {
      html += "<div>No monitor detected.<br>If connected to a GreenEye Monitor try using the other baud rate.</div>";
    }

    html += "<div id='data'>";
    html += getData();
    html += "</div>";

    html += "<div><b><a href='/update' class='button'>Update Firmware</a></b></div><div id='serialDebug'>";
    html += serialDebug();
    html += "</div></body></html>";
    server.send(200, "text/html", html);
  }
}

String aux5Opt(uint8_t opt) {
  if (ecmSettings.aux5Option == opt) {
    return "checked='checked'";
  } else {
    return "";
  }
}


String getData() {
  String html = "<h3>Data:</h3><br>";

  if (deviceType == 1) {

    /*if (deviceData.validPacket) {
    html += "<b>Valid Packet:</b> True<br>";
  } else {
    html += "<b>Valid Packet:</b> False<br>";
  }*/

    html += "<b>Serial:</b> " + String(deviceData.serialNumber) + "  <b>Voltage:</b> " + String(deviceData.voltage);
    html += "<br><b>Prev. Seconds:</b> " + String(deviceData.prevSeconds) + " <b>Seconds:</b> " + String(deviceData.seconds);
    html += "<br><h3>Wattseconds:</h3>CH1: " + String(deviceData.wattSeconds[0]) + " CH2: " + String(deviceData.wattSeconds[1]);
    html += "<br>CH3: " + String(deviceData.wattSeconds[2]) + " CH4: " + String(deviceData.wattSeconds[3]);
    html += "<br>CH5: " + String(deviceData.wattSeconds[4]) + " CH6: " + String(deviceData.wattSeconds[5]);
    html += "<br>CH7: " + String(deviceData.wattSeconds[6]);
    html += "<br><h3>Prev Wattseconds:</h3>CH1: " + String(deviceData.prevWattSeconds[0]) + " CH2: " + String(deviceData.prevWattSeconds[1]);
    html += "<br>CH3: " + String(deviceData.prevWattSeconds[2]) + " CH4: " + String(deviceData.prevWattSeconds[3]);
    html += "<br>CH5: " + String(deviceData.prevWattSeconds[4]) + " CH6: " + String(deviceData.prevWattSeconds[5]);
    html += "<br>CH7: " + String(deviceData.prevWattSeconds[6]);
    html += "<br><h3>Polarized Wattseconds:</h3>CH1: " + String(deviceData.polWattSeconds[0]) + " CH2: " + String(deviceData.polWattSeconds[1]);
    html += "<br><h3>Watts:</h3>CH1: " + String(deviceData.watts[0]) + " CH2: " + String(deviceData.watts[1]);
    html += "<br>CH3: " + String(deviceData.watts[2]) + " CH4: " + String(deviceData.watts[3]);
    html += "<br>CH5: " + String(deviceData.watts[4]) + " CH6: " + String(deviceData.watts[5]);
    html += "<br>CH7: " + String(deviceData.watts[6]);
    html += "<br><h3>Net Watts:</h3>CH1: " + String(deviceData.netWatts[0]) + " CH2: " + String(deviceData.netWatts[1]);
    html += "<br><h3>kWh:</h3>CH1: " + String(deviceData.kwh[0], 5) + " CH2: " + String(deviceData.kwh[1], 5);
    html += "<br>CH3: " + String(deviceData.kwh[2], 5) + " CH4: " + String(deviceData.kwh[3], 5);
    html += "<br>CH5: " + String(deviceData.kwh[4], 5) + " CH6: " + String(deviceData.kwh[5], 5);
    html += "<br>CH7: " + String(deviceData.kwh[6], 5);
    html += "<br><h3>Total kWh (since last boot):</h3>CH1: " + String(deviceData.totalKwh[0], 5) + " CH2: " + String(deviceData.totalKwh[1], 5);
    html += "<br>CH3: " + String(deviceData.totalKwh[2], 5) + " CH4: " + String(deviceData.totalKwh[3], 5);
    html += "<br>CH5: " + String(deviceData.totalKwh[4], 5) + " CH6: " + String(deviceData.totalKwh[5], 5);
    html += "<br>CH7: " + String(deviceData.totalKwh[6], 5);
    html += "<br><h3>Delta Wattsec:</h3>CH1: " + String(deviceData.deltaWattSeconds[0], 5) + " CH2: " + String(deviceData.deltaWattSeconds[1], 5);
    html += "<br>CH3: " + String(deviceData.deltaWattSeconds[2], 5) + " CH4: " + String(deviceData.deltaWattSeconds[3], 5);
    html += "<br>CH5: " + String(deviceData.deltaWattSeconds[4], 5) + " CH6: " + String(deviceData.deltaWattSeconds[5], 5);
    html += "<br>CH7: " + String(deviceData.deltaWattSeconds[6], 5);
  } else if (deviceType == 2) {

    html += "<table>";
    html += "<tr><td style='width:125px; padding-bottom:5px;'><b>Serial Number</b></td><td>" + deviceData.serialNumber + "</td></tr>";
    html += "<tr><td><b>Seconds</b></td><td>" + String(deviceData.seconds) + "</td></tr>";
    html += "<tr><td><b>Previous Seconds</b></td><td>" + String(deviceData.prevSeconds) + "</td></tr>";
    html += "<tr><td><b>Voltage</b></td><td>" + String(deviceData.voltage) + "</td></tr>";
    html += "<tr><td><b>Wattseconds</b></td><td>";
    for (int i = 0; i < 32; i++) {
      html += "C" + String(i + 1) + ": " + String(deviceData.wattSeconds[i]) + " ";
    }
    html += "</td></tr>";

    html += "<tr><td><b>Prev. WattSeconds</b></td><td>";
    for (int i = 0; i < 32; i++) {
      html += "C" + String(i + 1) + ": " + String(deviceData.prevWattSeconds[i]) + " ";
    }
    html += "</td></tr>";

    html += "<tr><td><b>Polarized WattSeconds</b></td><td>";
    for (int i = 0; i < 32; i++) {
      html += "C" + String(i + 1) + ": " + String(deviceData.polWattSeconds[i]) + " ";
    }
    html += "</td></tr>";

    html += "<tr><td><b>Prev. Pol. WattSeconds</b></td><td>";
    for (int i = 0; i < 32; i++) {
      html += "C" + String(i + 1) + ": " + String(deviceData.prevPolWattSeconds[i]) + " ";
    }
    html += "</td></tr>";

    html += "<tr><td><b>Amps</b></td><td>";
    for (int i = 0; i < 32; i++) {
      html += "C" + String(i + 1) + ": " + String(deviceData.amps[i]) + " ";
    }
    html += "</td></tr>";

    html += "<tr><td><b>Watts</b></td><td>";
    for (int i = 0; i < 32; i++) {
      html += "C" + String(i + 1) + ": " + String(deviceData.watts[i]) + " ";
    }
    html += "</td></tr>";

    html += "<tr><td><b>Net Watts</b></td><td>";
    for (int i = 0; i < 32; i++) {
      html += "C" + String(i + 1) + ": " + String(deviceData.netWatts[i]) + " ";
    }
    html += "</td></tr>";

    html += "<tr><td><b>kWh</b></td><td>";
    for (int i = 0; i < 32; i++) {
      html += "C" + String(i + 1) + ": " + String(deviceData.kwh[i], 4) + " ";
    }
    html += "</td></tr>";

    html += "<tr><td><b>Total kWh</b></td><td>";
    for (int i = 0; i < 32; i++) {
      html += "C" + String(i + 1) + ": " + String(deviceData.totalKwh[i], 4) + " ";
    }
    html += "</td></tr>";

    html += "<tr><td><b>Temperature</b></td><td>";
    for (int i = 0; i < 8; i++) {
      html += "C" + String(i + 1) + ": " + String(deviceData.temp[i], 2) + " ";
    }
    html += "</td></tr>";

    html += "<tr><td><b>Pulse</b></td><td>";
    for (int i = 0; i < 4; i++) {
      html += "C" + String(i + 1) + ": " + String(deviceData.pulse[i]) + " ";
    }
    html += "</td></tr>";

    html += "</table>";
  } else {
    html += "No Device Detected";
  }

  return html;
}

String getHTMLHeader(uint8_t pageNum) {
  String htmlHeader = "<html>";
  htmlHeader += "<head>";
  htmlHeader += "<style>";
  htmlHeader += "body { background-color:#c3c3c3; font-family: Arial, sans-serif; }";
  htmlHeader += "div { background-color: #fff; border: 1px solid #ccc; box-shadow: 0 2px 2px rgba(0, 0, 0, 0.1); margin: 50px auto; max-width: 400px; padding: 20px; text-align: center; }";
  htmlHeader += "h1 { margin: 0 0 20px 0; }";
  htmlHeader += "label { padding-top:5px; display: block; font-size: 16px; font-weight: bold; margin-bottom: 5px; text-align: left; }";
  htmlHeader += ".full { box-sizing: border-box; border: 1px solid #ccc; font-size: 16px; padding: 10px; width: 100% ; }";
  htmlHeader += ".small { box-sizing: border-box; border: 1px solid #ccc; font-size: 16px; padding: 10px; width: 100px ; }";
  htmlHeader += ".button { background-color: #4CAF50; border: none; color: #fff; cursor: pointer; font-size: 16px; margin-top: 20px; padding: 10px; width: 100% ; text-decoration: none; }";
  htmlHeader += ".button:hover { background-color: #45a049; }";
  htmlHeader += "p.error { color: #f00; font-size: 14px; margin: 10px 0; text-align: left; }";
  htmlHeader += "</style>";

  if (pageNum == 1) {
    htmlHeader += "<script>";
    htmlHeader += "function updateDiv() {";
    htmlHeader += "const xhr = new XMLHttpRequest();";
    htmlHeader += "xhr.onreadystatechange = function() {";
    htmlHeader += "if (this.readyState === 4 && this.status === 200) {";
    htmlHeader += "document.getElementById(\"data\").innerHTML = this.responseText;";
    htmlHeader += "  }";
    htmlHeader += "};";
    htmlHeader += "xhr.open(\"GET\", \"/data\");";
    htmlHeader += "xhr.send();";
    htmlHeader += "const xhrTwo = new XMLHttpRequest();";
    htmlHeader += "xhrTwo.onreadystatechange = function() {";
    htmlHeader += "if (this.readyState === 4 && this.status === 200) {";
    htmlHeader += "document.getElementById(\"serialDebug\").innerHTML = this.responseText;";
    htmlHeader += "  }";
    htmlHeader += "};";
    htmlHeader += "xhrTwo.open(\"GET\", \"/serial-debug\");";
    htmlHeader += "xhrTwo.send();";
    htmlHeader += "}";
    htmlHeader += "setInterval(updateDiv, 5000);";
    htmlHeader += "</script>";
  }

  htmlHeader += "</head>";
  htmlHeader += "<body>";

  return htmlHeader;
}

void handleData() {
  server.send(200, "text/html", getData());
}

void handleSerialDebug() {
  server.send(200, "text/html", serialDebug());
}

void handleECMSettings() {
  uint8_t packet_send = server.arg("packet_send").toInt();
  uint8_t ch1type = server.arg("ch1type").toInt();
  uint8_t ch1range = server.arg("ch1range").toInt();
  uint8_t ch2type = server.arg("ch2type").toInt();
  uint8_t ch2range = server.arg("ch2range").toInt();
  uint8_t pttype = server.arg("pttype").toInt();
  uint8_t ptrange = server.arg("ptrange").toInt();
  bool auxX2[5] = { server.arg("aux1x2").equals("on"), server.arg("aux2x2").equals("on"), server.arg("aux3x2").equals("on"), server.arg("aux4x2").equals("on"), server.arg("aux5x2").equals("on") };
  uint8_t aux5Option = server.arg("aux5option").toInt();
  double fwVer = (double)ecmSettings.firmwareVersion;
  String sendSettings = "";

  bool auxChange = false;

  uint8_t aux5Bits = 0;
  int i = 0;

  for (i; i < 5; i++) {
    if (auxX2[i]) {
      aux5Bits |= 1 << i;
    }

    if (auxX2[i] != ecmSettings.auxX2[i]) {
      auxChange = true;
    }
  }

  if (aux5Option == 1) {
    aux5Bits |= 1 << i;
  } else if (aux5Option == 3) {
    aux5Bits |= 1 << ++i;
  }

  if (aux5Option != ecmSettings.aux5Option) {
    auxChange = true;
  }

  int aux5Value = aux5Bits;

  Serial.println(aux5Value);

  // Generate the HTML page with the variable values
  String html = getHTMLHeader(0) + "<div><h3>Settings saved.</h3><br>";
  String commandsSent = "";

  byte data = 0xFC;  // binary 0xFC
  if (fwVer > 1.031) {
    if (packet_send != ecmSettings.sendInterval || ch1type != ecmSettings.ch1Set[0] || ch1range != ecmSettings.ch1Set[1] || ch2type != ecmSettings.ch2Set[0] || ch2range != ecmSettings.ch2Set[1] || pttype != ecmSettings.ptSet[0] || ptrange != ecmSettings.ptSet[1]) {
      sendSettings = "SETALL1," + zeroPad(String(ch1type), 3) + ",";
      sendSettings += zeroPad(String(ch1range), 3) + ",";
      sendSettings += zeroPad(String(ch2type), 3) + ",";
      sendSettings += zeroPad(String(ch2range), 3) + ",";
      sendSettings += zeroPad(String(pttype), 3) + ",";
      sendSettings += zeroPad(String(ptrange), 3) + ",";
      sendSettings += zeroPad(String(aux5Value), 3) + ",";
      sendSettings += zeroPad(String(packet_send), 3);
      commandsSent += "SETALL" + sendSettings;
      Serial.write(data);
      delay(50);
      Serial.write(sendSettings.c_str());
    }
  } else {
    if (packet_send != ecmSettings.sendInterval) {
      Serial.write(data);
      delay(50);
      Serial.write("SET");
      delay(50);
      Serial.write("IV2");
      delay(50);
      Serial.write((char)packet_send);
      delay(50);

      commandsSent += "SETIV2" + String(packet_send) + "<br>";
    }

    if (ch1type != ecmSettings.ch1Set[0] || ch1range != ecmSettings.ch1Set[1]) {
      Serial.write(data);
      delay(50);
      Serial.write("SET");
      delay(50);
      Serial.write("CT1");
      delay(50);
      Serial.write("TYP");
      delay(50);
      Serial.write((char)ch1type);
      delay(50);
      Serial.write("RNG");
      delay(50);
      Serial.write((char)ch1range);
      delay(50);
      commandsSent += "SETCT1TYP" + String(ch1type) + "RNG" + String(ch1range) + "<br>";
    }

    if (ch2type != ecmSettings.ch2Set[0] || ch2range != ecmSettings.ch2Set[1]) {
      Serial.write(data);
      delay(50);
      Serial.write("SET");
      delay(50);
      Serial.write("CT2");
      delay(50);
      Serial.write("TYP");
      delay(50);
      Serial.write((char)ch2type);
      delay(50);
      Serial.write("RNG");
      delay(50);
      Serial.write((char)ch2range);
      delay(50);
      commandsSent += "SETCT2TYP" + String(ch2type) + "RNG" + String(ch2range) + "<br>";
    }

    if (pttype != ecmSettings.ptSet[0] || ptrange != ecmSettings.ptSet[1]) {
      Serial.write(data);
      delay(50);
      Serial.write("SET");
      delay(50);
      Serial.write("PTT");
      delay(50);
      Serial.write((char)pttype);
      delay(50);
      Serial.write("PTR");
      delay(50);
      Serial.write((char)ptrange);
      delay(50);
      commandsSent += "SETPTT" + String(pttype) + "PTR" + String(ptrange) + "<br>";
    }

    if (auxChange) {
      Serial.write(data);
      delay(50);
      Serial.write("SET");
      delay(50);
      Serial.write("OPT");
      delay(50);
      Serial.write((char)aux5Value);
      commandsSent += "SETOPT" + String(aux5Value) + "<br>";
    }
  }

  html += commandsSent;
  html += "Click <a href='/main'>here</a> to return to the settings page.</div>";
  html += "</body></html>";

  server.send(200, "text/html", html);
}

String boolToText(bool check, bool input) {
  if (check) {
    if (!input) {
      return "True";
    } else {
      return "checked='checked'";
    }
  } else {
    if (!input) {
      return "False";
    } else {
      return "";
    }
  }
}

void handleRoot() {
  if (!isAuthenticated()) {
    sendLogin(false);
  } else {
    // Redirect to the dashboard page if authenticated
    server.sendHeader("Location", "/main");
    server.send(302);
  }
}

void sendLogin(bool error) {
  // Send the login HTML page if not authenticated
  String html = getHTMLHeader(0) + "<div>";

  if (error) {
    html += "<p class='error'>Invalid username or password.</p>";
  }
  html += "<form action='/login' method='post'> <label for='username'>Username:</label> <input class='full' type='text' id='username' name='username'> <label for='password'>Password:</label> <input class='full' type='password' id='password' name='password'> <button class='button' type='submit'>Login</button> </form> </div> </body> </html>";
  server.send(200, "text/html", html);
}

void handleLogin() {
  String username = server.arg("username");
  String password = server.arg("password");

  // Replace with your authentication logic
  if (username == loginUser && password == loginPass) {
    // Set the session cookie and redirect to the dashboard page
    server.sendHeader("Set-Cookie", "session_id=1; Max-Age=7200; HttpOnly");
    server.sendHeader("Location", "/main");
    server.send(302);
  } else {
    // Send the login HTML page with an error message if authentication failed
    sendLogin(true);
  }
}

void handleSerialToTcp() {
  // Serial to TCP client connection
  String ip = server.arg("ip");
  int port = server.arg("port").toInt();
  IPAddress ipStore;

  String html = getHTMLHeader(0);

  // Error test the client connection
  if (ipStore.fromString(ip) && port > 1024 && port < 65536) {

    // Store in EEPROM
    for (int i = 0; i < 4; i++) {
      EEPROM.write(tcpIPAddress + i, ipStore[i]);
    }

    EEPROM.put(tcpPortAddress, port);
    EEPROM.commit();

    tcpIp = ip;
    tcpPort = port;

    html += "<h2>IP: " + ip + " Port: " + port + " saved to EEPROM.  Starting server...</h2>";

    if (client.connected()) {
      client.stop();

      if (client.connect(tcpIp.c_str(), tcpPort)) {
        client.setTimeout(100);
        html += "<h5>Connected to TCP server</h5>";
      } else {
        html += "<h5>Connection failed</h5>";
      }
    }

  } else {
    html += "<h2>Invalid IP Address or Port</h2>";
  }

  html += "</body></html>";
  server.send(200, "text/html", html);
}

void handleSerialToTcpServer() {
  // Serial to TCP client connection
  int port = server.arg("port").toInt();

  String html = getHTMLHeader(0);

  // Error test the client connection
  if (port > 1024 && port < 65536) {

    EEPROM.put(tcpServerPortAddress, port);
    EEPROM.commit();

    tcpServerPort = port;

    html += "<h2>Port saved to EEPROM.  Starting server...</h2>";

    ecmServer.stop();
    ecmServer.begin(tcpServerPort);
  } else {
    html += "<h2>Port is out of range.</h2>";
  }

  html += "</body></html>";
  server.send(200, "text/html", html);
}

void handleMqtt() {
  // Serial to TCP client connection
  String ip = server.arg("ip");
  String user = server.arg("user");
  String pass = server.arg("pass");
  int port = server.arg("port").toInt();

  String html = getHTMLHeader(0);

  // Error test the client connection
  if (port > 1024 && port < 65536) {
    storeString(ip, mqttServerAddress);
    storeString(user, mqttUserAddress);
    storeString(pass, mqttPassAddress);

    EEPROM.put(mqttPortAddress, port);
    EEPROM.commit();

    mqttServer = ip;
    mqttPort = port;
    mqttUser = user;
    mqttPass = pass;


    html += "<div><h3>Address: " + mqttServer + " Port: " + String(port) + " User: " + String(mqttUser) + " Pass:  " + String(mqttPass) + " saved to EEPROM.</h3></div>";
    if (!mqttClient.connect(mqttClientID.c_str(), mqttUser.c_str(), mqttPass.c_str())) {
      html += "<div><h3>MQTT couldn't connect, please check your settings.</h3></div>";
    } else {
      mqttClient.disconnect();
    }
  } else {
    html += "<div><h3>Invalid Port</h3></div>";
  }

  html += "</body></html>";
  server.send(200, "text/html", html);
}

void handleBaud() {
  // Serial to TCP client connection
  int baudRate = server.arg("baud").toInt();

  String html = getHTMLHeader(0);

  // Error test the client connection
  if (baudRate == 19200 || baudRate == 115200) {
    EEPROM.put(baudAddress, baudRate);
    EEPROM.commit();

    if (baudRate != baud) {
      Serial.updateBaudRate(baudRate);
    }

    baud = baudRate;

    html += "<div><h3>Baudrate " + String(baudRate) + " saved to EEPROM.</h3></div>";
  } else {
    html += "<div><h3>Invalid Baudrate</h3></div>";
  }

  html += "</body></html>";
  server.send(200, "text/html", html);
}

void handleIPConfig() {
  IPAddressConfig config;
  String html = getHTMLHeader(0);

  // Parse form data and validate IP settings
  if (config.ip.fromString(server.arg("ip")) && config.subnet.fromString(server.arg("subnet")) && config.gateway.fromString(server.arg("gateway")) && config.dns1.fromString(server.arg("dns1")) && config.dns2.fromString(server.arg("dns2"))) {
    config.isConfigured = true;

    // Write the IP settings structure to EEPROM
    EEPROM.put(ipConfigAddress, config);

    EEPROM.commit();  // Save changes to EEPROM

    html += "<div><h3>IP settings saved, click <a href='http://" + config.ip.toString() + "/'>here</a> to access the unit.<br>If you can't access the module afterwards it can be reset by using the push button.</h3></div></body></html>";

    server.send(200, "text/html", html);

    WiFi.config(config.ip, config.gateway, config.subnet, config.dns1, config.dns2);
  } else {
    html += "<div><h3>Invalid IP Address, Subnet, Gateway, or DNS.</h3></div></body></html>";

    server.send(200, "text/html", html);
  }
}

void handleLoginSettings() {
  // Serial to TCP client connection
  String user = server.arg("user");
  String pass = server.arg("pass");

  String html = getHTMLHeader(0);

  storeString(user, loginUserAddress);
  storeString(pass, loginPassAddress);

  loginUser = user;
  loginPass = pass;

  httpUpdater.setup(&server, loginUser, loginPass);

  html += "<div><h3>User: " + String(loginUser) + " Pass:  " + String(loginPass) + " saved to EEPROM.</h3></div></body></html>";

  server.send(200, "text/html", html);
}

void storeString(String store, int location) {
  for (int i = 0; i < store.length(); i++) {
    EEPROM.write(location++, store[i]);
  }

  EEPROM.write(location, '\0');  // add null terminator to end of string
  EEPROM.commit();
}

String getString(int location) {
  String readString = "";
  char c = EEPROM.read(location++);
  while (c != '\0' && location < EEPROM.length()) {
    readString += c;
    c = EEPROM.read(location++);
  }

  return readString.c_str();
}

String zeroPad(String str, int desiredLength) {
  while (str.length() < desiredLength) {
    str = "0" + str;
  }
  return str;
}

bool isValidIP(String str) {
  IPAddress ipCheck;
  return ipCheck.fromString(str);
}

bool isAuthenticated() {
  if (loginUser != "" && loginPass != "") {
    // Check if the session cookie is set and valid
    String cookie = server.header("Cookie");

    if (cookie.indexOf("session_id=1") != -1) {
      return true;
    } else {
      return false;
    }
  } else {
    return true;
  }
}
