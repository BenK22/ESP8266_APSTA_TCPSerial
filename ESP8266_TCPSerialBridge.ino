#include <ESP8266WiFi.h>
#include <EEPROM.h>
#include <ESP8266WebServer.h>
#include <PubSubClient.h>
#include <ESP8266mDNS.h>
#include <ESP8266HTTPUpdateServer.h>
#include <Ticker.h>
#include <WiFiUdp.h>
#include <ArduinoJson.h>
#include <NTPClient.h>

#define LEAP_YEAR(Y) ((Y > 0) && !(Y % 4) && ((Y % 100) || !(Y % 400)))

const String ver = "v1.99c";

// EEPROM memory locations
const int eepromSize = 4096;
const int ssidAddress = 0;

// old address
const int passwordAddress = 32;
const int isFirstRunAddress = 64;
const int tcpPortAddress = 65;
const int tcpIPAddress = 69;
const int tcpServerPortAddress = 89;
const int mqttServerAddress = 109;
const int mqttPortAddress = 129;
const int mqttUserAddress = 131;
const int mqttPassAddress = 151;
const int loginUserAddress = 171;
const int loginPassAddress = 191;
const int baudAddress = 211;
const int ipConfigAddress = 216;
const int mqttDataAddress = 233;
const int ntpServerAddress = 982;

//  shifted password location for legacy units, older F/W did not support full passwords
const int isNewPasswordAddress = 1001;
const int newPasswordAddress = 1002;
const int idleTimeAddress = 1069;



// MQTT config
struct MqttData {
  bool channelEnabled[44] = { true };
  char pulseUnits[4][5];
  char pulseTypes[4][4];
  char tempUnits[8];
  char labels[44][15];
  bool isConfigured = false;
};

IPAddress mqttServer = IPAddress(0, 0, 0, 0);
char mqttUser[20] = {};
char mqttPass[20] = {};
char mqttClientID[20] = {};
uint16_t mqttPort = 1883;
MqttData mqttData;


// UDP config
const uint16_t udpPort = 48925;
WiFiUDP UDP;
char udpPacket[255];
String udpResponse;
IPAddress broadcastIP(255, 255, 255, 255);
StaticJsonDocument<128> doc;


// WiFi config
char ssid[32] = "";
char password[65] = "";
String apName = "Brultech-";
char apPassword[9] = "brultech";
bool inAP = false;

struct IPAddressConfig {
  bool isConfigured = false;  // Flag indicating if configuration is stored
  IPAddress ip = IPAddress(192, 168, 1, 100);
  IPAddress gateway = IPAddress(192, 168, 1, 1);
  IPAddress subnet = IPAddress(255, 255, 255, 0);
  IPAddress dns = IPAddress(8, 8, 8, 8);  // DNS server 1
};

IPAddressConfig storedIPConfig;


// Device config
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
  int netWatts[32] = { 0 };
  float kwh[32] = { 0.0 };
  float netKwh[32] = { 0.0 };
  float totalKwh[32] = { 0.0 };
  float totalNetKwh[32] = { 0.0 };
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

String gemSerial = "";
uint8_t deviceType = 0;
String deviceName = "";

DeviceData deviceData;
EcmSettings ecmSettings;
uint32_t baud = 115200;

// Serial buffer
const int MAX_DATA_LENGTH = 2048;  // Set the maximum length of the data
char buffer[MAX_DATA_LENGTH];      // Declare the array to store the data
int dataLength = 0;                // Declare a variable to keep track of the length of the data

// Web Server config
#define HTTP_MAX_HEADER_SIZE 4096
WiFiClient webServerClient;

ESP8266WebServer server(80);
ESP8266HTTPUpdateServer httpUpdater;
PubSubClient mqttClient(webServerClient);

char loginUser[20] = "";
char loginPass[20] = "";


// TCP Client/Server
WiFiClient tcpClient;
IPAddress tcpIP = IPAddress(0, 0, 0, 0);
uint16_t tcpPort = 0;

uint16_t tcpServerPort = 8000;
WiFiServer ecmServer(5555);


// Misc
const byte isFirstRunValue = 0xAA;
const byte isNewPasswordValue = 0xAA;
const char* headerkeys[] = { "User-Agent", "Cookie", "Content-Type", "Content-Length", "Update-Size" };
size_t headerkeyssize = sizeof(headerkeys) / sizeof(char*);
uint8_t mac[6];
bool resetFlag = false;
uint16_t idleTime = 5;

String debugText = "";

char ntpServer[40] = "pool.ntp.org";
WiFiUDP ntpUDP;
NTPClient ntpClient(ntpUDP, ntpServer);

String errorMsg = "";

String localAddress = "";

// LED & Reset config
const int LED_PIN = 2;    // GPIO2
const int RESET_PIN = 0;  // GPIO0
Ticker tickerSlow;
Ticker tickerSTA;
Ticker tickerAP;
static bool ledState = false;

const int UPDATE_SIZE_UNKNOWN = -1;

void ICACHE_RAM_ATTR resetToAP() {
  resetFlag = true;
}

void toggleLED() {
  ledState = !ledState;
  digitalWrite(LED_PIN, ledState);
}

void setup() {
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  tickerSlow.attach(2, toggleLED);  // Start the thread

  WiFi.macAddress(mac);

  sprintf(mqttClientID, "%02X%02X%02X", mac[3], mac[4], mac[5]);

  WiFi.hostname("Brultech-" + String(mqttClientID));

  apName = apName + mqttClientID;
  localAddress = "brultech" + String(mqttClientID);

  // Start EEPROM
  EEPROM.begin(eepromSize);

  // Check if this is the first run
  byte isFirstRun = EEPROM.read(isFirstRunAddress);
  if (isFirstRun != isFirstRunValue) {
    resetMemory();
  }

  byte isNewPassword = EEPROM.read(isNewPasswordAddress);
  if (isNewPassword != isNewPasswordValue) {
    EEPROM.get(passwordAddress, password);
    EEPROM.put(newPasswordAddress, password);
    EEPROM.commit();

    EEPROM.write(isNewPasswordAddress, isNewPasswordValue);
    EEPROM.commit();
  }

  // Read settings from EEPROM
  EEPROM.get(ssidAddress, ssid);
  EEPROM.get(newPasswordAddress, password);

  EEPROM.get(baudAddress, baud);
  EEPROM.get(ntpServerAddress, ntpServer);
  String ntpServerString = String(ntpServer);

  if (ntpServerString.isEmpty()) {
    strcpy(ntpServer, "pool.ntp.org");
  }

  // Read the stored IP address configuration
  EEPROM.get(ipConfigAddress, storedIPConfig);

  // Check if configuration is stored
  if (storedIPConfig.isConfigured) {
    // Set the static IP address and DNS configuration
    WiFi.config(
      storedIPConfig.ip,
      storedIPConfig.dns,
      storedIPConfig.gateway,
      storedIPConfig.subnet);
  }

  loadMQTTSettings();

  if (baud != 19200 && baud != 115200) {
    baud = 115200;
  }

  // Start serial port
  Serial.begin(baud);
  Serial.setRxBufferSize(1024);
  Serial.flush();
  Serial.setTimeout(300);

  tcpIP = getIP(tcpIPAddress);
  mqttServer = getIP(mqttServerAddress);

  // Read settings from EEPROM
  EEPROM.get(loginUserAddress, loginUser);
  EEPROM.get(loginPassAddress, loginPass);

  EEPROM.get(tcpPortAddress, tcpPort);
  EEPROM.get(tcpServerPortAddress, tcpServerPort);


  EEPROM.get(idleTimeAddress, idleTime);

  if (idleTime == 0) {
    idleTime = 5;
  }

  //here the list of headers to be recorded
  setupWebServer();
  setupWiFi();
  getDeviceSettings();

  pinMode(RESET_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(RESET_PIN), resetToAP, FALLING);
}

void loadMQTTSettings() {
  EEPROM.get(mqttUserAddress, mqttUser);
  EEPROM.get(mqttPassAddress, mqttPass);

  EEPROM.get(mqttPortAddress, mqttPort);
  EEPROM.get(mqttDataAddress, mqttData);

  if (!mqttData.isConfigured) {
    // Initialize mqttData
    for (int x = 0; x < 44; x++) {
      strcpy(mqttData.labels[x], "");
      mqttData.channelEnabled[x] = true;
      if (x > 31 && x < 40) {
        mqttData.tempUnits[x - 32] = 'C';
      }

      if (x > 39) {
        strcpy(mqttData.pulseUnits[x - 40], "");
        strcpy(mqttData.pulseTypes[x - 40], "");
      }
    }
  }
}

void setupWiFi() {

  // Connect to saved network
  tickerSlow.detach();  // Stop the ticker
  if (strcmp(ssid, "") != 0 && strcmp(password, "") != 0 && WiFi.status() != WL_CONNECTED) {
    int networksFound = WiFi.scanNetworks();
    bool found = false;

    for (int i = 0; i < networksFound; i++) {
      if (String(ssid).equals(WiFi.SSID(i))) {
        found = true;
        break;
      }
    }

    if (found) {
      tickerAP.detach();
      tickerSTA.attach(0.5, toggleLED);  // Start the thread
      WiFi.mode(WIFI_STA);
      WiFi.begin(ssid, password);
      inAP = false;

      int x = 0;

      while (WiFi.status() != WL_CONNECTED) {
        delay(1000);

        if (x > 5) {
          break;
        }

        x++;
      }

      if (WiFi.status() == WL_CONNECTED) {
        tickerSTA.detach();          // Stop the ticker
        digitalWrite(LED_PIN, LOW);  // Turn off the LED

        MDNS.begin(localAddress);

        if (tcpServerPort != 0) {
          ecmServer.stop();
          ecmServer.begin(tcpServerPort);
          //ecmServer.setNoDelay(true);
        }

        UDP.begin(udpPort);

        ntpClient.setPoolServerName(ntpServer);


        ntpClient.begin();

        delay(1000);
      } else {
        tickerSTA.detach();  // Stop the ticker
      }
    }
  }

  // If not connected, start in Access Point mode
  if (WiFi.status() != WL_CONNECTED && !inAP) {
    tickerAP.attach(1, toggleLED);  // Start the thread

    // Start WiFi in Access Point mode
    inAP = true;
    WiFi.mode(WIFI_AP);
    WiFi.softAP(apName, apPassword);

    if (tcpServerPort != 0) {
      ecmServer.stop();
      ecmServer.begin(tcpServerPort);
      //ecmServer.setNoDelay(true);
    }
  }
}

void setupWebServer() {
  server.on("/", handleRoot);
  server.on("/login", handleLogin);
  server.on("/main", handleStationMode);
  server.on("/config", handleConfig);
  server.on("/start-real", handleStartReal);
  server.on("/stop-real", handleStopReal);
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
  server.on("/mqtt-debug", handleMQTTDebug);
  server.on("/ntp-server", handleNTPServer);
  server.on("/reboot", handleReboot);
  server.on("/updater", handleUpdate);
  server.on("/apmain", handleAP);
  server.collectHeaders(headerkeys, headerkeyssize);
  httpUpdater.setup(&server, loginUser, loginPass);
  server.begin();
}

void handleUpdate() {
  String html = getHTMLHeader(0);
  html += "<div><h2>Firmware Upgrade</h2>";
  html += "<form id='updateForm' method='POST' action='/update' enctype='multipart/form-data'>";
  html += "<label>Firmware:</label>";
  html += "<input type='file' accept='.bin,.bin.gz' name='firmware'>";
  html += "<br><br><button class='button'>Update Firmware</button>";
  html += "</form>";
  html += "<div id='pleaseWait' style='display: none;'>";
  html += "<p>Please wait...</p>";
  html += "<div class='spinner'></div>";  // Here we add the spinner
  html += "</div>";
  html += "</body></html>";

  html += "<style>";
  html += ".spinner {";
  html += "  border: 4px solid rgba(0, 0, 0, 0.1);";
  html += "  border-left-color: #09f;";
  html += "  border-radius: 50%;";
  html += "  width: 10px;";
  html += "  height: 10px;";
  html += "  animation: spin 1s linear infinite;";
  html += "}";
  html += "@keyframes spin {";
  html += "  to { transform: rotate(360deg); }";
  html += "}";
  html += "</style>";

  html += "<script>";
  html += "document.getElementById('updateForm').addEventListener('submit', function(event) {";
  html += "  document.getElementById('pleaseWait').style.display = 'block';";
  html += "});";
  html += "</script>";
  server.send(200, "text/html", html);
}

void handleReboot() {
  // Root webpage
  String html = getHTMLHeader(2);
  html += "<div><h2>Rebooting the ESP8266, please wait..</h2></div>";
  html += "</body></html>";
  server.send(200, "text/html", html);
  delay(1000);
  ESP.restart();
}

void resetMemory() {
  for (int i = 0; i < eepromSize; i++) {
    EEPROM.write(i, '\0');
  }
  EEPROM.commit();

  EEPROM.put(tcpServerPortAddress, 8000);
  EEPROM.commit();

  EEPROM.write(isFirstRunAddress, isFirstRunValue);
  EEPROM.commit();
}

void loop() {
  // Reset debounce
  if (resetFlag) {
    resetFlag = false;
    delay(3000);
    if (digitalRead(RESET_PIN) == LOW) {
      digitalWrite(LED_PIN, LOW);  // Turn off the LED
      delay(2000);
      digitalWrite(LED_PIN, HIGH);  // Turn off the LED
      delay(2000);
      digitalWrite(LED_PIN, LOW);  // Turn off the LED
      resetMemory();
      digitalWrite(LED_PIN, HIGH);  // Turn off the LED
      ESP.restart();
    }
  }

  // TCP Server mode
  WiFiClient ecmClient = ecmServer.available();

  unsigned long loopTime;
  unsigned long endTime;
  unsigned long elapsedTime;
  if (ecmClient && ecmClient.connected()) {
    ecmClient.setTimeout(5);
    Serial.setTimeout(50);

    int startTime = millis();
    char temp;
    int x = 0;

    while (ecmClient.connected()) {

      if (millis() - startTime >= idleTime * 1000) {
        break;
      }

      if (ecmClient.available()) {
        dataLength = ecmClient.readBytes(buffer, sizeof(buffer));  // Read all available data from WiFi and store it in the buffer

        Serial.write(buffer, dataLength);  // Send the entire buffer to the ECM-1240
        dataLength = 0;
        delay(10);
      } else {
        server.handleClient();
      }

      if (Serial.available()) {
        while (Serial.available()) {

          if (ecmClient.availableForWrite()) {
            temp = Serial.read();
            ecmClient.write(temp);

            if (x < MAX_DATA_LENGTH) {
              buffer[x] = temp;
              x++;
            }
          } else {
            delay(1);
          }
        }

        startTime = millis();

        if (sizeof(buffer) > 64) {
          dataLength = x;
          handlePacket();
          x = 0;
        }
      }

      delay(1);
    }

    ecmClient.stop();
  } else {
    // If not connected, start in Access Point mode
    if (WiFi.status() != WL_CONNECTED) {
      setupWiFi();
    } else {
      // UDP Detection, check for UDP json packet, respond if received, for GEM Network Utility
      int packetSize = UDP.parsePacket();
      if (packetSize) {
        int len = UDP.read(udpPacket, 255);
        if (len > 0) {
          DeserializationError error = deserializeJson(doc, udpPacket);

          if (!error) {
            if (strcmp(doc["type"], "btech") == 0) {
              if (strcmp(doc["cmd"], "req") == 0) {
                // Send response packet
                udpResponse = "{\"type\":\"" + deviceData.serialNumber + "-" + "esp8266-" + mqttClientID + "\", \"ip\":\"" + WiFi.localIP().toString() + "\"}";

                char charArray[udpResponse.length() + 1];
                udpResponse.toCharArray(charArray, udpResponse.length() + 1);

                UDP.beginPacket(broadcastIP, UDP.remotePort());
                UDP.write(charArray);
                UDP.endPacket();
              }
            }
          }
        }
      }
    }


    MDNS.update();
    server.handleClient();

    // TCP Client mode
    if (!tcpClient.connected() && tcpIP.isSet() && tcpPort > 1024 && tcpPort < 65536 && Serial.available()) {
      tcpClient.connect(tcpIP, tcpPort);
      tcpClient.setTimeout(100);
    }

    if (tcpClient.connected()) {
      // Handle the data passthru
      if (Serial.available()) {
        dataLength = Serial.readBytes(buffer, sizeof(buffer));  // Read all available data from serial and store it in the buffer

        tcpClient.write(buffer, dataLength);  // Send the entire buffer to the server

        handlePacket();
      }

      if (tcpClient.available()) {
        dataLength = tcpClient.readBytes(buffer, sizeof(buffer));  // Read all available data from WiFi and store it in the buffer

        Serial.write(buffer, dataLength);  // Send the entire buffer to the ECM-1240
      }

      tcpClient.stop();
    } else {
      if (Serial.available()) {
        dataLength = Serial.readBytes(buffer, sizeof(buffer));  // Read all available data from serial and store it in the buffer

        handlePacket();
      }
    }

    delay(10);
  }
}

void handleECM() {
  // Loop thru bytes to check for ECM packet
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
  }
}


void ecmPacket() {
  memcpy(deviceData.prevWattSeconds, deviceData.wattSeconds, sizeof(deviceData.prevWattSeconds));
  memcpy(deviceData.prevPolWattSeconds, deviceData.polWattSeconds, sizeof(deviceData.prevPolWattSeconds));

  deviceData.prevSeconds = deviceData.seconds;

  // Extract the voltage value as an unsigned integer
  deviceData.voltage = static_cast<float>((buffer[3] << 8) | buffer[4]) / 10;
  deviceData.seconds = ((uint16_t)buffer[39] << 16) | ((uint16_t)buffer[38] << 8) | (uint16_t)buffer[37];


  // Extract the 5/4-byte value as an unsigned integer
  deviceData.wattSeconds[0] = ((uint64_t)buffer[9] << 32) | ((uint64_t)buffer[8] << 24) | ((uint64_t)buffer[7] << 16) | ((uint64_t)buffer[6] << 8) | (uint64_t)buffer[5];
  deviceData.wattSeconds[1] = ((uint64_t)buffer[14] << 32) | ((uint64_t)buffer[13] << 24) | ((uint64_t)buffer[12] << 16) | ((uint64_t)buffer[11] << 8) | (uint64_t)buffer[10];
  deviceData.wattSeconds[2] = ((uint64_t)buffer[43] << 24) | ((uint64_t)buffer[42] << 16) | ((uint64_t)buffer[41] << 8) | (uint64_t)buffer[40];
  deviceData.wattSeconds[3] = ((uint64_t)buffer[47] << 24) | ((uint64_t)buffer[46] << 16) | ((uint64_t)buffer[45] << 8) | (uint64_t)buffer[44];
  deviceData.wattSeconds[4] = ((uint64_t)buffer[51] << 24) | ((uint64_t)buffer[50] << 16) | ((uint64_t)buffer[49] << 8) | (uint64_t)buffer[48];
  deviceData.wattSeconds[5] = ((uint64_t)buffer[55] << 24) | ((uint64_t)buffer[54] << 16) | ((uint64_t)buffer[53] << 8) | (uint64_t)buffer[52];
  deviceData.wattSeconds[6] = ((uint64_t)buffer[59] << 24) | ((uint64_t)buffer[58] << 16) | ((uint64_t)buffer[57] << 8) | (uint64_t)buffer[56];

  deviceData.polWattSeconds[0] = ((uint64_t)buffer[19] << 32) | ((uint64_t)buffer[18] << 24) | ((uint64_t)buffer[17] << 16) | ((uint64_t)buffer[16] << 8) | (uint64_t)buffer[15];
  deviceData.polWattSeconds[1] = ((uint64_t)buffer[24] << 32) | ((uint64_t)buffer[23] << 24) | ((uint64_t)buffer[22] << 16) | ((uint64_t)buffer[21] << 8) | (uint64_t)buffer[20];

  deviceData.serialNumber = String((uint16_t)buffer[32]) + String(((buffer[29] << 8) | buffer[30]));

  if (deviceData.prevSeconds != 0) {
    processPacket();
  }
}

float tempConv(uint16_t hi, uint16_t lo) {
  if (((hi & 0x02) >> 1) == 1) {
    return 8192.0;
  } else if ((hi >> 7) != 1) {
    return float(((hi & 0x01) << 8) | (lo & 0xFF));
  } else {
    return float(-1 * (((hi & 0x01) << 8) | (lo & 0xFF)));
  }
}

void gemPacket() {
  memcpy(deviceData.prevWattSeconds, deviceData.wattSeconds, sizeof(deviceData.prevWattSeconds));
  memcpy(deviceData.prevPolWattSeconds, deviceData.polWattSeconds, sizeof(deviceData.prevPolWattSeconds));

  deviceData.prevSeconds = deviceData.seconds;

  // Extract the voltage value as an unsigned integer
  deviceData.voltage = static_cast<float>((buffer[3] << 8) | buffer[4]) / 10;
  deviceData.seconds = ((uint16_t)buffer[395] << 16) | ((uint16_t)buffer[394] << 8) | (uint16_t)buffer[393];

  int v = 0;
  for (int z = 0; z < 32; z++) {
    deviceData.wattSeconds[z] = ((uint64_t)buffer[(z * 5) + 9] << 32) | ((uint64_t)buffer[(z * 5) + 8] << 24) | ((uint64_t)buffer[(z * 5) + 7] << 16) | ((uint64_t)buffer[(z * 5) + 6] << 8) | (uint64_t)buffer[(z * 5) + 5];
    deviceData.polWattSeconds[z] = ((uint64_t)buffer[(z * 5) + 169] << 32) | ((uint64_t)buffer[(z * 5) + 168] << 24) | ((uint64_t)buffer[(z * 5) + 167] << 16) | ((uint64_t)buffer[(z * 5) + 166] << 8) | (uint64_t)buffer[(z * 5) + 165];
    deviceData.amps[z] = static_cast<float>((uint16_t)buffer[(z * 2) + 330] << 8 | (uint16_t)buffer[(z * 2) + 329]) / 50;

    if (z < 8) {
      deviceData.temp[z] = tempConv((uint16_t)buffer[(z * 2) + 409], (uint16_t)buffer[(z * 2) + 408]) / 2;
      if (z < 4) {
        deviceData.pulse[z] = ((uint64_t)buffer[(z * 3) + 398] << 16) | ((uint64_t)buffer[(z * 3) + 397] << 8) | (uint64_t)buffer[(z * 3) + 396];
      }
    }
  }

  String serial = String(((buffer[325] << 8) | buffer[326]));
  String id = String((uint16_t)buffer[328]);

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
}

void gemPacketLarge() {
  memcpy(deviceData.prevWattSeconds, deviceData.wattSeconds, sizeof(deviceData.prevWattSeconds));
  memcpy(deviceData.prevPolWattSeconds, deviceData.polWattSeconds, sizeof(deviceData.prevPolWattSeconds));

  deviceData.prevSeconds = deviceData.seconds;

  // Extract the voltage value as an unsigned integer
  deviceData.voltage = static_cast<float>((buffer[3] << 8) | buffer[4]) / 10;
  deviceData.seconds = ((uint16_t)buffer[395] << 16) | ((uint16_t)buffer[394] << 8) | (uint16_t)buffer[393];

  int v = 0;
  for (int z = 0; z < 32; z++) {
    deviceData.wattSeconds[z] = ((uint64_t)buffer[(z * 5) + 9] << 32) | ((uint64_t)buffer[(z * 5) + 8] << 24) | ((uint64_t)buffer[(z * 5) + 7] << 16) | ((uint64_t)buffer[(z * 5) + 6] << 8) | (uint64_t)buffer[(z * 5) + 5];
    deviceData.polWattSeconds[z] = ((uint64_t)buffer[(z * 5) + 249] << 32) | ((uint64_t)buffer[(z * 5) + 248] << 24) | ((uint64_t)buffer[(z * 5) + 247] << 16) | ((uint64_t)buffer[(z * 5) + 246] << 8) | (uint64_t)buffer[(z * 5) + 245];
    deviceData.amps[z] = static_cast<float>((uint16_t)buffer[(z * 2) + 489] << 8 | (uint16_t)buffer[(z * 2) + 490]) / 50;

    if (z < 8) {
      deviceData.temp[z] = static_cast<float>((uint16_t)buffer[(z * 2) + 601] << 8 | (uint16_t)buffer[(z * 2) + 600]) / 2;
      if (z < 4) {
        deviceData.pulse[z] = ((uint64_t)buffer[(z * 3) + 590] << 16) | ((uint64_t)buffer[(z * 3) + 589] << 8) | (uint64_t)buffer[(z * 3) + 588];
      }
    }
  }

  String serial = String(((buffer[485] << 8) | buffer[486]));
  String id = String((uint16_t)buffer[488]);

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
}

void handlePacket() {
  bool found = false;

  if (dataLength > 63) {
    if (dataLength > 63) {
      if (buffer[0] == 0xFE && buffer[1] == 0xFF && buffer[2] == 0x03 && buffer[62] == 0xFF && buffer[63] == 0xFE) {
        ecmPacket();
        found = true;
      }
    }

    if (dataLength > 427 && !found) {
      if (buffer[0] == 0xFE && buffer[1] == 0xFF && buffer[2] == 0x07 && buffer[426] == 0xFF && buffer[427] == 0xFE) {
        gemPacket();
        found = true;
      }
    }

    if (dataLength > 623 && !found) {
      if (buffer[0] == 0xFE && buffer[1] == 0xFF && buffer[2] == 0x05 && buffer[622] == 0xFF && buffer[623] == 0xFE) {
        gemPacketLarge();
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
  int deltaPolWs = 0;

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

      deltaPolWs = deviceData.deltaWattSeconds[x] - (2 * polWattSecDiff);

      deviceData.netWatts[x] = static_cast<float>(deltaPolWs) / secDiff;
      deviceData.netKwh[x] = static_cast<float>(deltaPolWs) / 3600000;
      deviceData.totalNetKwh[x] += deviceData.netKwh[x];
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
  if (WiFi.status() == WL_CONNECTED && mqttServer.isSet()) {
    mqttClient.setServer(mqttServer, mqttPort);

    deviceName = "ECM1240";
    uint8_t numChan = 7;

    if (deviceType == 2) {
      deviceName = "GEM";
      numChan = 32;
    }

    if (!mqttClient.connect(mqttClientID, mqttUser, mqttPass)) {
      Serial.println("Failed to connect to MQTT broker");
      return;
    }

    // Publish topics
    mqttClient.publish((deviceName + "-" + String(deviceData.serialNumber) + "/v").c_str(), String(deviceData.voltage).c_str());

    for (int i = 0; i < numChan; i++) {
      mqttClient.publish((deviceName + "-" + String(deviceData.serialNumber) + "/c" + String(i + 1) + "/watt").c_str(), String(deviceData.watts[i]).c_str());
      mqttClient.publish((deviceName + "-" + String(deviceData.serialNumber) + "/c" + String(i + 1) + "/kwh").c_str(), String(deviceData.kwh[i], 5).c_str());
      mqttClient.publish((deviceName + "-" + String(deviceData.serialNumber) + "/c" + String(i + 1) + "/total_kwh").c_str(), String(deviceData.totalKwh[i], 5).c_str());
      mqttClient.publish((deviceName + "-" + String(deviceData.serialNumber) + "/c" + String(i + 1) + "/ws").c_str(), String(deviceData.wattSeconds[i]).c_str());
      mqttClient.publish((deviceName + "-" + String(deviceData.serialNumber) + "/c" + String(i + 1) + "/pws").c_str(), String(deviceData.polWattSeconds[i]).c_str());
      mqttClient.publish((deviceName + "-" + String(deviceData.serialNumber) + "/c" + String(i + 1) + "/dws").c_str(), String(deviceData.deltaWattSeconds[i]).c_str());

      if (deviceType == 2) {
        mqttClient.publish((deviceName + "-" + String(deviceData.serialNumber) + "/c" + String(i + 1) + "/amp").c_str(), String(deviceData.amps[i]).c_str());

        if (i < 8 && deviceType == 2) {
          mqttClient.publish((deviceName + "-" + String(deviceData.serialNumber) + "/t" + String(i + 1) + "/value").c_str(), String(deviceData.temp[i]).c_str());

          if (i < 4) {
            mqttClient.publish((deviceName + "-" + String(deviceData.serialNumber) + "/p" + String(i + 1) + "/value").c_str(), String(deviceData.pulse[i]).c_str());
          }
        }
      }
    }

    // Disconnect from MQTT broker
    mqttClient.disconnect();
  }
}

void handleHA() {
  String html = getHTMLHeader(2);

  /* struct MqttData {
      bool channelEnabled[40] = { true };
      String pulseUnits[4];
      String pulseTypes[4];
      char tempUnits[8];
      String labels[40];
      bool isConfigured = false;
    };


  deviceName = "ECM1240";*/
  uint8_t numChan = 7;

  if (deviceType == 2) {
    deviceName = "GEM";
    numChan = 32;
  }
  /* for (int x = 0; x < 8; x++) {
      mqttData.tempUnits[x] = server.arg("t" + String(x + 1) + "unit").charAt(0);
      strcpy(mqttData.labels[x + numChan], server.arg("ch" + String(x + numChan + 1) + "label").c_str());

      if (server.arg("ch" + String(x + numChan + 1) + "enable") == "1") {
        mqttData.channelEnabled[x + numChan] = true;
      } else {
        mqttData.channelEnabled[x + numChan] = false;
      }
    }

    for (int x = 0; x < 4; x++) {
      strcpy(mqttData.pulseTypes[x], server.arg("p" + String(x + 1) + "type").c_str());
      strcpy(mqttData.pulseUnits[x], server.arg("p" + String(x + 1) + "unit").c_str());
      strcpy(mqttData.labels[x + numChan + 8], server.arg("ch" + String(x + numChan + 9) + "label").c_str());

      if (mqttData.pulseUnits[x] == "0") {
        strcpy(mqttData.pulseUnits[x], "m³");
      } else if (mqttData.pulseUnits[x] == "1") {
        strcpy(mqttData.pulseUnits[x], "ft³");
      }

      if (server.arg("ch" + String(x + numChan + 9) + "enable") == "1") {
        mqttData.channelEnabled[x + numChan + 8] = true;
      } else {
        mqttData.channelEnabled[x + numChan + 8] = false;
      }
    }
  } else {
    strcpy(mqttData.pulseTypes[0], server.arg("p1type").c_str());
    strcpy(mqttData.pulseUnits[0], server.arg("p1unit").c_str());

    if (mqttData.pulseUnits[0] == "0") {
      strcpy(mqttData.pulseUnits[0], "m³");
    } else if (mqttData.pulseUnits[0] == "1") {
      strcpy(mqttData.pulseUnits[0], "ft³");
    }
  }

  for (int x = 0; x < numChan; x++) {
    strcpy(mqttData.labels[x], server.arg("ch" + String(x + 1) + "label").c_str());

    if (server.arg("ch" + String(x + 1) + "enable") == "1") {
      mqttData.channelEnabled[x] = true;
    } else {
      mqttData.channelEnabled[x] = false;
    }
  }

  mqttData.isConfigured = true;

  // Write the IP settings structure to EEPROM
  EEPROM.put(mqttDataAddress, mqttData);

  EEPROM.commit();

  html += "<div><h3>MQTT saved.</h3></div></html></body>";

  server.send(200, "text/html", html);
  return; */

  if (WiFi.status() == WL_CONNECTED && mqttServer.isSet()) {


    server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
    server.sendHeader("Pragma", "no-cache");
    server.sendHeader("Expires", "-1");
    server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    // here begin chunked transfer
    server.send(200, "text/html", getHTMLHeader(1));

    mqttClient.setServer(mqttServer, mqttPort);
    if (!mqttClient.connect(mqttClientID, mqttUser, mqttPass)) {
      html += "<div><h3>MQTT couldn't connect, please check your settings.</h3></div></html></body>";
      server.send(200, "text/html", html);
      server.close();
      return;
    }


    html += "<div><h3>MQTT Values Sent:</h3>";
    String payload = "{\"unique_id\": \"" + deviceData.serialNumber + "v\", \"name\":\"" + deviceName + "-" + deviceData.serialNumber + " Volts\",\"state_topic\":\"" + deviceName + "-" + deviceData.serialNumber + "/v\",\"unit_of_measurement\":\"V\", \"state_class\": \"measurement\", \"dev\":{\"ids\":\"" + deviceData.serialNumber + "\",\"name\":\"" + deviceName + "-" + deviceData.serialNumber + "\",\"sw\":\"esp8266-custom\",\"mdl\":\"" + deviceName + "\",\"mf\":\"BrulTech Research Inc.\"}}";
    String topic = "homeassistant/sensor/" + deviceName + "-" + deviceData.serialNumber + "/volts/config";
    if (!mqttClient.publish(topic.c_str(), payload.c_str(), true)) {
      html += "Not Sent: " + String(sizeof(payload)) + " " + String(MQTT_MAX_PACKET_SIZE) + "<br><br>";
    }

    html += payload + "<br><br>" + topic + "<br><br>";

    for (int x = 0; x < numChan; x++) {

      // if ecm aux 5 is pulse or gas
      if (deviceType == 1 && x == 6 && mqttData.pulseTypes[0] != "energy") {
        payload = "{\"unique_id\": \"" + deviceData.serialNumber + "p" + (x + 1) + "_value\", \"name\":\"" + deviceName + "-" + deviceData.serialNumber + " P" + (x + 1) + " Value\",\"state_topic\":\"" + deviceName + "-" + deviceData.serialNumber + "/p" + (x + 1) + "/value\",\"unit_of_measurement\":\"" + mqttData.pulseUnits[0] + "\", \"device_class\": \"" + mqttData.pulseTypes[0] + "\", \"state_class\": \"measurement\", \"dev\":{\"ids\":\"" + deviceData.serialNumber + "\",\"name\":\"" + deviceName + "-" + deviceData.serialNumber + "\",\"sw\":\"esp8266-custom\",\"mdl\":\"" + deviceName + "\",\"mf\":\"BrulTech Research Inc.\"}}";
        topic = "homeassistant/sensor/" + deviceName + "-" + deviceData.serialNumber + "/p" + (x + 1) + "_value/config";
        if (!mqttClient.publish(topic.c_str(), payload.c_str(), true)) {
          html += "Not Sent:<br><br>";
        }
      } else {
        payload = "{\"unique_id\": \"" + deviceData.serialNumber + "ch" + (x + 1) + "kwh\", \"name\":\"" + deviceName + "-" + deviceData.serialNumber + " CH" + (x + 1) + " kWh\",\"state_topic\":\"" + deviceName + "-" + deviceData.serialNumber + "/c" + (x + 1) + "/kwh\",\"unit_of_measurement\":\"kWh\", \"state_class\": \"measurement\", \"dev\":{\"ids\":\"" + deviceData.serialNumber + "\",\"name\":\"" + deviceName + "-" + deviceData.serialNumber + "\",\"sw\":\"esp8266-custom\",\"mdl\":\"" + deviceName + "\",\"mf\":\"BrulTech Research Inc.\"}}";
        topic = "homeassistant/sensor/" + deviceName + "-" + deviceData.serialNumber + "/ch" + (x + 1) + "_kwh/config";
        if (!mqttClient.publish(topic.c_str(), payload.c_str(), true)) {
          html += "Not Sent:<br><br>";
        }

        payload = "{\"unique_id\": \"" + deviceData.serialNumber + "ch" + (x + 1) + "_total_kwh\", \"name\":\"" + deviceName + "-" + deviceData.serialNumber + " CH" + (x + 1) + " Total kWh\",\"state_topic\":\"" + deviceName + "-" + deviceData.serialNumber + "/c" + (x + 1) + "/total_kwh\",\"unit_of_measurement\":\"kWh\", \"device_class\": \"energy\", \"state_class\": \"total_increasing\", \"last_reset\": \"1970-01-01T00:00:00+00:00\", \"dev\":{\"ids\":\"" + deviceData.serialNumber + "\",\"name\":\"" + deviceName + "-" + deviceData.serialNumber + "\",\"sw\":\"esp8266-custom\",\"mdl\":\"" + deviceName + "\",\"mf\":\"BrulTech Research Inc.\"}}";
        topic = "homeassistant/sensor/" + deviceName + "-" + deviceData.serialNumber + "/ch" + (x + 1) + "_total_kwh/config";
        if (!mqttClient.publish(topic.c_str(), payload.c_str(), true)) {
          html += "Not Sent:<br><br>";
        }


        html += payload + "<br><br>" + topic + "<br><br>";

        payload = "{\"unique_id\": \"" + deviceData.serialNumber + "ch" + (x + 1) + "ws\", \"name\":\"" + deviceName + "-" + deviceData.serialNumber + " CH" + (x + 1) + " WattSeconds\",\"state_topic\":\"" + deviceName + "-" + deviceData.serialNumber + "/c" + (x + 1) + "/dws\",\"unit_of_measurement\":\"WS\", \"state_class\": \"measurement\", \"dev\":{\"ids\":\"" + deviceData.serialNumber + "\",\"name\":\"" + deviceName + "-" + deviceData.serialNumber + "\",\"sw\":\"esp8266-custom\",\"mdl\":\"" + deviceName + "\",\"mf\":\"BrulTech Research Inc.\"}}";
        topic = "homeassistant/sensor/" + deviceName + "-" + deviceData.serialNumber + "/ch" + (x + 1) + "_ws/config";
        if (!mqttClient.publish(topic.c_str(), payload.c_str(), true)) {
          html += "Not Sent:<br><br>";
        }

        html += payload + "<br><br>" + topic + "<br><br>";

        payload = "{\"unique_id\": \"" + deviceData.serialNumber + "ch" + (x + 1) + "w\", \"name\":\"" + deviceName + "-" + deviceData.serialNumber + " CH" + (x + 1) + " Watts\",\"state_topic\":\"" + deviceName + "-" + deviceData.serialNumber + "/c" + (x + 1) + "/watt\",\"unit_of_measurement\":\"W\", \"state_class\": \"measurement\", \"dev\":{\"ids\":\"" + deviceData.serialNumber + "\",\"name\":\"" + deviceName + "-" + deviceData.serialNumber + "\",\"sw\":\"esp8266-custom\",\"mdl\":\"" + deviceName + "\",\"mf\":\"BrulTech Research Inc.\"}}";
        topic = "homeassistant/sensor/" + deviceName + "-" + deviceData.serialNumber + "/ch" + (x + 1) + "_watts/config";
        if (!mqttClient.publish(topic.c_str(), payload.c_str(), true)) {
          html += "Not Sent:<br><br>";
        }

        html += payload + "<br><br>" + topic + "<br><br>";

        if (deviceType == 2) {
          payload = "{\"unique_id\": \"" + deviceData.serialNumber + "ch" + (x + 1) + "a\", \"name\":\"" + deviceName + "-" + deviceData.serialNumber + " CH" + (x + 1) + " Amps\",\"state_topic\":\"" + deviceName + "-" + deviceData.serialNumber + "/c" + (x + 1) + "/amp\",\"unit_of_measurement\":\"A\", \"state_class\": \"measurement\", \"dev\":{\"ids\":\"" + deviceData.serialNumber + "\",\"name\":\"" + deviceName + "-" + deviceData.serialNumber + "\",\"sw\":\"esp8266-custom\",\"mdl\":\"" + deviceName + "\",\"mf\":\"BrulTech Research Inc.\"}}";
          topic = "homeassistant/sensor/" + deviceName + "-" + deviceData.serialNumber + "/ch" + (x + 1) + "_amps/config";
          if (!mqttClient.publish(topic.c_str(), payload.c_str(), true)) {
            html += "Not Sent:<br><br>";
          }

          html += payload + "<br><br>" + topic + "<br><br>";

          if (x < 8) {
            payload = "{\"unique_id\": \"" + deviceData.serialNumber + "t" + (x + 1) + "_value\", \"name\":\"" + deviceName + "-" + deviceData.serialNumber + " T" + (x + 1) + " Value\",\"state_topic\":\"" + deviceName + "-" + deviceData.serialNumber + "/t" + (x + 1) + "/value\",\"unit_of_measurement\":\"C\", \"state_class\": \"measurement\", \"dev\":{\"ids\":\"" + deviceData.serialNumber + "\",\"name\":\"" + deviceName + "-" + deviceData.serialNumber + "\",\"sw\":\"esp8266-custom\",\"mdl\":\"" + deviceName + "\",\"mf\":\"BrulTech Research Inc.\"}}";
            topic = "homeassistant/sensor/" + deviceName + "-" + deviceData.serialNumber + "/t" + (x + 1) + "_value/config";
            if (!mqttClient.publish(topic.c_str(), payload.c_str(), true)) {
              html += "Not Sent:<br><br>";
            }

            if (x < 4) {
              payload = "{\"unique_id\": \"" + deviceData.serialNumber + "p" + (x + 1) + "_value\", \"name\":\"" + deviceName + "-" + deviceData.serialNumber + " P" + (x + 1) + " Value\",\"state_topic\":\"" + deviceName + "-" + deviceData.serialNumber + "/p" + (x + 1) + "/value\",\"unit_of_measurement\":\"" + mqttData.pulseUnits[x] + "\", \"device_class\": \"" + mqttData.pulseTypes[x] + "\", \"state_class\": \"total_increasing\", \"dev\":{\"ids\":\"" + deviceData.serialNumber + "\",\"name\":\"" + deviceName + "-" + deviceData.serialNumber + "\",\"sw\":\"esp8266-custom\",\"mdl\":\"" + deviceName + "\",\"mf\":\"BrulTech Research Inc.\"}}";
              topic = "homeassistant/sensor/" + deviceName + "-" + deviceData.serialNumber + "/p" + (x + 1) + "_value/config";
              if (!mqttClient.publish(topic.c_str(), payload.c_str(), true)) {
                html += "Not Sent:<br><br>";
              }
            }
          }
        }
      }
    }

    // Publish the auto-discovery payload to the MQTT broker

    mqttClient.disconnect();
    html += "</div></body></html>";
  } else {
    html += "<div><h3>MQTT couldn't connect, please check your settings.</h3></div></html></body>";
  }
  server.send(200, "text/html", html);
}

void handleAP() {
  if (!isAuthenticated()) {
    sendLogin(false);
  } else if (WiFi.status() != WL_CONNECTED) {
    // Root webpage
    int networksFound = WiFi.scanNetworks();
    String html = getHTMLHeader(0);

    html += "<div><h2>Network Configuration</h2>";
    html += "<form action='/config'>";
    html += "<label>Select a network:</label> <select id='ssid' name='ssid'>";
    for (int i = 0; i < networksFound; i++) {
      html += "<option value='" + String(WiFi.SSID(i)) + "'>" + String(WiFi.SSID(i)) + " <b>RSSI:</b> " + String(WiFi.RSSI(i)) + "</option>";
    }
    html += "</select><br>";
    html += "<label>Or enter SSID:</label> <input id='custom_ssid' class='full' type='text' name='custom_ssid'><br>";
    html += "<label>Password:</label> <input class='full' maxlength='64' type='password' name='password' value=''><br>";
    html += "<button id='saveLocal' class='button'>Connect to Network</button>";
    html += "</form></div><div>Local address will be copied to clipboard upon clicking connect.<br><br> <a id='localLink' href='http://" + localAddress + ".local/'>http://" + localAddress + ".local/</a></div>";
    html += "<div>Click below to access the configuration page.<br><br> <a href='http://192.168.4.1/main'>Configuration Page</a></div>";
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
  passwordValue.toCharArray(password, 64);

  EEPROM.put(ssidAddress, ssid);
  EEPROM.put(newPasswordAddress, password);

  EEPROM.commit();
  String html = getHTMLHeader(4);

  html += "<div><h3>Network configuration saved, please connect back to your network. The ESP-XBEE module will have a solid green LED once connected. <br><br>  Paste the copied .local address into your address bar after to try to connect.<br><br></h3></div></body></html>";
  server.send(200, "text/html", html);
  delay(250);

  // Reboot module
  ESP.restart();
}

void getDeviceSettings() {
  Serial.flush();
  bool tryGEM = false;

  deviceType = 3;

  if (baud == 19200) {
    byte data = 0xFC;  // binary 0xFC
    Serial.write(data);
    delay(50);
    Serial.write("SET");
    delay(100);
    Serial.write("RCV");
    delay(50);

    if (Serial.available()) {
      dataLength = Serial.readBytes(buffer, sizeof(buffer));  // Read all available data from serial and store it in the buffer

      if (dataLength > 32) {
        debugText = "Success " + String(buffer) + " " + String(dataLength);
        deviceType = 1;
        processECMSettings();
      } else {
        debugText = String(buffer) + " " + String(dataLength);
        tryGEM = true;
      }
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
    } else {
      //getDeviceSettingsChangeBaud();
    }
  }
}

void getDeviceSettingsChangeBaud() {
  if (baud == 19200) {
    baud = 115200;
  } else {
    baud = 19200;
  }

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
  bool process = false;
  int i = 0;

  for (i = 0; i < 5; i++) {
    if (buffer[i] == 0xFC) {
      process = true;
      break;
    }
  }

  i = i + 3;

  // Validate the first three bytes and last 2 bytes
  if (process) {
    // Extract the voltage value as an unsigned integer
    ecmSettings.gotSettings = true;

    ecmSettings.ch1Set[0] = (uint8_t)buffer[i++];
    debugText += " " + String(i);
    ecmSettings.ch1Set[1] = (uint8_t)buffer[i++];
    debugText += " " + String(i);

    ecmSettings.ch2Set[0] = (uint8_t)buffer[i++];
    debugText += " " + String(i);
    ecmSettings.ch2Set[1] = (uint8_t)buffer[i++];
    debugText += " " + String(i);

    ecmSettings.ptSet[0] = (uint8_t)buffer[i++];
    debugText += " " + String(i);
    ecmSettings.ptSet[1] = (uint8_t)buffer[i++];
    debugText += " " + String(i);

    ecmSettings.sendInterval = (uint8_t)buffer[i++];
    debugText += " " + String(i);
    i++;

    ecmSettings.firmwareVersion = static_cast<double>((buffer[i] << 8) | buffer[i + 1]) / 1000;
    debugText += " " + String(i);

    i = i + 2;

    ecmSettings.serialNumber = String((uint16_t)buffer[i]) + String(((buffer[i + 2] << 8) | buffer[i + 1]));
    debugText += " " + String(i);

    i = i + 3;

    debugText += " " + String(i);
    int y = 0;
    for (y; y < 5; y++) {
      ecmSettings.auxX2[y] = (buffer[i] & (1 << y)) != 0;
    }

    if ((buffer[i] & (1 << y)) != 0) {
      ecmSettings.aux5Option = 1;
    } else if ((buffer[i] & (1 << ++y)) != 0) {
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

    int networksFound = WiFi.scanNetworks();
    String networks = "";
    bool selected = false;
    for (int i = 0; i < networksFound; i++) {
      networks += "<option value='" + String(WiFi.SSID(i)) + "'";
      if (!selected) {
        if (String(ssid).equals(WiFi.SSID(i))) {
          networks += " selected='selected'";
          selected = true;
        }
      }
      networks += ">" + String(WiFi.SSID(i)) + " <b>RSSI:</b> " + String(WiFi.RSSI(i)) + "</option>";
    }

    ntpClient.update();
    // here begin chunked transfer
    String html = getHTMLHeader(1);
    // Station mode webpage

    html += "<div><h3>Brultech Config " + ver + "</h3><br><a href='http://" + localAddress + ".local/'>http://" + localAddress + ".local/</a></div>";
    html += "<div id='network'><form action='/config'><h3>Menu</h3>";
    html += "<a href='#network' class='button'>Network</a>";
    html += "<a href='#baud' class='button'>Baud</a>";
    html += "<a href='#client' class='button'>TCP Client</a>";
    html += "<a href='#server' class='button'>TCP Server</a>";
    html += "<a href='#mqtt' class='button'>MQTT</a>";
    html += "<a href='#login' class='button'>Login</a>";
    html += "<a href='#settings' class='button'>Device Settings</a>";
    html += "<a href='#data' class='button'>Data</a>";
    html += "<a href='#fw' class='button'>ESP Firmware</a>";
    html += "<a href='#serialDebug' class='button'>Debug</a>";
    html += "</div>";
    html += "<div id='network'><form action='/config'><h3>Network Settings</h3>";
    html += "<h4 style='color:#4CAF50;'>Connected to: " + String(WiFi.SSID()) + " <br><br>RSSI: " + String(WiFi.RSSI()) + "</h4>";
    html += "<label>Select a new network:</label> <select id='ssid' name='ssid'>";
    html += networks;
    html += "</select>";
    html += "<label>or enter the SSID:</label><input id='custom_ssid' class='full' maxlength='20' type='text' name='custom_ssid' value='" + String(ssid) + "'>";
    html += "<label>Password:</label><input class='full' maxlength='64' type='password' name='password' value=''>";
    html += "<button class='button'>Submit</button>";
    html += "</form></div>";
    html += "<div><form action='/ip-config'><h3>IP Address Settings</h3>";
    html += "<label>Type:</label>DHCP: <input name='type' type='radio' ";

    if (!storedIPConfig.isConfigured) {
      html += "checked='checked'";
    }

    html += " value='0'>  Static: <input name='type' type='radio' ";

    if (storedIPConfig.isConfigured) {
      html += "checked='checked'";
    }

    html += " value='1'>";
    html += "<label>IP address:</label><input class='full' type='text' name='ip' value='" + WiFi.localIP().toString() + "'>";
    html += "<label>Subnet:</label><input class='full' type='text' name='subnet' value='" + WiFi.subnetMask().toString() + "'>";
    html += "<label>Gateway:</label><input class='full' type='text' name='gateway' value='" + WiFi.gatewayIP().toString() + "'>";
    html += "<label>DNS</label><input class='full' type='text' name='dns' value='" + WiFi.dnsIP(0).toString() + "'>";
    html += "<button class='button'>Save Settings</button>";
    html += "</form></div>";
    html += "<div id='baud'><form action='/baud'><h3>Change Baud Rate</h3>";
    html += "19200: <input type='radio' name='baud' value='19200'";

    if (baud == 19200) {
      html += " checked='checked'";
    }

    html += "> 115200: <input type='radio' name='baud' value='115200'";

    if (baud == 115200) {
      html += " checked='checked'";
    }

    html += "><br><button class='button'>Save Baudrate</button>";
    html += "</form></div>";



    html += "<div id='client'><form action='/ntp-server'><h3>Time Server</h3>";
    html += "<label>UTC Time:</label>" + getFormattedDate() + "<br>";
    html += "<label>NTP Server:</label><input class='full' type='text' name='ntp_server' value='" + String(ntpServer) + "'>";
    html += "<button class='button'>Change Server</button>";
    html += "</form></div>";

    html += "<div id='client'><form action='/serial-to-tcp'><h3>Serial to TCP Client</h3>";
    html += "<label>IP address:</label><input class='full' type='text' name='ip' value='" + tcpIP.toString() + "'>";
    html += "<label>Port:</label><input class='full' type='number' name='port' value='" + String(tcpPort) + "'>";
    html += "<button class='button'>Connect</button>";
    html += "</form></div>";

    html += "<div id='server'><form action='/serial-to-tcp-server'><h3>TCP Server Connection</h3>";
    html += "<label>Disconnect Time (no activity):</label>Time (in seconds): <input type='number' min='1' max='6000' name='idle_time' value='" + String(idleTime) + "'>";
    html += "<label>Port:</label><input class='full' type='number' name='port' value='" + String(tcpServerPort) + "'>";
    html += "<button class='button'>Save</button>";
    html += "</form></div>";

    html += "<div id='mqtt'><form action='/mqtt'><h3>MQTT Server Connection</h3>";
    html += "<label>IP address/Domain:</label><input class='full' type='text' name='ip' value='" + mqttServer.toString() + "'>";
    html += "<label>Port:</label><input class='full' type='number' name='port' value='" + String(mqttPort) + "'>";
    html += "<label>User:</label><input class='full' maxlength='20'  type='text' name='user' value='" + String(mqttUser) + "'>";
    html += "<label>Password:</label><input  maxlength='20' class='full' type='password' name='pass' value=''>";
    html += "<button class='button'>Save</button>";
    html += "</form><form action='/send-ha'><h3>Home-Assistant Config</h3>";

    /*<a class='button' href='#' id='toggleMqtt'>Open Advanced Config</a><div id='advancedMqtt' style='display:none;'>";

    if (deviceType == 1) {
      html += "<label>AUX #5:</label><select name='p1type'><option value='energy'>Energy</option><option value='gas'>Gas</option><option value='water'>Water</option></select>Unit: <select name='p1unit'><option value='L'>L</option><option value='gal'>gal</option><option value='m³'>m³</option><option value='ft³'>ft³</option><option value='CCF'>CCF</option></select>";
    } else if (deviceType == 2) {

      server.sendContent(html);
      html = "";
      for (int x = 1; x < 33; x++) {
        html += "<label><input type='checkbox' name='ch" + String(x) + "enable' value='1' " + boolToText(mqttData.channelEnabled[x - 1], true) + ">CH" + String(x) + ": Label: <input maxlength='50' class='mid' type='text' name='ch" + String(x) + "label' value='" + mqttData.labels[x - 1] + "'></label>";
      }
      server.sendContent(html);

      html = "";


      for (int x = 1; x < 9; x++) {
        html += "<label><input type='checkbox' name='ch" + String(x + 32) + "enable' value='1' " + boolToText(mqttData.channelEnabled[x + 31], true) + ">Temp " + String(x) + ":  Label: <input maxlength='50' class='mid' type='text' name='ch" + String(x + 32) + "label' value='" + mqttData.labels[x + 31] + "'> <select name='t" + String(x) + "type'><option value='C'>C</option><option value='F'>F</option></select></label>";
      }
      server.sendContent(html);

      html = "";

      for (int z = 1; z < 5; z++) {
        html += "<label><input type='checkbox'  name='ch" + String(z + 40) + "enable' value='1' " + boolToText(mqttData.channelEnabled[z + 39], true) + ">Pulse " + z + ":  Label: <input maxlength='50' class='mid' type='text' name='ch" + String(z + 40) + "label' value='" + mqttData.labels[z + 39] + "'></label>Type: <select name='p" + String(z) + "type'><option value='gas'";
        html += ">Gas</option><option value='water'";
        html += ">Water</option></select> Unit: <select name='p" + String(z) + "unit'><option value='L'";

        if (mqttData.pulseUnits[z - 1][0] == 'L') {
          html += " selected='selected'";
        }
        html += ">L</option><option value='gal'";

        if (mqttData.pulseUnits[z - 1][0] == 'g') {
          html += " selected='selected'";
        }
        html += ">gal</option><option value='0'";

        if (mqttData.pulseUnits[z - 1][0] == 'm') {
          html += " selected='selected'";
        }
        html += ">m<sup>3</sup></option><option value='1'";

        if (mqttData.pulseUnits[z - 1][0] == 'f') {
          html += " selected='selected'";
        }
        html += ">ft<sup>3</sup></option><option value='CCF'";

        if (mqttData.pulseUnits[z - 1][0] == 'C') {
          html += " selected='selected'";
        }
        html += ">CCF</option></select>";
      }
    }

    html += "</div>"; */

    html += "<br><button class='button'>Send Config</button>";
    html += "</form></div>";
    html += "<div id='login'><form action='/login-settings'><h3>Login Information</h3>";
    html += "<label>User:</label><input maxlength='20' class='full' type='text' name='user' value='" + String(loginUser) + "'>";
    html += "<label>Password:</label><input maxlength='20' class='full' type='password' name='pass' value=''>";
    html += "<button class='button'>Save</button>";
    html += "</form></div>";
    html += "<div id='login'>";
    html += "<h3>GEM Packets</h3><form action='/start-real'><input type='hidden' name='send_type' value='1'><button class='button'>Start Packets</button></form>";
    html += "<form action='/stop-real'><input type='hidden' name='send_type' value='1'><button class='button'>Stop Packets</button></form>";
    html += "<h3>ECM-1240 Packets</h3><form action='/start-real'><input type='hidden' name='send_type' value='0'><button class='button'>Start Packets</button></form>";
    html += "<form action='/stop-real'><input type='hidden' name='send_type' value='0'><button class='button'>Stop Packets</button></form>";
    html += "</div>";
    html += "<div id='settings'><h3>Device Settings</h3>";
    if (deviceType == 1) {
      html += "<form action='/ecm-settings'><h3>ECM Settings</h3>Type is a fine-tune value that increases the sensed value with each tick (255 Max). Range halves the sensed value with each increase.";
      //html += "<label>Debug</label>" + debugText;
      html += "<label>Settings Retrieved?</label>" + boolToText(ecmSettings.gotSettings, false);
      html += "<label>Serial Number:</label>" + ecmSettings.serialNumber;
      html += "<label>Firmware Version:</label>" + String(ecmSettings.firmwareVersion, 4);
      html += "<label>Packet Send Interval:</label><input name='packet_send' class='small' type='number' min='1' max='255' value='" + String(ecmSettings.sendInterval) + "'> (Max 255)";
      html += "<label>Channel 1 Config:</label>Type: <input name='ch1type' class='small' type='number' min='1' max='255' value='" + String(ecmSettings.ch1Set[0]) + "'> Range: <input class='small' name='ch1range' type='number' min='1' max='255' value='" + String(ecmSettings.ch1Set[1]) + "'>";
      html += "<label>Channel 2 Config:</label>Type: <input name='ch2type' class='small' type='number' min='1' max='255' value='" + String(ecmSettings.ch2Set[0]) + "'> Range: <input class='small' name='ch2range' type='number' min='1' max='255' value='" + String(ecmSettings.ch2Set[1]) + "'>";
      html += "<label>PT (Voltage) Config:</label>Type: <input name='pttype' class='small' type='number' min='1' max='255' value='" + String(ecmSettings.ptSet[0]) + "'>  Range: <input name='ptrange' class='small' type='number' min='1' max='255' value='" + String(ecmSettings.ptSet[1]) + "'>";
      html += "<label>AUX Channel Double:</label>AUX1:  <input type='checkbox' name='aux1x2' " + boolToText(ecmSettings.auxX2[0], true) + "> AUX2: <input type='checkbox' name='aux2x2' " + boolToText(ecmSettings.auxX2[1], true) + "> AUX3: <input type='checkbox' name='aux3x2' " + boolToText(ecmSettings.auxX2[2], true) + "> AUX4:  <input type='checkbox' name='aux4x2' " + boolToText(ecmSettings.auxX2[3], true) + "> AUX5: <input type='checkbox' name='aux5x2' " + boolToText(ecmSettings.auxX2[4], true) + " ><br>";
      html += "<label>Aux 5 Options:</label>Power: <input name='aux5option' " + aux5Opt(0) + " type='radio' value='0'><br>Pulse: <input name='aux5option' " + aux5Opt(1) + " type='radio' value='1'><br>DC: <input name='aux5option' " + aux5Opt(3) + " type='radio' value='3'>";
      html += "<button class='button'>Update Settings</button></form>";
    } else if (deviceType == 2) {
      html += "<b>GreenEye Monitor Detected:</b> " + String(gemSerial) + "<br><br><br>";

      if (tcpServerPort > 0) {
        html += "<a class='button' href='http://" + WiFi.localIP().toString() + ":" + tcpServerPort + "/'>Click Here for Setup</a>";
      } else {
        html += "Setup TCP Server for setup.";
      }

    } else {
      html += "No monitor detected.<br>On occasion the device type poll can fail, please try refreshing the browser window.<br><br>If detection continues to fail, try changing baud rates.<br><br>The ECM-1240 runs at 19200.<br><br>The GreenEye Monitor COM2 setting is generally set to 115200 but it may be set to 19200 on older models.";
    }
    html += "</div>";
    html += "<div id='data'>";
    html += "</div>";
    html += "<div id='fw'><a href='/updater' class='button'>Update ESP Firmware</a><br><a href='/reboot' class='button'>Reboot</a></div><div id='serialDebug'>";
    html += serialDebug();
    html += "</div></body></html>";

    server.sendContent(html);
  }
}

String aux5Opt(uint8_t opt) {
  if (ecmSettings.aux5Option == opt) {
    return "checked='checked'";
  } else {
    return "";
  }
}

String getDigits(int number, int digits) {
  String result = String(number);
  while (result.length() < digits) {
    result = "0" + result;
  }
  return result;
}

String getData() {
  String html = "<h3>Data:</h3><br>";
  int chNum = 32;

  if (deviceType == 1) {
    chNum = 7;
  }

  html += "<table id='infoTable'>";
  html += "<tr><td style='width:125px; padding-bottom:5px;'><b>Serial Number</b></td><td>" + deviceData.serialNumber + "</td></tr>";
  html += "<tr><td><b>Seconds</b></td><td>" + String(deviceData.seconds) + "<br>" + String(deviceData.prevSeconds) + "</td></tr>";
  html += "<tr><td><b>Voltage</b></td><td>" + String(deviceData.voltage) + "</td></tr>";
  html += "</table>";

  html += "<table id='chanTable'>";
  html += "<tr><th>CH</th><th>Wattseconds</th><th>Pol WS</th><th>Watts</th><th>Amps</th><th>kWh</th><th>Net kWh</th></tr>";


  for (int i = 0; i < chNum; i++) {
    if (deviceType != 1 || i < 2) {
      html += "<tr><td><b>C" + String(i + 1) + "</b></td>";
    } else {
      html += "<tr><td><b>AUX" + String(i - 1) + "</b></td>";
    }

    html += "<td>" + String(deviceData.wattSeconds[i]) + "<br>";
    html += String(deviceData.prevWattSeconds[i]) + "</td>";
    html += "<td>" + String(deviceData.polWattSeconds[i]) + "<br>";
    html += String(deviceData.prevPolWattSeconds[i]) + "</td>";
    html += "<td>" + String(deviceData.watts[i]) + "<br>";

    if (deviceData.watts[i] != deviceData.netWatts[i]) {
      html += String(deviceData.netWatts[i]);
    }

    html += "</td><td>" + String(deviceData.amps[i]) + "</td>";
    html += "<td>" + String(deviceData.kwh[i], 4) + "<br>";
    html += String(deviceData.totalKwh[i], 4) + "</td>";
    html += "<td>" + String(deviceData.netKwh[i], 4) + "<br>";
    html += String(deviceData.totalNetKwh[i], 4) + "</td>";
    html += "</tr>";
  }

  if (deviceType == 2) {
    html += "<tr><td><b>T</b></td><td colspan='6'>";
    for (int i = 0; i < 8; i++) {
      html += "C" + String(i + 1) + ": " + String(deviceData.temp[i], 2) + " ";
    }
    html += "</td></tr>";

    html += "<tr><td><b>P</b></td><td colspan='6'>";
    for (int i = 0; i < 4; i++) {
      html += "C" + String(i + 1) + ": " + String(deviceData.pulse[i]) + " ";
    }
    html += "</td></tr>";
  }

  html += "</table>";

  return html;
}

String getHTMLHeader(uint8_t pageNum) {
  String htmlHeader = "<!DOCTYPE html lang='en'>";
  htmlHeader += "<head><title>Brultech ESP-8266 Setup</title>";
  htmlHeader += "<style>";
  htmlHeader += "body { background-color:#c3c3c3; font-family: Arial, sans-serif; }";
  htmlHeader += "select { font-size: 16px; font-family: Arial, sans-serif; }";
  htmlHeader += "div { background-color: #fff; border: 1px solid #ccc; box-shadow: 0 2px 2px rgba(0, 0, 0, 0.1); margin: 50px auto; width: 600px; padding: 20px; text-align: center; }";
  htmlHeader += "h1 { margin: 0 0 20px 0; }";
  htmlHeader += "label { padding-top:5px; display: block; font-size: 16px; font-weight: bold; margin-bottom: 5px; text-align: left; }";
  htmlHeader += ".full { box-sizing: border-box; border: 1px solid #ccc; font-size: 16px; padding: 10px; width: 100% ; }";
  htmlHeader += ".small { box-sizing: border-box; border: 1px solid #ccc; font-size: 16px; padding: 10px; width: 100px ; }";
  htmlHeader += ".mid { box-sizing: border-box; border: 1px solid #ccc; font-size: 16px; padding-left: 10px; padding-right: 10px; width: 150px ; }";
  htmlHeader += ".button { background-color: #4CAF50; border: none; color: #fff; cursor: pointer; font-size: 16px; margin-top: 20px; padding: 10px; display:inline-block; margin:5px; text-decoration: none; }";
  htmlHeader += ".button:hover { background-color: #45a049; }";
  htmlHeader += "p.error { color: #f00; font-size: 14px; margin: 10px 0; text-align: left; }";
  htmlHeader += "#chanTable {width: 100%; border-collapse: collapse;}";
  htmlHeader += "#chanTable th, td {border: 1px solid black; padding: 8px; text-align: center;}";
  htmlHeader += "#chanTable th {background-color: #f2f2f2;}";
  htmlHeader += "#infoTable { margin: 0 auto; border:none; }";
  htmlHeader += "#infoTable td { border:none; }";
  htmlHeader += "#pleaseWait { width:500px; }";
  htmlHeader += "</style>";
  htmlHeader += "<script>";

  if (pageNum != 4) {
    if (pageNum == 1) {
      htmlHeader += "var ip = location.host; ";
      htmlHeader += "document.addEventListener('DOMContentLoaded', function() { ";
      htmlHeader += "var selectBox = document.getElementById('ssid'); ";
      htmlHeader += "var textbox = document.getElementById('custom_ssid'); ";
      htmlHeader += "selectBox.addEventListener('change', function() { ";
      htmlHeader += "    textbox.value = selectBox.value; ";
      htmlHeader += "}); ";
      htmlHeader += "});";
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
      /*htmlHeader += "window.addEventListener('DOMContentLoaded', (event) => {";
      htmlHeader += "const myDiv = document.getElementById('advancedMqtt');";
      htmlHeader += "const toggleLink = document.getElementById('toggleMqtt');";
      htmlHeader += "toggleLink.addEventListener('click', function(event) {";
      htmlHeader += "event.preventDefault();";
      htmlHeader += "if (myDiv.style.display === 'none') {";
      htmlHeader += "myDiv.style.display = 'block';";
      htmlHeader += "this.innerHTML = 'Close Advanced Config';";
      htmlHeader += "} else {";
      htmlHeader += "myDiv.style.display = 'none';";
      htmlHeader += "this.innerHTML = 'Open Advanced Config';";
      htmlHeader += "}";
      htmlHeader += "});";
      htmlHeader += "});";*/
    } else if (pageNum == 2) {
      htmlHeader += "function goBack() { history.back(); }; setInterval(goBack,5000);";
    } else {
      htmlHeader += "document.addEventListener('DOMContentLoaded', function() { ";
      htmlHeader += "var selectBox = document.getElementById('ssid'); ";
      htmlHeader += "var textbox = document.getElementById('custom_ssid'); ";
      htmlHeader += "selectBox.addEventListener('change', function() { ";
      htmlHeader += "    textbox.value = selectBox.value; ";
      htmlHeader += "}); ";
      htmlHeader += "var link = document.getElementById('localLink'); ";
      htmlHeader += "function copyText() { ";
      htmlHeader += "var linkText = link.innerText || link.textContent; ";
      htmlHeader += "navigator.clipboard.writeText(linkText); ";
      htmlHeader += "}; ";
      htmlHeader += "link.addEventListener('click', function(event) { ";
      htmlHeader += "event.preventDefault(); ";
      htmlHeader += "copyTextToClipboard('localLink'); ";
      htmlHeader += "});";
      htmlHeader += "document.getElementById('saveLocal').addEventListener('click', () => {";
      htmlHeader += "    const textToCopy = 'http://" + localAddress + ".local';";
      htmlHeader += "    const textArea = document.createElement('textarea');";
      htmlHeader += "    textArea.value = textToCopy;";
      htmlHeader += "    document.body.appendChild(textArea);";
      htmlHeader += "    textArea.select();";
      htmlHeader += "    try {";
      htmlHeader += "        document.execCommand('copy');";
      htmlHeader += "        alert('Local address copied to clipboard.');";
      htmlHeader += "    } catch (err) {";
      htmlHeader += "        console.error('Failed to copy text: ', err);";
      htmlHeader += "    }";
      htmlHeader += "    document.body.removeChild(textArea);";
      htmlHeader += "});";
      htmlHeader += "});";
    }
  }

  htmlHeader += "</script>";
  htmlHeader += "</head>";
  htmlHeader += "<body>";

  return htmlHeader;
}

void handleMQTTDebug() {
  String htmlPage = "<!DOCTYPE html>\n";
  htmlPage += "<html>\n";
  htmlPage += "<head>\n";
  htmlPage += "<title>MQTT Data</title>\n";
  htmlPage += "</head>\n";
  htmlPage += "<body>\n";
  htmlPage += "<h1>MQTT Data</h1>\n";
  htmlPage += "<p>Configuration Status: ";
  htmlPage += (mqttData.isConfigured ? "Configured" : "Not Configured");
  htmlPage += "</p>\n";

  // Loop through channel data and display
  for (int i = 0; i < 44; ++i) {
    htmlPage += "<p>";
    htmlPage += "Channel ";
    htmlPage += String(i);
    htmlPage += ": ";
    htmlPage += (mqttData.channelEnabled[i] ? "Enabled" : "Disabled");
    htmlPage += " - ";
    htmlPage += mqttData.labels[i];
    if (i > 40) {
      htmlPage += " - ";
      htmlPage += String(mqttData.pulseUnits[i][0]);
      htmlPage += " - ";
      htmlPage += String(mqttData.pulseTypes[i][0]);
    }
    htmlPage += "</p>\n";
  }

  // Add any other data you want to display

  htmlPage += "</body>\n";
  htmlPage += "</html>\n";

  server.send(200, "text/html", htmlPage);
}

void handleData() {
  server.send(200, "text/html", getData());
}

void handleSerialDebug() {
  server.send(200, "text/html", serialDebug());
}

void handleStartReal() {
  uint8_t sendType = server.arg("send_type").toInt();

  if (sendType == 0) {
    byte data = 0xFC;  // binary 0xFC
    Serial.write(data);
    delay(50);
    Serial.write("TOG");
    delay(50);
    Serial.write("XTD");
    delay(50);
  } else {
    Serial.write("^^^SYS_ON\r\n");
  }

  server.on("/", HTTP_GET, []() {
    server.sendHeader("Location", "/main", true);  // Redirect to /main
    server.send(302, "text/plain", "Redirecting to /main");
  });
}

void handleStopReal() {
  uint8_t sendType = server.arg("send_type").toInt();

  if (sendType == 0) {
    byte data = 0xFC;  // binary 0xFC
    Serial.write(data);
    delay(50);
    Serial.write("TOG");
    delay(50);
    Serial.write("OFF");
    delay(50);
  } else {
    Serial.write("^^^SYSOFF\r\n");
  }

  server.on("/", HTTP_GET, []() {
    server.sendHeader("Location", "/main", true);  // Redirect to /main
    server.send(302, "text/plain", "Redirecting to /main");
  });
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
  String html = getHTMLHeader(2) + "<div>";
  // Generate the HTML page with the variable values
  if (ch1type > 0 && ch2type > 0 && ch1range > 0 && ch2range > 0 && pttype > 0 && ptrange > 0 && packet_send > 0) {
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
        html += "<h3>Settings saved.</h3><br>";
      } else {
        html += "<h3>No change detected.</h3><br>";
      }
    } else {
      bool change = false;
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
        change = true;
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
        change = true;
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
        change = true;
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
        change = true;
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
        change = true;
      }

      if (change) {
        html += "<h3>Settings saved.</h3><br>";
      } else {
        html += "<h3>No change detected.</h3><br>";
      }
    }

    html += commandsSent;
  } else {
    html += "All values must be non-zero.";
  }
  html += "Click <a href='/main'>here</a> to return to the settings page.";

  html += "</div></body></html>";

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
    if (WiFi.status() == WL_CONNECTED) {
      server.sendHeader("Location", "/main");
    } else {
      server.sendHeader("Location", "/apmain");
    }

    server.send(302);
  }
}

void sendLogin(bool error) {
  // Send the login HTML page if not authenticated
  String html = getHTMLHeader(0) + "<div>";

  if (error) {
    html += "<p class='error'>Invalid username or password.</p>";
  }
  html += "<form action='/login' method='post'><label for='username'>Username:</label> <input class='full' type='text' id='username' name='username'> <label for='password'>Password:</label> <input class='full' type='password' id='password' name='password'> <button class='button' type='submit'>Login</button> </form> </div> </body> </html>";
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
  IPAddress ip;
  int port = server.arg("port").toInt();
  IPAddress ipStore;

  String html = getHTMLHeader(2);

  // Error test the client connection
  if (ip.fromString(server.arg("ip")) && port > 1024 && port < 65536) {

    storeIP(tcpIPAddress, ip);
    EEPROM.put(tcpPortAddress, port);
    EEPROM.commit();

    tcpIP = ip;
    tcpPort = port;

    html += "<h2>IP: " + ip.toString() + " Port: " + port + +" saved to EEPROM.  Starting server...</h2>";

    if (tcpClient.connected()) {
      tcpClient.stop();

      if (tcpClient.connect(tcpIP, tcpPort)) {
        tcpClient.setTimeout(100);
        html += "<h5>Connected to TCP server</h5>";
      } else {
        html += "<h5>Connection failed</h5>";
      }
    }

  } else if (server.arg("ip") == "") {
    tcpIP = IPAddress(0, 0, 0, 0);
    storeIP(tcpIPAddress, IPAddress(0, 0, 0, 0));
  } else {
    html += "<h2>Invalid IP Address or Port</h2>";
  }

  html += "</body></html>";
  server.send(200, "text/html", html);
}

void handleSerialToTcpServer() {
  // Serial to TCP client connection
  int port = server.arg("port").toInt();
  uint16_t iTime = server.arg("idle_time").toInt();

  String html = getHTMLHeader(2);

  // Error test the client connection
  if ((port > 1024 && port < 65536 && iTime > 0 && iTime < 6001) || port == 0) {

    EEPROM.put(tcpServerPortAddress, port);
    EEPROM.commit();
    EEPROM.put(idleTimeAddress, iTime);
    EEPROM.commit();

    idleTime = iTime;

    tcpServerPort = port;

    if (port != 0) {
      html += "<h2>Port saved to EEPROM.  Starting server...</h2>";
    } else {
      html += "<h2>TCP Server Mode has been disabled.</h2>";
    }

    ecmServer.stop();
    ecmServer.begin(tcpServerPort);
    //ecmServer.setNoDelay(true);
  } else {
    html += "<h2>Port or Idle Time is out of range.</h2>";
  }

  html += "</body></html>";
  server.send(200, "text/html", html);
}


void handleNTPServer() {
  String tempNTP = server.arg("ntp_server");

  String html = getHTMLHeader(2);
  if (strlen(tempNTP.c_str()) < 41) {
    tempNTP.toCharArray(ntpServer, 40);
    EEPROM.put(ntpServerAddress, ntpServer);
    EEPROM.commit();

    html += "<h2>" + String(ntpServer) + " saved to EEPROM.  Starting server...</h2>";

    ntpClient.end();

    ntpClient.setPoolServerName(ntpServer);

    ntpClient.begin();
  } else {
    html += "<h2>NTP Server Address is too longer then 50 characters.</h2>";
  }

  html += "</body></html>";
  server.send(200, "text/html", html);
}
void handleMqtt() {

  // Serial to TCP client connection
  IPAddress ip;
  String user = server.arg("user");
  String pass = server.arg("pass");
  int port = server.arg("port").toInt();

  String html = getHTMLHeader(2);

  // Error test the client connection
  if (port > 1024 && port < 65536 && ip.fromString(server.arg("ip"))) {
    EEPROM.commit();

    mqttServer = ip;
    mqttPort = port;
    strcpy(mqttUser, user.c_str());
    strcpy(mqttPass, pass.c_str());

    storeIP(mqttServerAddress, mqttServer);
    EEPROM.put(mqttPortAddress, mqttPort);
    EEPROM.put(mqttUserAddress, mqttUser);
    EEPROM.put(mqttPassAddress, mqttPass);

    EEPROM.commit();

    mqttClient.disconnect();

    mqttClient.setServer(mqttServer, mqttPort);

    html += "<div><h3>Address: " + mqttServer.toString() + " Port: " + String(port) + " User: " + String(mqttUser) + " Pass:  " + String(mqttPass) + " saved to EEPROM.</h3></div>";
    if (!mqttClient.connect(mqttClientID, mqttUser, mqttPass)) {
      html += "<div><h3>MQTT couldn't connect, please check your settings.</h3></div>";
    } else {
      mqttClient.disconnect();
    }
  } else if (server.arg("ip") == "") {
    mqttServer = IPAddress(0, 0, 0, 0);
    storeIP(mqttServerAddress, IPAddress(0, 0, 0, 0));
  } else {
    html += "<h2>Invalid IP Address or Port</h2>";
  }

  html += "</body></html>";
  server.send(200, "text/html", html);
}

void handleBaud() {
  // Serial to TCP client connection
  uint32_t baudRate = server.arg("baud").toInt();

  String html = getHTMLHeader(2);

  // Error test the client connection
  if (baudRate == 19200 || baudRate == 115200) {
    EEPROM.put(baudAddress, baudRate);
    EEPROM.commit();

    if (baudRate != baud) {
      Serial.updateBaudRate(baudRate);
    }

    baud = baudRate;

    html += "<div><h3>Baudrate " + String(baudRate) + " saved.</h3></div>";
  } else {
    html += "<div><h3>Invalid Baudrate</h3></div>";
  }

  html += "</body></html>";
  server.send(200, "text/html", html);
}

void handleIPConfig() {
  IPAddressConfig config;
  uint8_t type = server.arg("type").toInt();
  String html = getHTMLHeader(2);

  // Parse form data and validate IP settings
  if (type == 1) {
    if (config.ip.fromString(server.arg("ip")) && config.subnet.fromString(server.arg("subnet")) && config.gateway.fromString(server.arg("gateway")) && config.dns.fromString(server.arg("dns"))) {
      config.isConfigured = true;

      // Write the IP settings structure to EEPROM
      EEPROM.put(ipConfigAddress, config);

      EEPROM.commit();  // Save changes to EEPROM

      storedIPConfig = config;

      html += "<div><h3>IP settings saved, click <a href='http://" + config.ip.toString() + "/'>here</a> to access the unit.<br><br>If you can't access the module afterwards it can be reset by using the push button.</h3></div></body></html>";

      server.send(200, "text/html", html);

      delay(250);

      WiFi.config(config.ip, config.dns, config.gateway, config.subnet);
    } else {
      html += "<div><h3>Invalid IP Address, Subnet, Gateway, or DNS.</h3></div></body></html>";

      server.send(200, "text/html", html);
    }
  } else {
    storedIPConfig.isConfigured = false;

    // Write the IP settings structure to EEPROM
    EEPROM.put(ipConfigAddress, storedIPConfig);

    EEPROM.commit();  // Save changes to EEPROM

    html += "<div><h3>IP settings changed to DHCP, click <a href='http://brultechesp.local/'>here</a> to access the unit.<br><br>If you can't access the module afterwards it can be reset by using the push button.</h3></div></body></html>";

    server.send(200, "text/html", html);

    delay(250);

    WiFi.config(0, 0, 0);
  }
}

void handleLoginSettings() {
  // Serial to TCP client connection
  String user = server.arg("user");
  String pass = server.arg("pass");

  String html = getHTMLHeader(2);

  strcpy(loginUser, user.c_str());
  strcpy(loginPass, pass.c_str());

  EEPROM.put(loginUserAddress, loginUser);
  EEPROM.put(loginPassAddress, loginPass);
  EEPROM.commit();

  httpUpdater.setup(&server, loginUser, loginPass);

  html += "<div><h3>User: " + String(loginUser) + " Pass:  " + String(loginPass) + " saved to EEPROM.</h3></div></body></html>";

  server.send(200, "text/html", html);
}

void storeIP(int addressOffset, const IPAddress& ip) {
  for (int i = 0; i < 4; i++) {
    EEPROM.write(addressOffset + i, ip[i]);
  }

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

bool isAuthenticated() {
  if (loginUser[0] != '\0' && loginPass[0] != '\0') {
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

IPAddress getIP(int location) {
  uint8_t storedIPBytes[4];
  for (int i = 0; i < 4; i++) {
    storedIPBytes[i] = EEPROM.read(location + i);
  }

  if (IPAddress(storedIPBytes).isSet()) {
    return IPAddress(storedIPBytes);
  } else {
    return IPAddress(0, 0, 0, 0);
  }
}

String getFormattedDate() {
  unsigned long rawTime = ntpClient.getEpochTime() / 86400L;  // in days
  unsigned long days = 0, year = 1970;
  uint8_t month;
  static const uint8_t monthDays[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };

  while ((days += (LEAP_YEAR(year) ? 366 : 365)) <= rawTime)
    year++;
  rawTime -= days - (LEAP_YEAR(year) ? 366 : 365);  // now it is days in this year, starting at 0
  days = 0;
  for (month = 0; month < 12; month++) {
    uint8_t monthLength;
    if (month == 1) {  // february
      monthLength = LEAP_YEAR(year) ? 29 : 28;
    } else {
      monthLength = monthDays[month];
    }
    if (rawTime < monthLength) break;
    rawTime -= monthLength;
  }
  String monthStr = ++month < 10 ? "0" + String(month) : String(month);      // jan is month 1
  String dayStr = ++rawTime < 10 ? "0" + String(rawTime) : String(rawTime);  // day of month

  if (year != 1970) {
    return String(year) + "-" + monthStr + "-" + dayStr + "T" + ntpClient.getFormattedTime() + "Z";
  } else {
    return "Unable to retrieve date, check NTP Server settings.";
  }
}
