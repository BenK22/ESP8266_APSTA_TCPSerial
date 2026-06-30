/**
 * @file ESP8266_TCPSerialBridge_2.ino
 * @brief ESP8266 Firmware for Brultech ECM-1240 and GreenEye Monitor (GEM)
 *
 * This firmware enables the ESP8266 to act as a bridge between the serial output
 * of Brultech energy monitors (ECM-1240 and GEM) and network services.
 *
 * Key Features:
 * - Reads serial data from ECM/GEM devices.
 * - Parses data packets (ECM, GEM, GEM Large).
 * - Publishes energy data to MQTT.
 * - Provides a TCP server/client for raw data bridging.
 * - Web-based configuration interface for WiFi, MQTT, and device settings.
 * - Supports Over-The-Air (OTA) firmware updates.
 * - Network discovery via UDP.
 */
 
#include <ESP8266WiFi.h>
#include <EEPROM.h>
#include <ESP8266WebServer.h>
#include <PubSubClient.h>
#include <ESP8266mDNS.h>
#include <ESP8266HTTPUpdateServer.h>
#include <Ticker.h>
#include <WiFiUdp.h>
#include <ArduinoJson.h>
#include <time.h>

constexpr const char FW_VERSION[] = "v2.09";


#define LEAP_YEAR(Y) ((Y > 0) && !(Y % 4) && ((Y % 100) || !(Y % 400)))
#define HTTP_MAX_HEADER_SIZE 4096

// Web Assets Forward Declarations
extern const char HTML_CSS[] PROGMEM;
extern const char HTML_JS_PAGE1[] PROGMEM;
extern const char HTML_JS_PAGE2[] PROGMEM;

enum class PageType {
  Login = 0,
  Main = 1,
  Reboot = 2,
  ConfigSaved = 4
};

enum class DeviceType : uint8_t {
  Unknown = 0,
  GEM = 1,
  ECM = 2
};

// EEPROM memory locations
const int eepromSize = 4096;
const int ssidAddress = 0;

// addresses
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


// old address
const int passwordAddress = 32;

//  shifted password location for legacy units, older F/W did not support full passwords
const int isNewPasswordAddress = 1001;
const int newPasswordAddress = 1002;
const int idleTimeAddress = 1069;
const int dbPowerAddress = 1169;

// Constants
const int MQTT_MAX_CHANNELS = 44;
const int MAX_PULSE_COUNTERS = 4;
const int MAX_TEMP_SENSORS = 8;

// MQTT config
/**
 * @brief Structure to hold MQTT configuration for channels.
 */
struct MqttData {
  bool channelEnabled[MQTT_MAX_CHANNELS] = { true };  ///< Enable/disable status for each channel
  char pulseUnits[4][5];                              ///< Units for pulse counters
  char pulseTypes[4][4];                              ///< Types for pulse counters
  char tempUnits[8];                                  ///< Units for temperature sensors
  char labels[MQTT_MAX_CHANNELS][15];                 ///< Custom labels for channels
  bool isConfigured = false;                          ///< Flag to check if MQTT is configured
};

// System Configuration
/**
 * @brief Structure to hold system configuration settings.
 */
struct SystemConfig {
  char ssid[32] = "";                   ///< WiFi SSID
  char password[65] = "";               ///< WiFi Password
  char loginUser[20] = "";              ///< Web Interface Username
  char loginPass[20] = "";              ///< Web Interface Password
  uint16_t tcpPort = 0;                 ///< TCP Client Port
  uint16_t tcpServerPort = 8000;        ///< TCP Server Port
  uint16_t idleTime = 5;                ///< TCP Server Idle Timeout (seconds)
  uint8_t dbPower = 20;                 ///< WiFi Output Power (dBm)
  uint32_t baud = 115200;               ///< Serial Baud Rate
  char ntpServer[40] = "pool.ntp.org";  ///< NTP Server Address
};

/**
 * @brief Structure to hold IP address configuration.
 */
struct IPAddressConfig {
  bool isConfigured = false;  // Flag indicating if configuration is stored
  IPAddress ip = IPAddress(192, 168, 1, 100);
  IPAddress gateway = IPAddress(192, 168, 1, 1);
  IPAddress subnet = IPAddress(255, 255, 255, 0);
  IPAddress dns = IPAddress(8, 8, 8, 8);  // DNS server 1
};

/**
 * @brief Structure to hold parsed device data.
 */
struct DeviceData {
  double voltage = 0.0;
  uint64_t wattSeconds[32] = { 0 };
  uint64_t prevWattSeconds[32] = { 0 };
  uint32_t deltaWattSeconds[32] = { 0 };
  uint64_t polWattSeconds[32] = { 0 };
  uint64_t prevPolWattSeconds[32] = { 0 };
  float amps[32] = { 0 };
  uint32_t seconds = 0;
  uint32_t prevSeconds = 0;
  String serialNumber = "";
  uint16_t watts[32] = { 0 };
  int netWatts[32] = { 0 };
  float kwh[32] = { 0.0 };
  float netKwh[32] = { 0.0 };
  float totalKwh[32] = { 0.0 };
  float totalNetKwh[32] = { 0.0 };
  float temp[8] = { 0.0 };
  uint64_t pulse[4] = { 0.0 };
  double dcVoltage = 0.0;
};

/**
 * @brief Structure to hold ECM-1240 specific settings.
 */
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

namespace Packet {
// Framing bytes
constexpr uint8_t HEADER_0 = 0xFE;
constexpr uint8_t HEADER_1 = 0xFF;
constexpr uint8_t FOOTER_0 = 0xFF;
constexpr uint8_t FOOTER_1 = 0xFE;

// Packet format/type identifiers
constexpr uint8_t TYPE_ECM = 0x03;
constexpr uint8_t TYPE_GEM = 0x07;
constexpr uint8_t TYPE_GEM_LARGE = 0x05;

// Offsets
constexpr size_t TYPE_OFFSET = 2;
constexpr size_t ECM_SIZE = 64;
constexpr size_t GEM_SIZE = 428;
constexpr size_t GEM_LARGE_SIZE = 624;

namespace ECM {
constexpr size_t VOLTAGE_HI = 3;
constexpr size_t VOLTAGE_LO = 4;
constexpr size_t SECONDS_LO = 37;
constexpr size_t SECONDS_MD = 38;
constexpr size_t SECONDS_HI = 39;
constexpr size_t WATT_SECONDS_START = 5;
constexpr size_t POL_WATT_SECONDS_START = 15;
constexpr size_t SERIAL_LO = 29;
constexpr size_t SERIAL_HI = 30;
constexpr size_t SERIAL_TYPE = 32;
constexpr size_t DC_VOLTAGE_LO = 60;
constexpr size_t DC_VOLTAGE_HI = 61;
}

struct GEM {
  static constexpr size_t VOLTAGE_HI = 3;
  static constexpr size_t VOLTAGE_LO = 4;
  static constexpr size_t SECONDS_LO = 393;
  static constexpr size_t SECONDS_MD = 394;
  static constexpr size_t SECONDS_HI = 395;
  static constexpr size_t WATT_SECONDS_START = 5;
  static constexpr size_t POL_WATT_SECONDS_START = 165;
  static constexpr size_t AMPS_LO_START = 329;
  static constexpr size_t AMPS_HI_START = 330;
  static constexpr size_t TEMP_LO_START = 408;
  static constexpr size_t TEMP_HI_START = 409;
  static constexpr size_t PULSE_START = 396;
  static constexpr size_t SERIAL_HI = 325;
  static constexpr size_t SERIAL_LO = 326;
  static constexpr size_t ID = 328;
};

struct GEMLarge {
  static constexpr size_t VOLTAGE_HI = 3;
  static constexpr size_t VOLTAGE_LO = 4;
  static constexpr size_t SECONDS_LO = 585;
  static constexpr size_t SECONDS_MD = 586;
  static constexpr size_t SECONDS_HI = 587;
  static constexpr size_t WATT_SECONDS_START = 5;
  static constexpr size_t POL_WATT_SECONDS_START = 245;
  static constexpr size_t AMPS_HI_START = 489;
  static constexpr size_t AMPS_LO_START = 490;
  static constexpr size_t TEMP_LO_START = 600;
  static constexpr size_t TEMP_HI_START = 601;
  static constexpr size_t PULSE_START = 588;
  static constexpr size_t SERIAL_HI = 485;
  static constexpr size_t SERIAL_LO = 486;
  static constexpr size_t ID = 488;
};
}

// Different overflow counters for wattseconds/seconds counters
constexpr uint64_t WS_OVERFLOW[] = {
  0,           // index 0 (unused)
  0,           // index 1 (unused)
  0,           // index 2 (unused)
  1ULL << 24,  // 256^3
  1ULL << 32,  // 256^4
  1ULL << 40   // 256^5
};


IPAddress mqttServer = IPAddress(0, 0, 0, 0);
char mqttUser[20] = {};
char mqttPass[20] = {};
char mqttClientID[20] = {};
uint16_t mqttPort = 1883;
MqttData mqttData;
SystemConfig sysConfig;


// UDP config
const uint16_t udpPort = 48925;
WiFiUDP UDP;
char udpPacket[255];
String udpResponse;
IPAddress broadcastIP(255, 255, 255, 255);
StaticJsonDocument<128> doc;


// WiFi config
String apName = "Brultech-";
char apPassword[9] = "brultech";
bool inAP = false;
String networkOptions = "";
String tcpClientConnect = "Not connected.";
int connectTries = 0;

WiFiClient ecmClient;  // Declare globally

IPAddressConfig storedIPConfig;

int startTime = millis();

String gemSerial = "";
DeviceType deviceType = DeviceType::Unknown;
String deviceName = "";

DeviceData deviceData;
EcmSettings ecmSettings;

// Serial buffer
const int MAX_DATA_LENGTH = 2048;      // Set the maximum length of the data
char buffer[MAX_DATA_LENGTH];          // Declare the array to store the data
char sharedBuffer[MAX_DATA_LENGTH];    // Declare the array to store the data
char settingsBuffer[MAX_DATA_LENGTH];  // Declare the array to store the data
int dataLength = 0;                    // Declare a variable to keep track of the length of the data
int sharedDataLength = 0;              // Declare a variable to keep track of the length of the data
int settingsLength = 0;                // Declare a variable to keep track of the length of the data
bool newData = false;

// Web Server config
WiFiClient webServerClient;

ESP8266WebServer server(80);
ESP8266HTTPUpdateServer httpUpdater;
PubSubClient mqttClient(webServerClient);
String mqttStatus = "";


// TCP Client/Server
WiFiClient tcpClient;
IPAddress tcpIP = IPAddress(192, 168, 4, 1);

WiFiServer ecmServer(5555);


// Misc
const byte isFirstRunValue = 0xAA;
const byte isNewPasswordValue = 0xAA;
const char* headerKeys[] = { "User-Agent", "Cookie", "Content-Type", "Content-Length", "Update-Size" };
size_t headerKeysSize = sizeof(headerKeys) / sizeof(char*);
uint8_t mac[6];
bool resetFlag = false;

String globalSessionId = "";
//String debugText = "";

String errorMsg = "";

String localAddress = "";

// LED & Reset config
const int LED_PIN = 2;    // GPIO2
const int RESET_PIN = 0;  // GPIO0
Ticker tickerSlow;
Ticker tickerSTA;
Ticker tickerAP;
static bool ledState = false;

const float TICKER_SLOW_SEC = 2.0;
const float TICKER_STA_SEC = 0.5;
const float TICKER_AP_SEC = 1.0;

const int UPDATE_SIZE_UNKNOWN = -1;

String escapeHtml(String text) {
  String out = text;
  out.replace("&", "&amp;");
  out.replace("<", "&lt;");
  out.replace(">", "&gt;");
  out.replace("\"", "&quot;");
  out.replace("'", "&#39;");
  return out;
}

/**
 * @brief Interrupt handler to set the reset flag.
 */
void ICACHE_RAM_ATTR resetToAP() {
  resetFlag = true;
}

/**
 * @brief Toggles the state of the LED.
 */
void toggleLED() {
  ledState = !ledState;
  digitalWrite(LED_PIN, ledState);
}

/**
 * @brief Standard Arduino setup function.
 * 
 * Initializes pins, WiFi, EEPROM, Serial, and Web Server.
 */
void setup() {
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  tickerSlow.attach(TICKER_SLOW_SEC, toggleLED);  // Start the thread

  WiFi.macAddress(mac);

  snprintf(mqttClientID, sizeof(mqttClientID), "%02X%02X%02X", mac[3], mac[4], mac[5]);

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
    EEPROM.get(passwordAddress, sysConfig.password);
    EEPROM.put(newPasswordAddress, sysConfig.password);
    EEPROM.commit();

    EEPROM.write(isNewPasswordAddress, isNewPasswordValue);
    EEPROM.commit();
  }

  // Read settings from EEPROM
  EEPROM.get(ssidAddress, sysConfig.ssid);
  EEPROM.get(newPasswordAddress, sysConfig.password);
  EEPROM.get(dbPowerAddress, sysConfig.dbPower);

  if (sysConfig.dbPower < 12) {
    sysConfig.dbPower = 20;
  }

  EEPROM.get(baudAddress, sysConfig.baud);
  EEPROM.get(ntpServerAddress, sysConfig.ntpServer);
  String ntpServerString = String(sysConfig.ntpServer);

  if (ntpServerString.isEmpty()) {
    strcpy(sysConfig.ntpServer, "pool.ntp.org");
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

  if (sysConfig.baud != 19200 && sysConfig.baud != 115200) {
    sysConfig.baud = 115200;
  }

  // Start serial port
  Serial.begin(sysConfig.baud, SERIAL_8N1);
  Serial.setRxBufferSize(1024);
  Serial.flush();
  Serial.setTimeout(100);

  tcpIP = getIP(tcpIPAddress);
  mqttServer = getIP(mqttServerAddress);

  // Read settings from EEPROM
  EEPROM.get(loginUserAddress, sysConfig.loginUser);
  EEPROM.get(loginPassAddress, sysConfig.loginPass);

  EEPROM.get(tcpPortAddress, sysConfig.tcpPort);
  EEPROM.get(tcpServerPortAddress, sysConfig.tcpServerPort);


  EEPROM.get(idleTimeAddress, sysConfig.idleTime);

  if (sysConfig.idleTime == 0) {
    sysConfig.idleTime = 5;
  }

  //here the list of headers to be recorded
  setupWebServer();
  scanNetworks();
  setupWiFi();
  //getDeviceSettings();

  pinMode(RESET_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(RESET_PIN), resetToAP, FALLING);
}


/**
 * @brief Loads MQTT settings from EEPROM.
 */
void loadMQTTSettings() {
  EEPROM.get(mqttUserAddress, mqttUser);
  EEPROM.get(mqttPassAddress, mqttPass);

  EEPROM.get(mqttPortAddress, mqttPort);
  EEPROM.get(mqttDataAddress, mqttData);

  if (!mqttData.isConfigured) {
    // Initialize mqttData
    for (int x = 0; x < MQTT_MAX_CHANNELS; x++) {
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

/**
 * @brief Configures WiFi connection.
 */
void setupWiFi() {
  WiFi.setOutputPower(sysConfig.dbPower);
  // Connect to saved network
  tickerSlow.detach();  // Stop the ticker
  if (strcmp(sysConfig.ssid, "") != 0 && strcmp(sysConfig.password, "") != 0 && WiFi.status() != WL_CONNECTED) {
    int networksFound = WiFi.scanNetworks();
    bool found = false;

    for (int i = 0; i < networksFound; i++) {
      if (String(sysConfig.ssid).equals(WiFi.SSID(i))) {
        found = true;
        break;
      }
    }

    if (found) {
      tickerAP.detach();
      tickerSTA.attach(TICKER_STA_SEC, toggleLED);  // Start the thread
      WiFi.mode(WIFI_STA);
      WiFi.setPhyMode(WIFI_PHY_MODE_11G);
      WiFi.begin(sysConfig.ssid, sysConfig.password);
      WiFi.setAutoReconnect(true);
      WiFi.persistent(true);

      inAP = false;

      const unsigned long timeout = 15000;
      unsigned long start = millis();
      while (WiFi.status() != WL_CONNECTED && millis() - start < timeout) {
        yield();    // let background tasks run, feed watchdog
        delay(10);  // small delay to avoid hammering CPU too hard
      }

      if (WiFi.status() == WL_CONNECTED) {
        tickerSTA.detach();          // Stop the ticker
        digitalWrite(LED_PIN, LOW);  // Turn off the LED

        MDNS.begin(localAddress);

        if (sysConfig.tcpServerPort != 0) {
          ecmServer.stop();
          ecmServer.begin(sysConfig.tcpServerPort);
          //ecmServer.setNoDelay(true);
        }

        UDP.begin(udpPort);

        configTime(0, 0, sysConfig.ntpServer);

        delay(1000);
      } else {
        tickerSTA.detach();  // Stop the ticker
      }
    }
  }

  // If not connected, start in Access Point mode
  if (WiFi.status() != WL_CONNECTED && !inAP) {
    tickerAP.attach(TICKER_AP_SEC, toggleLED);  // Start the thread

    // Start WiFi in Access Point mode
    inAP = true;
    WiFi.mode(WIFI_AP);
    WiFi.softAP(apName, apPassword);

    if (sysConfig.tcpServerPort != 0) {
      ecmServer.stop();
      ecmServer.begin(sysConfig.tcpServerPort);
      //ecmServer.setNoDelay(true);
    }
  }
}

/**
 * @brief Sets up the Web Server routes and handlers.
 */
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
  server.on("/ecm-reset", handleECMReset);
  server.on("/serial-to-tcp-server", handleSerialToTcpServer);
  server.on("/mqtt", handleMqtt);
  server.on("/baud", handleBaud);
  server.on("/ip-config", handleIPConfig);
  server.on("/send-ha", handleHA);
  server.on("/data", handleData);
  server.on("/serial-debug", handleSerialDebug);
  server.on("/mqtt-debug", handleMQTTDebug);
  server.on("/mqtt-test", mqttPost);
  server.on("/ntp-server", handleNTPServer);
  server.on("/reboot", handleReboot);
  server.on("/updater", handleUpdate);
  server.on("/apmain", handleAP);
  server.on("/scan", handleScan);
  server.collectHeaders(headerKeys, headerKeysSize);
  httpUpdater.setup(&server, sysConfig.loginUser, sysConfig.loginPass);
  server.begin();
}

// Forward declaration
void sendHTMLHeader(PageType pageNum);

/**
 * @brief Handles the firmware update page request.
 */
void handleUpdate() {
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/html", "");
  sendHTMLHeader(PageType::Login);
  server.sendContent(F("<div><h2>Firmware Upgrade</h2>"));
  server.sendContent(F("<form id='updateForm' method='POST' action='/update' enctype='multipart/form-data'>"));
  server.sendContent(F("<label>Firmware:</label>"));
  server.sendContent(F("<input type='file' accept='.bin,.bin.gz' name='firmware'>"));
  server.sendContent(F("<br><br><button class='button'>Update Firmware</button>"));
  server.sendContent(F("</form>"));
  server.sendContent(F("<div id='pleaseWait' style='display: none;'>"));
  server.sendContent(F("<p>Please wait...</p>"));
  server.sendContent(F("<div class='spinner'></div>"));  // Here we add the spinner
  server.sendContent(F("</div>"));
  server.sendContent(F("</body></html>"));

  server.sendContent(F("<style>"));
  server.sendContent(F(".spinner {"));
  server.sendContent(F("  border: 4px solid rgba(0, 0, 0, 0.1);"));
  server.sendContent(F("  border-left-color: #09f;"));
  server.sendContent(F("  border-radius: 50%;"));
  server.sendContent(F("  width: 10px;"));
  server.sendContent(F("  height: 10px;"));
  server.sendContent(F("  animation: spin 1s linear infinite;"));
  server.sendContent(F("}"));
  server.sendContent(F("@keyframes spin {"));
  server.sendContent(F("  to { transform: rotate(360deg); }"));
  server.sendContent(F("}"));
  server.sendContent(F("</style>"));

  server.sendContent(F("<script>"));
  server.sendContent(F("document.getElementById('updateForm').addEventListener('submit', function(event) {"));
  server.sendContent(F("  document.getElementById('pleaseWait').style.display = 'block';"));
  server.sendContent(F("});"));
  server.sendContent(F("</script>"));
  server.client().stop();
}

/**
 * @brief Handles the reboot request.
 */
void handleReboot() {
  // Root webpage
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/html", "");
  sendHTMLHeader(PageType::Reboot);
  server.sendContent(F("<div><h2>Rebooting the ESP8266, please wait..</h2></div>"));
  server.sendContent(F("</body></html>"));
  server.client().stop();
  delay(1000);
  ESP.restart();
}

/**
 * @brief Resets the EEPROM memory to default values.
 */
void resetMemory() {
  for (int i = 0; i < eepromSize; i++) {
    EEPROM.write(i, '\0');
  }
  EEPROM.commit();

  EEPROM.put(tcpServerPortAddress, 8000);
  EEPROM.commit();

  EEPROM.write(isFirstRunAddress, isFirstRunValue);
  EEPROM.commit();

  EEPROM.put(baudAddress, sysConfig.baud);
  EEPROM.commit();

  EEPROM.put(dbPowerAddress, sysConfig.dbPower);
  EEPROM.commit();
}

/**
 * @brief Handles the physical reset button logic.
 */
void handleReset() {
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

/**
 * @brief Handles the TCP Server functionality.
 */
void handleTcpServer() {

  // TCP Server mode
  WiFiClient newClient = ecmServer.available();  // Check for a new client

  if (newClient) {
    if (ecmClient && ecmClient.connected()) {
      newClient.stop();  // Reject the new connection
    } else {
      ecmClient = newClient;       // Accept new client
      ecmClient.setNoDelay(true);  // CRITICAL: Disables Nagle's algorithm to prevent chunked buffering
      startTime = millis();
    }
  }

  if (ecmClient && ecmClient.connected()) {

    // Step 1: Read from TCP Client (Browser) -> Send to Serial (GEM)
    while (ecmClient.available()) {
      // Use .read() instead of .readBytes() so it doesn't block for 50ms
      int bytesRead = ecmClient.read(sharedBuffer, sizeof(sharedBuffer));
      if (bytesRead > 0) {
        Serial.write(sharedBuffer, bytesRead);
        startTime = millis();
      }
      yield();
    }

    // Step 2: Read from Serial (GEM) -> Send to TCP Client (Browser)
    if (newData) {
      ecmClient.write(buffer, dataLength);
      ecmClient.flush();  // CRITICAL: Forces the ESP8266 to send the WiFi packet IMMEDIATELY
      startTime = millis();
    }

    // Step 3: Idle timeout
    if (millis() - startTime >= sysConfig.idleTime * 1000) {
      ecmClient.stop();
    }

    yield();
  }
}

/**
 * @brief Handles UDP packet detection for network discovery.
 */
void handleUdpDetect() {
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
            udpResponse = "{\"type\":\"SN: " + deviceData.serialNumber + " " + " esp8266-" + mqttClientID + "\", \"ip\":\"" + WiFi.localIP().toString() + "\"}";

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

/**
 * @brief Handles the TCP Client functionality.
 */
void handleTcpClient() {
  if (!tcpClient.connected()) {
    tcpClientConnect = "Not Connected";
  }

  // Step 1: Connect if needed
  if (!tcpClient.connected() && tcpIP.isSet() && sysConfig.tcpPort > 1024 && sysConfig.tcpPort < 65536 && newData) {
    //connectTries++;
    tcpClient.connect(tcpIP, sysConfig.tcpPort);
    tcpClient.setTimeout(250);
  }

  if (tcpClient.connected()) {
    tcpClientConnect = "Connected";
    // Step 2: Send new packet if flagged — don't wait here
    if (newData) {
      tcpClient.write(buffer, dataLength);  // Fire-and-forget
    }

    // Step 3: Wait for Apache to reply (Max 250ms)
    unsigned long waitStart = millis();
    while (tcpClient.connected() && !tcpClient.available() && (millis() - waitStart < 50)) {
      yield();
    }

    // Step 4: If response available, send to Serial
    if (tcpClient.available()) {
      sharedDataLength = tcpClient.read(sharedBuffer, sizeof(sharedBuffer));
      if (sharedDataLength > 0) {
        Serial.write(sharedBuffer, sharedDataLength);

        // Step 5: Wait briefly for Serial reply
        unsigned long serialWait = millis();
        while (!Serial.available() && millis() - serialWait < 50) {
          yield();
        }

        // Step 6: If Serial replied, send that back to TCP server
        if (Serial.available()) {
          sharedDataLength = Serial.readBytes(sharedBuffer, sizeof(sharedBuffer));
          if (sharedDataLength > 0) {
            tcpClient.write(sharedBuffer, sharedDataLength);
          }
        }
      }
    }

    // Step 7: We're done — close the client
    tcpClient.stop();
  }
}

/**
 * @brief Reads data from the serial port.
 */
void readSerialData() {
  if (Serial.available()) {
    dataLength = Serial.readBytes(buffer, sizeof(buffer));
    newData = true;
  }

  if (newData) {
    handlePacket();
  }
}


unsigned long previousMillis = 0;    // will store last time heap was printed
const unsigned long interval = 500;  // interval at which to print (milliseconds)


/**
 * @brief Standard Arduino loop function.
 */
void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    setupWiFi();
  }

  // Reset debounce
  if (resetFlag) {
    handleReset();
  }

  mqttClient.loop();

  readSerialData();

  server.handleClient();
  handleTcpServer();

  if (newData) {
    handleTcpClient();
    newData = false;
  }

  unsigned long currentMillis = millis();

  if (currentMillis - previousMillis >= interval) {
    MDNS.update();
    handleUdpDetect();
    previousMillis = currentMillis;
  }

  yield();
}

/**
 * @brief Analyzes the received packet and dispatches to specific handlers.
 */
void handlePacket() {
  if (dataLength < Packet::ECM_SIZE) return;

  if (buffer[0] != Packet::HEADER_0 || buffer[1] != Packet::HEADER_1) return;

  if (dataLength >= Packet::GEM_LARGE_SIZE && buffer[Packet::TYPE_OFFSET] == Packet::TYPE_GEM_LARGE && buffer[Packet::GEM_LARGE_SIZE - 2] == Packet::FOOTER_0 && buffer[Packet::GEM_LARGE_SIZE - 1] == Packet::FOOTER_1) {

    if (deviceType == DeviceType::Unknown) {
      deviceType = DeviceType::GEM;
    }

    gemPacketLarge();
  } else if (dataLength >= Packet::GEM_SIZE && buffer[Packet::TYPE_OFFSET] == Packet::TYPE_GEM && buffer[Packet::GEM_SIZE - 2] == Packet::FOOTER_0 && buffer[Packet::GEM_SIZE - 1] == Packet::FOOTER_1) {

    if (deviceType == DeviceType::Unknown) {
      deviceType = DeviceType::GEM;
    }

    gemPacket();
  } else if (buffer[Packet::TYPE_OFFSET] == Packet::TYPE_ECM && buffer[Packet::ECM_SIZE - 2] == Packet::FOOTER_0 && buffer[Packet::ECM_SIZE - 1] == Packet::FOOTER_1) {

    if (deviceType == DeviceType::Unknown) {
      deviceType = DeviceType::ECM;
    }

    ecmPacket();
  }
}

/**
 * @brief Processes an ECM packet.
 */
void ecmPacket() {
  memcpy(deviceData.prevWattSeconds, deviceData.wattSeconds, sizeof(deviceData.prevWattSeconds));
  memcpy(deviceData.prevPolWattSeconds, deviceData.polWattSeconds, sizeof(deviceData.prevPolWattSeconds));

  deviceData.prevSeconds = deviceData.seconds;

  // Extract the voltage value as an unsigned integer
  deviceData.voltage = static_cast<float>((buffer[Packet::ECM::VOLTAGE_HI] << 8) | buffer[Packet::ECM::VOLTAGE_LO]) / 10;
  deviceData.seconds = ((uint16_t)buffer[Packet::ECM::SECONDS_HI] << 16) | ((uint16_t)buffer[Packet::ECM::SECONDS_MD] << 8) | (uint16_t)buffer[Packet::ECM::SECONDS_LO];


  // Extract the 5/4-byte value as an unsigned integer
  deviceData.wattSeconds[0] = ((uint64_t)buffer[Packet::ECM::WATT_SECONDS_START + 4] << 32) | ((uint64_t)buffer[Packet::ECM::WATT_SECONDS_START + 3] << 24) | ((uint64_t)buffer[Packet::ECM::WATT_SECONDS_START + 2] << 16) | ((uint64_t)buffer[Packet::ECM::WATT_SECONDS_START + 1] << 8) | (uint64_t)buffer[Packet::ECM::WATT_SECONDS_START];
  deviceData.wattSeconds[1] = ((uint64_t)buffer[Packet::ECM::WATT_SECONDS_START + 9] << 32) | ((uint64_t)buffer[Packet::ECM::WATT_SECONDS_START + 8] << 24) | ((uint64_t)buffer[Packet::ECM::WATT_SECONDS_START + 7] << 16) | ((uint64_t)buffer[Packet::ECM::WATT_SECONDS_START + 6] << 8) | (uint64_t)buffer[Packet::ECM::WATT_SECONDS_START + 5];
  deviceData.wattSeconds[2] = ((uint64_t)buffer[43] << 24) | ((uint64_t)buffer[42] << 16) | ((uint64_t)buffer[41] << 8) | (uint64_t)buffer[40];
  deviceData.wattSeconds[3] = ((uint64_t)buffer[47] << 24) | ((uint64_t)buffer[46] << 16) | ((uint64_t)buffer[45] << 8) | (uint64_t)buffer[44];
  deviceData.wattSeconds[4] = ((uint64_t)buffer[51] << 24) | ((uint64_t)buffer[50] << 16) | ((uint64_t)buffer[49] << 8) | (uint64_t)buffer[48];
  deviceData.wattSeconds[5] = ((uint64_t)buffer[55] << 24) | ((uint64_t)buffer[54] << 16) | ((uint64_t)buffer[53] << 8) | (uint64_t)buffer[52];
  deviceData.wattSeconds[6] = ((uint64_t)buffer[59] << 24) | ((uint64_t)buffer[58] << 16) | ((uint64_t)buffer[57] << 8) | (uint64_t)buffer[56];
  deviceData.dcVoltage = ((uint64_t)buffer[Packet::ECM::DC_VOLTAGE_HI] << 8) | (uint64_t)buffer[Packet::ECM::DC_VOLTAGE_LO];

  deviceData.polWattSeconds[0] = ((uint64_t)buffer[Packet::ECM::POL_WATT_SECONDS_START + 4] << 32) | ((uint64_t)buffer[Packet::ECM::POL_WATT_SECONDS_START + 3] << 24) | ((uint64_t)buffer[Packet::ECM::POL_WATT_SECONDS_START + 2] << 16) | ((uint64_t)buffer[Packet::ECM::POL_WATT_SECONDS_START + 1] << 8) | (uint64_t)buffer[Packet::ECM::POL_WATT_SECONDS_START];
  deviceData.polWattSeconds[1] = ((uint64_t)buffer[Packet::ECM::POL_WATT_SECONDS_START + 9] << 32) | ((uint64_t)buffer[Packet::ECM::POL_WATT_SECONDS_START + 8] << 24) | ((uint64_t)buffer[Packet::ECM::POL_WATT_SECONDS_START + 7] << 16) | ((uint64_t)buffer[Packet::ECM::POL_WATT_SECONDS_START + 6] << 8) | (uint64_t)buffer[Packet::ECM::POL_WATT_SECONDS_START + 5];

  String serialEnd = String(((uint16_t)buffer[Packet::ECM::SERIAL_HI] << 8) | (uint16_t)buffer[Packet::ECM::SERIAL_LO]);

  // Add leading zeros if necessary to make it 5 characters long
  while (serialEnd.length() < 5) {
    serialEnd = "0" + serialEnd;
  }


  deviceData.serialNumber = String((uint16_t)buffer[Packet::ECM::SERIAL_TYPE]) + serialEnd;

  if (deviceData.prevSeconds != 0) {
    processPacket();
  }
}

/**
 * @brief Generic function to process GEM packets.
 */
template<typename T>
void processGemPacketGeneric() {
  memcpy(deviceData.prevWattSeconds, deviceData.wattSeconds, sizeof(deviceData.prevWattSeconds));
  memcpy(deviceData.prevPolWattSeconds, deviceData.polWattSeconds, sizeof(deviceData.prevPolWattSeconds));

  deviceData.prevSeconds = deviceData.seconds;

  // Extract the voltage value as an unsigned integer
  deviceData.voltage = static_cast<double>(((uint16_t)(uint8_t)buffer[T::VOLTAGE_HI] << 8) | (uint16_t)(uint8_t)buffer[T::VOLTAGE_LO]) / 10.0;
  
  deviceData.seconds = 
    ((uint32_t)(uint8_t)buffer[T::SECONDS_HI] << 16) | 
    ((uint32_t)(uint8_t)buffer[T::SECONDS_MD] << 8)  | 
    (uint32_t)(uint8_t)buffer[T::SECONDS_LO];

  for (int channelIndex = 0; channelIndex < 32; channelIndex++) {
    deviceData.wattSeconds[channelIndex] =
      ((uint64_t)(uint8_t)buffer[(channelIndex * 5) + T::WATT_SECONDS_START + 4] << 32) | ((uint64_t)(uint8_t)buffer[(channelIndex * 5) + T::WATT_SECONDS_START + 3] << 24) | ((uint64_t)(uint8_t)buffer[(channelIndex * 5) + T::WATT_SECONDS_START + 2] << 16) | ((uint64_t)(uint8_t)buffer[(channelIndex * 5) + T::WATT_SECONDS_START + 1] << 8) | (uint64_t)(uint8_t)buffer[(channelIndex * 5) + T::WATT_SECONDS_START];

    deviceData.polWattSeconds[channelIndex] =
      ((uint64_t)(uint8_t)buffer[(channelIndex * 5) + T::POL_WATT_SECONDS_START + 4] << 32) | ((uint64_t)(uint8_t)buffer[(channelIndex * 5) + T::POL_WATT_SECONDS_START + 3] << 24) | ((uint64_t)(uint8_t)buffer[(channelIndex * 5) + T::POL_WATT_SECONDS_START + 2] << 16) | ((uint64_t)(uint8_t)buffer[(channelIndex * 5) + T::POL_WATT_SECONDS_START + 1] << 8) | (uint64_t)(uint8_t)buffer[(channelIndex * 5) + T::POL_WATT_SECONDS_START];

    deviceData.amps[channelIndex] = static_cast<float>(
                                      ((uint16_t)(uint8_t)buffer[(channelIndex * 2) + T::AMPS_HI_START] << 8) | (uint16_t)(uint8_t)buffer[(channelIndex * 2) + T::AMPS_LO_START])
                                    / 50.0;

    if (channelIndex < 8) {
      deviceData.temp[channelIndex] = tempConv(
                                        (uint8_t)buffer[(channelIndex * 2) + T::TEMP_HI_START],
                                        (uint8_t)buffer[(channelIndex * 2) + T::TEMP_LO_START])
                                      / 2.0;
      if (channelIndex < 4) {
        deviceData.pulse[channelIndex] = ((uint64_t)(uint8_t)buffer[(channelIndex * 3) + T::PULSE_START + 2] << 16) | ((uint64_t)(uint8_t)buffer[(channelIndex * 3) + T::PULSE_START + 1] << 8) | (uint64_t)(uint8_t)buffer[(channelIndex * 3) + T::PULSE_START];
      }
    }
  }

  String serial = String(((buffer[T::SERIAL_HI] << 8) | buffer[T::SERIAL_LO]));
  String id = String((uint16_t)buffer[T::ID]);

  while (id.length() < 3) {
    id = "0" + id;
  }

  while (serial.length() < 5) {
    serial = "0" + serial;
  }


  deviceData.serialNumber = id + serial;

  if (deviceData.prevSeconds != 0) {
    processPacket();
  }
}

/**
 * @brief Processes a GEM packet.
 */
void gemPacket() {
  processGemPacketGeneric<Packet::GEM>();
}

/**
 * @brief Processes a large GEM packet.
 */
void gemPacketLarge() {
  processGemPacketGeneric<Packet::GEMLarge>();
}

/**
 * @brief Converts raw temperature data to float.
 * 
 * @param hi High byte.
 * @param lo Low byte.
 * @return float Temperature value.
 */
float tempConv(uint16_t highByte, uint16_t lowByte) {
  const uint16_t specialBit = (highByte & 0x02) >> 1;
  if (specialBit == 1) {
    return 8192.0f;
  }

  const uint16_t value = ((highByte & 0x01) << 8) | (lowByte & 0xFF);
  const bool isNegative = (highByte & 0x80) != 0;

  return isNegative ? -static_cast<float>(value) : static_cast<float>(value);
}

/**
 * @brief Processes the parsed packet data.
 * 
 * Calculates watts, kWh, and handles counter overflows.
 */
void processPacket() {
  uint16_t secDiff = 0;

  // Calculate time difference handling overflow (16-bit counter)
  if (deviceData.prevSeconds > deviceData.seconds) {
    secDiff = (deviceData.seconds + WS_OVERFLOW[3]) - deviceData.prevSeconds;
  } else {
    secDiff = deviceData.seconds - deviceData.prevSeconds;
  }

  uint64_t wattSecDiff = 0;
  uint64_t polWattSecDiff = 0;
  int deltaPolWs = 0;

  uint8_t numChan = 7;
  uint8_t wsMulti = 5;

  if (deviceType == DeviceType::GEM) {
    numChan = 32;
  }

  if (secDiff != 0) {
    for (int channelIndex = 0; channelIndex < numChan; channelIndex++) {

      // ECM-1240 Channel 1 & 2 are 32-bit (index 4), others are 40-bit (index 5)
      if (channelIndex == 2 && deviceType == DeviceType::ECM) {
        wsMulti = 4;
      }

      // Calculate delta WattSeconds handling overflow
      if (deviceData.prevWattSeconds[channelIndex] > deviceData.wattSeconds[channelIndex]) {
        deviceData.deltaWattSeconds[channelIndex] = (deviceData.wattSeconds[channelIndex] + WS_OVERFLOW[wsMulti] - deviceData.prevWattSeconds[channelIndex]);
      } else {
        deviceData.deltaWattSeconds[channelIndex] = (deviceData.wattSeconds[channelIndex] - deviceData.prevWattSeconds[channelIndex]);
      }

      if (channelIndex < 2 || deviceType == DeviceType::GEM) {
        // Calculate Polarized WattSeconds (Net Metering)
        if (deviceData.prevPolWattSeconds[channelIndex] > deviceData.polWattSeconds[channelIndex]) {
          polWattSecDiff = (deviceData.polWattSeconds[channelIndex] + WS_OVERFLOW[wsMulti] - deviceData.prevPolWattSeconds[channelIndex]);
        } else {
          polWattSecDiff = (deviceData.polWattSeconds[channelIndex] - deviceData.prevPolWattSeconds[channelIndex]);
        }

        // Net Watts calculation: Abs(WS) - 2 * Polarized(WS)
        deltaPolWs = deviceData.deltaWattSeconds[channelIndex] - (2 * polWattSecDiff);

        deviceData.netWatts[channelIndex] = static_cast<float>(deltaPolWs) / secDiff;
        deviceData.netKwh[channelIndex] = static_cast<float>(deltaPolWs) / 3600000;
        deviceData.totalNetKwh[channelIndex] += deviceData.netKwh[channelIndex];
      }

      // Calculate Average Watts over the interval
      deviceData.watts[channelIndex] = deviceData.deltaWattSeconds[channelIndex] / secDiff;
      // Calculate kWh
      deviceData.kwh[channelIndex] = static_cast<float>(deviceData.deltaWattSeconds[channelIndex]) / 3600000;
      deviceData.totalKwh[channelIndex] += deviceData.kwh[channelIndex];
    }

    if (mqttPort != 0) {
      mqttPost();
    }
  }
}


/**
 * @brief Sends serial debug information to the web client.
 */
void serialDebug() {
  server.sendContent(F("<h3>Serial Debug:</h3><br><b>Buffer Length:</b> "));
  server.sendContent(String(dataLength) + F("<br><br><b>Packet:</b><br><br>"));

  char temp[3];  // Buffer to hold the formatted byte
  for (int i = 0; i < dataLength; i++) {
    sprintf(temp, "%02X", buffer[i]);
    server.sendContent(String(temp) + " ");
  }

  server.sendContent("");
  server.client().stop();
}

/**
 * @brief Helper to publish a value to a specific subtopic.
 */
void mqttPublish(const char* deviceName, const char* subtopic, const char* value) {
  char topic[64];
  snprintf(topic, sizeof(topic), "%s-%s/%s", deviceName, deviceData.serialNumber.c_str(), subtopic);
  mqttClient.publish(topic, value, false);
  yield();
}

/**
 * @brief Publishes data to MQTT.
 */
void mqttPost() {
  if (WiFi.status() != WL_CONNECTED || !mqttServer.isSet()) {
    mqttStatus = "WiFi not connected or MQTT IP not set.";
    return;
  }

  if (!mqttClient.connected()) {
    mqttClient.setSocketTimeout(5);
    mqttClient.setServer(mqttServer, mqttPort);

    if (!mqttClient.connect(mqttClientID, mqttUser, mqttPass)) {
      mqttStatus = "MQTT Connection failed.";
      return;
    }
  }

  const char* deviceName = (deviceType == DeviceType::GEM) ? "GEM" : "ECM1240";
  const uint8_t numChan = (deviceType == DeviceType::GEM) ? 32 : 7;

  char payload[64];
  char subtopic[32];


  // Publish Voltage
  snprintf(payload, sizeof(payload), "%.2f", deviceData.voltage);
  mqttPublish(deviceName, "v", payload);

  // Loop through channels and publish data
  for (uint8_t i = 0; i < numChan; i++) {
    snprintf(payload, sizeof(payload), "%u", deviceData.watts[i]);
    snprintf(subtopic, sizeof(subtopic), "c%u/watt", i + 1);
    mqttPublish(deviceName, subtopic, payload);

    snprintf(payload, sizeof(payload), "%u", deviceData.netWatts[i]);
    snprintf(subtopic, sizeof(subtopic), "c%u/net_watt", i + 1);
    mqttPublish(deviceName, subtopic, payload);

    dtostrf(deviceData.kwh[i], 1, 5, payload);
    snprintf(subtopic, sizeof(subtopic), "c%u/kwh", i + 1);
    mqttPublish(deviceName, subtopic, payload);

    dtostrf(deviceData.netKwh[i], 1, 5, payload);
    snprintf(subtopic, sizeof(subtopic), "c%u/net_kwh", i + 1);
    mqttPublish(deviceName, subtopic, payload);

    dtostrf(deviceData.totalKwh[i], 1, 5, payload);
    snprintf(subtopic, sizeof(subtopic), "c%u/total_kwh", i + 1);
    mqttPublish(deviceName, subtopic, payload);

    dtostrf(deviceData.totalNetKwh[i], 1, 5, payload);
    snprintf(subtopic, sizeof(subtopic), "c%u/total_net_kwh", i + 1);
    mqttPublish(deviceName, subtopic, payload);

    snprintf(payload, sizeof(payload), "%llu", deviceData.wattSeconds[i]);
    snprintf(subtopic, sizeof(subtopic), "c%u/ws", i + 1);
    mqttPublish(deviceName, subtopic, payload);

    snprintf(payload, sizeof(payload), "%llu", deviceData.polWattSeconds[i]);
    snprintf(subtopic, sizeof(subtopic), "c%u/pws", i + 1);
    mqttPublish(deviceName, subtopic, payload);

    snprintf(payload, sizeof(payload), "%lu", deviceData.deltaWattSeconds[i]);
    snprintf(subtopic, sizeof(subtopic), "c%u/dws", i + 1);
    mqttPublish(deviceName, subtopic, payload);

    if (deviceType == DeviceType::GEM) {
      snprintf(payload, sizeof(payload), "%.2f", deviceData.amps[i]);
      snprintf(subtopic, sizeof(subtopic), "c%u/amp", i + 1);
      mqttPublish(deviceName, subtopic, payload);

      if (i < 8) {
        snprintf(payload, sizeof(payload), "%.2f", deviceData.temp[i]);
        snprintf(subtopic, sizeof(subtopic), "t%u/value", i + 1);
        mqttPublish(deviceName, subtopic, payload);

        if (i < 4) {
          snprintf(payload, sizeof(payload), "%llu", deviceData.pulse[i]);
          snprintf(subtopic, sizeof(subtopic), "p%u/value", i + 1);
          mqttPublish(deviceName, subtopic, payload);
        }
      }
    }
  }

  mqttStatus = "MQTT Ran.";
}


/**
 * @brief Helper to send HA discovery config.
 */
void sendHAConfig(const String& uniqueIdSuffix, const String& nameSuffix, const String& stateTopic, const String& unit, const String& deviceClass, const String& stateClass, const String& configTopicPart) {
  String payload = "{\"unique_id\": \"" + deviceData.serialNumber + uniqueIdSuffix + "\", \"name\":\"" + deviceName + "-" + deviceData.serialNumber + " " + nameSuffix + "\",\"state_topic\":\"" + deviceName + "-" + deviceData.serialNumber + "/" + stateTopic + "\",\"unit_of_measurement\":\"" + unit + "\"";

  if (deviceClass.length() > 0) {
    payload += ", \"device_class\": \"" + deviceClass + "\"";
  }

  if (stateClass.length() > 0) {
    payload += ", \"state_class\": \"" + stateClass + "\"";
    if (stateClass == "total_increasing") {
      payload += ", \"last_reset\": \"1970-01-01T00:00:00+00:00\"";
    }
  }

  payload += ", \"dev\":{\"ids\":\"" + deviceData.serialNumber + "\",\"name\":\"" + deviceName + "-" + deviceData.serialNumber + "\",\"sw\":\"esp8266-custom\",\"mdl\":\"" + deviceName + "\",\"mf\":\"BrulTech Research Inc.\"}}";

  String topic = "homeassistant/sensor/" + deviceName + "-" + deviceData.serialNumber + "/" + configTopicPart + "/config";

  if (!mqttClient.publish(topic.c_str(), payload.c_str(), true)) {
    server.sendContent(F("Not Sent:<br><br>"));
  }
  server.sendContent(payload + "<br><br>" + topic + "<br><br>");
}

/**
 * @brief Handles Home Assistant configuration request.
 * 
 * Generates MQTT Discovery payloads for Home Assistant.
 */
void handleHA() {
  uint8_t numChan = 7;

  if (deviceType == DeviceType::GEM) {
    deviceName = "GEM";
    numChan = 32;
  } else {
    deviceName = "ECM1240";
  }
  server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
  server.sendHeader("Pragma", "no-cache");
  server.sendHeader("Expires", "-1");
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  // here begin chunked transfer
  server.send(200, "text/html", "");
  sendHTMLHeader(PageType::Main);

  if (WiFi.status() == WL_CONNECTED && mqttServer.isSet()) {
    mqttClient.setSocketTimeout(5);
    mqttClient.setServer(mqttServer, mqttPort);

    if (!mqttClient.connect(mqttClientID, mqttUser, mqttPass)) {
      server.sendContent(F("<div><h3>MQTT couldn't connect, please check your settings.</h3></div>"));
      server.sendContent(F("</body></html>"));
      server.sendContent(F(""));
      server.client().stop();
      return;
    }

    server.sendContent(F("<div><h3>MQTT Values Sent:</h3>"));

    sendHAConfig("v", "Volts", "v", "V", "", "measurement", "volts");

    for (int x = 0; x < numChan; x++) {

      // if ecm aux 5 is pulse or gas
      if (deviceType == DeviceType::ECM && x == 6 && mqttData.pulseTypes[0] != "energy") {
        sendHAConfig("p" + String(x + 1) + "_value", "P" + String(x + 1) + " Value", "p" + String(x + 1) + "/value", mqttData.pulseUnits[0], mqttData.pulseTypes[0], "measurement", "p" + String(x + 1) + "_value");
      } else {
        sendHAConfig("ch" + String(x + 1) + "kwh", "CH" + String(x + 1) + " kWh", "c" + String(x + 1) + "/kwh", "kWh", "", "measurement", "ch" + String(x + 1) + "_kwh");
        sendHAConfig("ch" + String(x + 1) + "_total_kwh", "CH" + String(x + 1) + " Total kWh", "c" + String(x + 1) + "/total_kwh", "kWh", "energy", "total_increasing", "ch" + String(x + 1) + "_total_kwh");
        sendHAConfig("ch" + String(x + 1) + "ws", "CH" + String(x + 1) + " WattSeconds", "c" + String(x + 1) + "/dws", "WS", "", "measurement", "ch" + String(x + 1) + "_ws");
        sendHAConfig("ch" + String(x + 1) + "w", "CH" + String(x + 1) + " Watts", "c" + String(x + 1) + "/watt", "W", "", "measurement", "ch" + String(x + 1) + "_watts");

        if (deviceType == DeviceType::GEM) {
          sendHAConfig("ch" + String(x + 1) + "a", "CH" + String(x + 1) + " Amps", "c" + String(x + 1) + "/amp", "A", "", "measurement", "ch" + String(x + 1) + "_amps");

          if (x < 8) {
            sendHAConfig("t" + String(x + 1) + "_value", "T" + String(x + 1) + " Value", "t" + String(x + 1) + "/value", "C", "", "measurement", "t" + String(x + 1) + "_value");

            if (x < 4) {
              sendHAConfig("p" + String(x + 1) + "_value", "P" + String(x + 1) + " Value", "p" + String(x + 1) + "/value", mqttData.pulseUnits[x], mqttData.pulseTypes[x], "total_increasing", "p" + String(x + 1) + "_value");
            }
          }
        }
      }
    }

    // Publish the auto-discovery payload to the MQTT broker

    mqttClient.disconnect();
    server.sendContent(F("</div></body></html>"));
  } else {
    server.sendContent(F("<div><h3>MQTT couldn't connect, please check your settings.</h3></div></html></body>"));
  }
  server.sendContent("");
  server.client().stop();
}

/**
 * @brief Handles the Access Point main page.
 */
void handleAP() {
  if (!isAuthenticated()) {
    sendLogin(false);
  } else if (WiFi.status() != WL_CONNECTED) {
    // Root webpage
    scanNetworks();

    server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    server.send(200, "text/html", "");
    sendHTMLHeader(PageType::Login);

    server.sendContent(F("<div><h2>Network Configuration</h2>"));
    server.sendContent(F("<form action='/config'>"));
    server.sendContent(F("<label>Select a network:</label> <select id='ssid' name='ssid'>"));
    if (networkOptions.length() > 0) {
      server.sendContent(networkOptions);
    }
    server.sendContent(F("</select><br>"));
    server.sendContent(F("<label>Or enter SSID:</label> <input id='custom_ssid' class='full' type='text' name='custom_ssid'><br>"));
    server.sendContent(F("<label>Password:</label> <input class='full' maxlength='64' type='password' name='password' value=''><br>"));
    server.sendContent(F("<button id='saveLocal' class='button'>Connect to Network</button>"));
    server.sendContent(F("</form></div><div>Local address will be copied to clipboard upon clicking connect.<br><br> <a id='localLink' href='http://"));
    server.sendContent(localAddress);
    server.sendContent(F(".local/'>http://"));
    server.sendContent(localAddress);
    server.sendContent(F(".local/</a></div>"));
    server.sendContent(F("<div>Click below to access the configuration page.<br><br> <a href='http://192.168.4.1/main'>Configuration Page</a></div>"));
    server.sendContent(F("</body></html>"));
    server.client().stop();
  }
}

/**
 * @brief Handles the network configuration submission.
 */
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
  sysConfig.dbPower = server.arg("db_power").toInt();

  ssidValue.toCharArray(sysConfig.ssid, 32);
  passwordValue.toCharArray(sysConfig.password, 64);

  EEPROM.put(ssidAddress, sysConfig.ssid);
  EEPROM.put(newPasswordAddress, sysConfig.password);
  EEPROM.put(dbPowerAddress, sysConfig.dbPower);

  EEPROM.commit();
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/html", "");
  sendHTMLHeader(PageType::ConfigSaved);

  server.sendContent(F("<div><h3>Network configuration saved, please connect back to your network. The ESP-XBEE module will have a solid green LED once connected. <br><br>  Paste the copied .local address into your address bar after to try to connect.<br><br></h3></div></body></html>"));
  server.client().stop();
  delay(250);

  // Reboot module
  ESP.restart();
}

/**
 * @brief Attempts to detect the connected device type (ECM or GEM).
 */
void getDeviceSettings() {
  Serial.flush();
  deviceType = DeviceType::Unknown;

  if (sysConfig.baud == 19200 && tryECM()) {
    deviceType = DeviceType::ECM;
    processECMSettings();
    return;
  }

  if (tryGEM()) {
    deviceType = DeviceType::GEM;
  }
}

/**
 * @brief Tries to communicate with an ECM device.
 * 
 * @return true If ECM detected.
 * @return false If ECM not detected.
 */
bool tryECM() {
  Serial.write(0xFC);
  delay(50);
  Serial.write("SET");
  delay(50);
  Serial.write("RCV");
  delay(50);

  if (!Serial.available()) return false;

  settingsLength = Serial.readBytes(settingsBuffer, sizeof(settingsBuffer));
  return settingsLength > 32;  // valid ECM response
}

/**
 * @brief Tries to communicate with a GEM device.
 * 
 * @return true If GEM detected.
 * @return false If GEM not detected.
 */
bool tryGEM() {
  Serial.flush();
  Serial.write("^^^RQSSRN\r\n");
  delay(100);

  if (!Serial.available()) return false;

  gemSerial = "";
  while (Serial.available()) {
    gemSerial += (char)Serial.read();
  }
  return true;
}

/**
 * @brief Tries to detect device settings by toggling baud rate.
 */
void getDeviceSettingsChangeBaud() {
  if (sysConfig.baud == 19200) {
    sysConfig.baud = 115200;
  } else {
    sysConfig.baud = 19200;
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
      deviceType = DeviceType::ECM;
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

      deviceType = DeviceType::GEM;
    }
  }
}

/**
 * @brief Parses ECM settings from the received buffer.
 */
void processECMSettings() {
  bool process = false;
  int bufferIndex = 0;

  for (bufferIndex = 0; bufferIndex < settingsLength; bufferIndex++) {
    if (bufferIndex + 1 < settingsLength) {
      if (settingsBuffer[bufferIndex] == 0xFC && settingsBuffer[bufferIndex + 1] == 0x54) {
        process = true;
        break;
      }
    }
  }

  bufferIndex = bufferIndex + 3;

  // Validate the first three bytes and last 2 bytes
  if (process) {
    // Extract the voltage value as an unsigned integer
    ecmSettings.gotSettings = true;

    ecmSettings.ch1Set[0] = (uint8_t)settingsBuffer[bufferIndex++];
    //debugText += " " + String(bufferIndex);
    ecmSettings.ch1Set[1] = (uint8_t)settingsBuffer[bufferIndex++];
    //debugText += " " + String(bufferIndex);

    ecmSettings.ch2Set[0] = (uint8_t)settingsBuffer[bufferIndex++];
    //debugText += " " + String(bufferIndex);
    ecmSettings.ch2Set[1] = (uint8_t)settingsBuffer[bufferIndex++];
    //debugText += " " + String(bufferIndex);

    ecmSettings.ptSet[0] = (uint8_t)settingsBuffer[bufferIndex++];
    //debugText += " " + String(bufferIndex);
    ecmSettings.ptSet[1] = (uint8_t)settingsBuffer[bufferIndex++];
    //debugText += " " + String(bufferIndex);

    ecmSettings.sendInterval = (uint8_t)settingsBuffer[bufferIndex++];
    //debugText += " " + String(bufferIndex);
    bufferIndex++;

    ecmSettings.firmwareVersion = static_cast<double>((settingsBuffer[bufferIndex] << 8) | settingsBuffer[bufferIndex + 1]) / 1000;
    //debugText += " " + String(bufferIndex);

    bufferIndex = bufferIndex + 2;

    String serialEnd = String(((settingsBuffer[bufferIndex + 1] << 8) | settingsBuffer[bufferIndex + 2]));

    while (serialEnd.length() < 5) {
      serialEnd = "0" + serialEnd;
    }


    ecmSettings.serialNumber = String((uint16_t)settingsBuffer[bufferIndex]) + serialEnd;
    //debugText += " " + String(bufferIndex);

    bufferIndex = bufferIndex + 4;

    //debugText += " " + String(bufferIndex);
    int bitIndex = 0;
    for (bitIndex; bitIndex < 5; bitIndex++) {
      ecmSettings.auxX2[bitIndex] = (settingsBuffer[bufferIndex] & (1 << bitIndex)) != 0;
    }

    if ((settingsBuffer[bufferIndex] & (1 << bitIndex)) != 0) {
      ecmSettings.aux5Option = 1;
    } else if ((settingsBuffer[bufferIndex] & (1 << ++bitIndex)) != 0) {
      ecmSettings.aux5Option = 3;
    } else {
      ecmSettings.aux5Option = 0;
    }
  }
}

/**
 * @brief Handles the scan networks request.
 */
void handleScan() {
  scanNetworks();
  server.send(200, "text/html", networkOptions);
}

/**
 * @brief Scans for available WiFi networks and populates the options string.
 */
void scanNetworks() {
  int networksFound = WiFi.scanNetworks();
  networkOptions = "";
  networkOptions.reserve(2048);
  bool selected = false;
  for (int i = 0; i < networksFound; i++) {
    networkOptions += F("<option value='") + escapeHtml(WiFi.SSID(i)) + "'";
    if (!selected) {
      if (String(sysConfig.ssid).equals(WiFi.SSID(i))) {
        networkOptions += F(" selected='selected'");
        selected = true;
      }
    }
    networkOptions += F("'>") + escapeHtml(WiFi.SSID(i)) + F(" <b>RSSI:</b> ") + WiFi.RSSI(i) + F("</option>");
  }
}

/**
 * @brief Sends the HTML header content (Logo/Title).
 */
void sendHeaderContent() {
  server.sendContent(F("<html><head><title>Brultech Config</title></head><body>"));
  server.sendContent(F("<div><h3>Brultech Config "));
  server.sendContent(FW_VERSION);
  server.sendContent(F("</h3><br><a href='http://"));
  server.sendContent(localAddress);
  server.sendContent(F(".local/'>http://"));
  server.sendContent(localAddress);
  server.sendContent(F(".local/</a></div>"));
}

/**
 * @brief Sends the navigation menu.
 */
void sendMenu() {
  server.sendContent(F("<div id='network'><form action='/config'><h3>Menu</h3>"));
  server.sendContent(F("<a href='#network' class='button'>Network</a>"));
  server.sendContent(F("<a href='#baud' class='button'>Baud</a>"));
  server.sendContent(F("<a href='#client' class='button'>TCP Client</a>"));
  server.sendContent(F("<a href='#server' class='button'>TCP Server</a>"));
  server.sendContent(F("<a href='#mqtt' class='button'>MQTT</a>"));
  server.sendContent(F("<a href='#login' class='button'>Login</a>"));
  server.sendContent(F("<a href='#settings' class='button'>Device Settings</a>"));
  server.sendContent(F("<a href='#data' class='button'>Data</a>"));
  server.sendContent(F("<a href='#fw' class='button'>ESP Firmware</a>"));
  server.sendContent(F("<a href='#serialDebug' class='button'>Debug</a></div>"));
}

/**
 * @brief Sends the Network Settings form.
 */
void sendNetworkSettings() {
  server.sendContent(F("<div id='network'><form action='/config'><h3>Network Settings</h3>"));
  server.sendContent(F("<h4 style='color:#4CAF50;'>Connected to: "));
  if (WiFi.SSID().length() > 0) {
    server.sendContent(escapeHtml(WiFi.SSID()));
  }
  server.sendContent(F(" <br><br>RSSI: "));
  server.sendContent(String(WiFi.RSSI()));
  server.sendContent(F("</h4>"));
  server.sendContent(F("<label>Select a new network:</label> <select id='ssid' name='ssid'>"));
  if (networkOptions.length() > 0) {
    server.sendContent(networkOptions);
  }
  server.sendContent(F("</select><button class='button' id='scan'>Scan</button>"));
  server.sendContent(F("<label>or enter the SSID:</label><input id='custom_ssid' class='full' maxlength='20' type='text' name='custom_ssid' value='"));
  if (strlen(sysConfig.ssid) > 0) {
    server.sendContent(escapeHtml(String(sysConfig.ssid)));
  }
  server.sendContent(F("'>"));
  server.sendContent(F("<label>Password:</label><input class='full' maxlength='64' type='password' name='password' value='"));
  if (strlen(sysConfig.password) > 0) {
    server.sendContent(escapeHtml(String(sysConfig.password)));
  }
  server.sendContent(F("'>"));
  server.sendContent(F("<label>dB Value:</label><input class='full' maxlength='64' type='text' name='db_power' value='"));
  server.sendContent(String(sysConfig.dbPower));
  server.sendContent(F("'>"));
  server.sendContent(F("<button class='button'>Submit</button></form></div>"));
}

/**
 * @brief Sends the IP Address Settings form.
 */
void sendIPSettings() {
  server.sendContent(F("<div><form action='/ip-config'><h3>IP Address Settings</h3>"));
  server.sendContent(F("<label>Type:</label>DHCP: <input name='type' type='radio' "));
  if (!storedIPConfig.isConfigured) {
    server.sendContent(F("checked='checked'"));
  }
  server.sendContent(F(" value='0'>"));
  server.sendContent(F("  Static: <input name='type' type='radio' "));
  if (storedIPConfig.isConfigured) {
    server.sendContent(F("checked='checked'"));
  }
  server.sendContent(F(" value='1'>"));
  server.sendContent(F("<label>IP address:</label><input class='full' type='text' name='ip' value='"));
  server.sendContent(WiFi.localIP().toString());
  server.sendContent(F("'>"));
  server.sendContent(F("<label>Subnet:</label><input class='full' type='text' name='subnet' value='"));
  server.sendContent(WiFi.subnetMask().toString());
  server.sendContent(F("'>"));
  server.sendContent(F("<label>Gateway:</label><input class='full' type='text' name='gateway' value='"));
  server.sendContent(WiFi.gatewayIP().toString());
  server.sendContent(F("'>"));
  server.sendContent(F("<label>DNS</label><input class='full' type='text' name='dns' value='"));
  server.sendContent(WiFi.dnsIP(0).toString());
  server.sendContent(F("'>"));
  server.sendContent(F("<button class='button'>Save Settings</button></form></div>"));
}

/**
 * @brief Sends the Baud Rate Settings form.
 */
void sendBaudSettings() {
  server.sendContent(F("<div id='baud'><form action='/baud'><h3>Change Baud Rate</h3>"));
  server.sendContent(F("19200: <input type='radio' name='baud' value='19200'"));
  if (sysConfig.baud == 19200) {
    server.sendContent(F(" checked='checked'"));
  }
  server.sendContent(F(">"));
  server.sendContent(F(" 115200: <input type='radio' name='baud' value='115200'"));
  if (sysConfig.baud == 115200) {
    server.sendContent(F(" checked='checked'"));
  }
  server.sendContent(F(">"));
  server.sendContent(F("<br><button class='button'>Save Baudrate</button></form></div>"));
  server.sendContent(F("</form></div>"));
}

/**
 * @brief Sends the Time Server Settings form.
 */
void sendTimeServerSettings() {
  server.sendContent(F("<div id='client'><form action='/ntp-server'><h3>Time Server</h3>"));
  server.sendContent(F("<label>UTC Time:</label>"));
  server.sendContent(String(getFormattedDate()));
  server.sendContent(F("<br>"));
  server.sendContent(F("<label>NTP Server:</label><input class='full' type='text' name='ntp_server' value='"));
  if (strlen(sysConfig.ntpServer) > 0) {
    server.sendContent(String(sysConfig.ntpServer));
  }
  server.sendContent(F("'>"));
  server.sendContent(F("<button class='button'>Change Server</button>"));
  server.sendContent(F("</form></div>"));
}

/**
 * @brief Sends the Serial to TCP Client Settings form.
 */
void sendSerialToTCPSettings() {
  server.sendContent(F("<div id='client'><form action='/serial-to-tcp'><h3>Serial to TCP Client</h3>"));
  server.sendContent(F("<label>IP address:</label><input class='full' type='text' name='ip' value='"));
  server.sendContent(tcpIP.toString());
  server.sendContent(F("'>"));
  server.sendContent(F("<label>Port:</label><input class='full' type='number' name='port' value='"));
  server.sendContent(String(sysConfig.tcpPort));
  server.sendContent(F("'>"));
  server.sendContent(F("<button class='button'>Connect</button>"));
  server.sendContent(F("</form></div>"));
}

/**
 * @brief Sends the TCP Server Settings form.
 */
void sendTCPServerSettings() {
  server.sendContent(F("<div id='server'><form action='/serial-to-tcp-server'><h3>TCP Server Connection</h3>"));
  if (ecmClient.connected()) {
    server.sendContent(F("<label>Connected:</label> Connected "));
  } else {
    server.sendContent(F("<label>Connected:</label> Not Connected"));
  }
  server.sendContent(F("<label>Disconnect Time (no activity):</label>Time (in seconds): <input type='number' min='1' max='6000' name='idle_time' value='"));
  server.sendContent(String(sysConfig.idleTime));
  server.sendContent(F("'>"));
  server.sendContent(F("<label>Port:</label><input class='full' type='number' name='port' value='"));
  server.sendContent(String(sysConfig.tcpServerPort));
  server.sendContent(F("'>"));
  server.sendContent(F("<button class='button'>Save</button>"));
  server.sendContent(F("</form></div>"));
}

/**
 * @brief Sends the MQTT Settings form.
 */
void sendMQTTSettings() {
  server.sendContent(F("<div id='mqtt'><form action='/mqtt'><h3>MQTT Server Connection</h3>"));
  server.sendContent(F("<label>IP address/Domain:</label><input class='full' type='text' name='ip' value='"));
  server.sendContent(mqttServer.toString());
  server.sendContent(F("'>"));
  server.sendContent(F("<label>Port:</label><input class='full' type='number' name='port' value='"));
  server.sendContent(String(mqttPort));
  server.sendContent(F("'>"));
  server.sendContent(F("<label>User:</label><input class='full' maxlength='20'  type='text' name='user' value='"));
  if (strlen(mqttUser) > 0) {
    server.sendContent(escapeHtml(String(mqttUser)));
  }
  server.sendContent(F("'>"));
  server.sendContent(F("<label>Password:</label><input  maxlength='20' class='full' type='password' name='pass' value=''>"));
  server.sendContent(F("<button class='button'>Save</button>"));
  server.sendContent(F("</form><form action='/send-ha'><h3>Home-Assistant Config</h3>"));
  server.sendContent(F("<br><button class='button'>Send Config</button>"));
  server.sendContent(F("</form></div>"));
}

/**
 * @brief Sends the Login Settings form.
 */
void sendLoginSettings() {
  server.sendContent(F("<div id='login'><form action='/login-settings'><h3>Login Information</h3>"));
  server.sendContent(F("<label>User:</label><input maxlength='20' class='full' type='text' name='user' value='"));
  if (strlen(sysConfig.loginUser) > 0) {
    server.sendContent(escapeHtml(String(sysConfig.loginUser)));
  }
  server.sendContent(F("'>"));
  server.sendContent(F("<label>Password:</label><input maxlength='20' class='full' type='password' name='pass' value=''>"));
  server.sendContent(F("<button class='button'>Save</button>"));
  server.sendContent(F("</form></div>"));
}

/**
 * @brief Sends the Packet Control buttons (Start/Stop Real-time).
 */
void sendPacketControl() {
  server.sendContent(F("<div id='login'>"));
  server.sendContent(F("<h3>GEM Packets</h3><form action='/start-real'><input type='hidden' name='send_type' value='1'><button class='button'>Start Packets</button></form>"));
  server.sendContent(F("<form action='/stop-real'><input type='hidden' name='send_type' value='1'><button class='button'>Stop Packets</button></form>"));
  server.sendContent(F("<h3>ECM-1240 Packets</h3><form action='/start-real'><input type='hidden' name='send_type' value='0'><button class='button'>Start Packets</button></form>"));
  server.sendContent(F("<form action='/stop-real'><input type='hidden' name='send_type' value='0'><button class='button'>Stop Packets</button></form>"));
  server.sendContent(F("</div>"));
}

/**
 * @brief Sends the Device Settings form (ECM/GEM specific).
 */
void sendDeviceSettings() {
  server.sendContent(F("<div id='settings'><h3>Device Settings</h3>"));
  if (deviceType == DeviceType::ECM) {
    ecmSettings.gotSettings = false;
    getDeviceSettings();
    server.sendContent(F("<form action='/ecm-settings'>"));
    server.sendContent(F("<h3 style='text-align:center;'>ECM Settings</h3>"));
    server.sendContent(F("<p style='text-align:center;'>Type is a fine-tune value that increases the sensed value with each tick (255 Max).<br>Range halves the sensed value with each increase.</p>"));

    /* General Settings - centered */
    server.sendContent(F("<fieldset style='text-align:center; border:0;'><legend>General</legend>"));
    server.sendContent(F("<label>Settings Retrieved?</label> "));
    server.sendContent(boolToText(ecmSettings.gotSettings, false));
    server.sendContent(F("<br>"));
    server.sendContent(F("<label>Serial Number:</label> "));
    if (ecmSettings.serialNumber.length() > 0) {
      server.sendContent(ecmSettings.serialNumber);
    }
    server.sendContent(F("<br>"));
    server.sendContent(F("<label>Firmware Version:</label> "));
    server.sendContent(String(ecmSettings.firmwareVersion, 4));
    server.sendContent(F("<br>"));
    server.sendContent(F("<label>Packet Send Interval:</label> <input name='packet_send' class='small' type='number' min='1' max='255' value='"));
    server.sendContent(String(ecmSettings.sendInterval));
    server.sendContent(F("'> (Max 255)"));
    server.sendContent(F("</fieldset><br>"));

    /* Channel Configuration Table - centered, no borders */
    server.sendContent(F("<fieldset style='text-align:center; border:0;'><legend>Channel Settings</legend>"));
    server.sendContent(F("<table style='margin:auto; border:none; border-collapse:collapse;'>"));
    server.sendContent(F("<tr><th style='border:none;'>Channel</th><th style='border:none;'>Type</th><th style='border:none;'>Range</th></tr>"));
    server.sendContent(F("<tr><td style='border:none;'>Ch1</td><td style='border:none;'><input name='ch1type' class='small' type='number' min='1' max='255' value='"));
    server.sendContent(String(ecmSettings.ch1Set[0]));
    server.sendContent(F("'></td><td style='border:none;'><input class='small' name='ch1range' type='number' min='1' max='255' value='"));
    server.sendContent(String(ecmSettings.ch1Set[1]));
    server.sendContent(F("'></td></tr>"));
    server.sendContent(F("<tr><td style='border:none;'>Ch2</td><td style='border:none;'><input name='ch2type' class='small' type='number' min='1' max='255' value='"));
    server.sendContent(String(ecmSettings.ch2Set[0]));
    server.sendContent(F("'></td><td style='border:none;'><input class='small' name='ch2range' type='number' min='1' max='255' value='"));
    server.sendContent(String(ecmSettings.ch2Set[1]));
    server.sendContent(F("'></td></tr>"));
    server.sendContent(F("<tr><td style='border:none;'>PT</td><td style='border:none;'><input name='pttype' class='small' type='number' min='1' max='255' value='"));
    server.sendContent(String(ecmSettings.ptSet[0]));
    server.sendContent(F("'></td><td style='border:none;'><input class='small' name='ptrange' type='number' min='1' max='255' value='"));
    server.sendContent(String(ecmSettings.ptSet[1]));
    server.sendContent(F("'></td></tr>"));
    server.sendContent(F("</table>"));
    server.sendContent(F("</fieldset><br>"));

    /* AUX Channel Doubles - centered */
    server.sendContent(F("<fieldset style='text-align:center; border:0;'><legend>AUX Double Settings</legend>"));
    server.sendContent(F("AUX1 <input type='checkbox' name='aux1x2' "));
    server.sendContent(boolToText(ecmSettings.auxX2[0], true));
    server.sendContent(F("> AUX2 <input type='checkbox' name='aux2x2' "));
    server.sendContent(boolToText(ecmSettings.auxX2[1], true));
    server.sendContent(F("> AUX3 <input type='checkbox' name='aux3x2' "));
    server.sendContent(boolToText(ecmSettings.auxX2[2], true));
    server.sendContent(F("> AUX4 <input type='checkbox' name='aux4x2' "));
    server.sendContent(boolToText(ecmSettings.auxX2[3], true));
    server.sendContent(F("> AUX5 <input type='checkbox' name='aux5x2' "));
    server.sendContent(boolToText(ecmSettings.auxX2[4], true));
    server.sendContent(F("<br><br><br>AUX5 Power Input<input name='aux5option' "));
    server.sendContent(aux5Opt(0));
    server.sendContent(F(" type='radio' value='0'><br>"));
    server.sendContent(F("AUX5 Pulse Input<input name='aux5option' "));
    server.sendContent(aux5Opt(1));
    server.sendContent(F(" type='radio' value='1'><br>"));
    server.sendContent(F("AUX5 DC Voltage<input name='aux5option' "));
    server.sendContent(aux5Opt(3));
    server.sendContent(F(" type='radio' value='3'>"));
    server.sendContent(F("</fieldset><br>"));

    /* Submit Button - centered */
    server.sendContent(F("<span style='text-align:center;'><button class='button'>Update Settings</button></span>"));
    server.sendContent(F("<br><br><br><br><form action='/ecm-reset'><button class='button'>Reset Counters</button></form>"));

    server.sendContent(F("</form>"));
  } else if (deviceType == DeviceType::GEM) {
    server.sendContent(F("<b>GreenEye Monitor Detected:</b> "));
    if (deviceData.serialNumber.length() > 0) {
      server.sendContent(deviceData.serialNumber);
    }
    server.sendContent(F("<br><br><br>"));

    if (sysConfig.tcpServerPort > 0) {
      String tmp = "192.168.4.1";

      if (WiFi.status() == WL_CONNECTED) {
        tmp = WiFi.localIP().toString();
      }

      server.sendContent(F("<a class='button' href='http://"));
      server.sendContent(tmp);
      server.sendContent(F(":"));
      server.sendContent(String(sysConfig.tcpServerPort));
      server.sendContent(F("/'>Click Here for Setup</a>"));
    } else {
      server.sendContent(F("Setup TCP Server for setup."));
    }

  } else {
    server.sendContent(F("No monitor detected.<br>On occasion the device type poll can fail, please try refreshing the browser window.<br><br>If detection continues to fail, try changing baud rates.<br><br>The ECM-1240 runs at 19200.<br><br>The GreenEye Monitor COM2 setting is generally set to 115200 but it may be set to 19200 on older models."));
  }
  server.sendContent(F("</div>"));
}

/**
 * @brief Sends the footer content (Update/Reboot/Debug).
 */
void sendFooterContent() {
  server.sendContent(F("<div id='data'>"));
  server.sendContent(F("</div>"));
  server.sendContent(F("<div id='fw'><a href='/updater' class='button'>Update ESP Firmware</a><br><a href='/reboot' class='button'>Reboot</a></div><div id='serialDebug'>"));
  serialDebug();
  server.sendContent(F("</div></body></html>"));
}

/**
 * @brief Handles the main station mode page.
 */
void handleStationMode() {
  if (!isAuthenticated()) {
    sendLogin(false);
  } else {
    //
    //getDeviceSettings();

    // Begin chunked transfer by sending initial part of the HTML
    server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    server.send(200, "text/html", "");
    sendHTMLHeader(PageType::Main);

    sendHeaderContent();
    sendMenu();
    sendNetworkSettings();
    sendIPSettings();
    sendBaudSettings();
    sendTimeServerSettings();
    sendSerialToTCPSettings();
    sendTCPServerSettings();
    sendMQTTSettings();
    sendLoginSettings();
    sendPacketControl();
    sendDeviceSettings();
    sendFooterContent();

    server.sendContent("");
    server.client().stop();
  }
}


/**
 * @brief Returns the checked attribute for AUX5 option.
 * 
 * @param opt Option value to check.
 * @return String "checked='checked'" or " ".
 */
const char* aux5Opt(uint8_t optionValue) {
  return (ecmSettings.aux5Option == optionValue) ? "checked='checked'" : " ";
}

/**
 * @brief Generates and sends the data table HTML.
 */
void getData() {
  int chNum = (deviceType == DeviceType::ECM) ? 7 : 32;

  server.sendContent(F("<h3>Data:</h3><br>"));
  server.sendContent(F("<table id='infoTable'>"));
  server.sendContent(F("<tr><td style='width:125px; padding-bottom:5px;'><b>Serial Number</b></td><td>"));
  if (deviceData.serialNumber.length() > 0) {
    server.sendContent(deviceData.serialNumber);
  }
  server.sendContent(F("</td></tr><tr><td><b>Seconds</b></td><td>"));
  server.sendContent(String(deviceData.seconds));
  server.sendContent(F("<br>"));
  server.sendContent(String(deviceData.prevSeconds));
  server.sendContent(F("</td></tr><tr><td><b>Voltage</b></td><td>"));
  server.sendContent(String(deviceData.voltage));
  server.sendContent(F("</td></tr></table>"));

  server.sendContent(F("<table id='chanTable'>"));
  server.sendContent(F("<tr><th>CH</th><th>Wattseconds</th><th>Pol WS</th><th>Watts</th><th>Amps</th><th>kWh</th><th>Net kWh</th></tr>"));

  for (int i = 0; i < chNum; i++) {
    if (deviceType != DeviceType::ECM || i < 2) {
      server.sendContent(F("<tr><td><b>CH"));
      server.sendContent(String(i + 1));
      server.sendContent(F("</b></td>"));
    } else {
      server.sendContent(F("<tr><td><b>AUX"));
      server.sendContent(String(i - 1));
      server.sendContent(F("</b></td>"));
    }

    server.sendContent(F("<td>"));
    server.sendContent(String(deviceData.wattSeconds[i]));
    server.sendContent(F("<br>"));
    server.sendContent(String(deviceData.prevWattSeconds[i]));
    server.sendContent(F("</td><td>"));
    server.sendContent(String(deviceData.polWattSeconds[i]));
    server.sendContent(F("<br>"));
    server.sendContent(String(deviceData.prevPolWattSeconds[i]));
    server.sendContent(F("</td><td>"));
    server.sendContent(String(deviceData.watts[i]));
    if (deviceData.watts[i] != deviceData.netWatts[i]) {
      server.sendContent(F("<br>"));
      server.sendContent(String(deviceData.netWatts[i]));
    }
    server.sendContent(F("</td><td>"));
    server.sendContent(String(deviceData.amps[i]));
    server.sendContent(F("</td><td>"));
    server.sendContent(String(deviceData.kwh[i], 4));
    server.sendContent(F("<br>"));
    server.sendContent(String(deviceData.totalKwh[i], 4));
    server.sendContent(F("</td><td>"));
    server.sendContent(String(deviceData.netKwh[i], 4));
    server.sendContent(F("<br>"));
    server.sendContent(String(deviceData.totalNetKwh[i], 4));
    server.sendContent(F("</td></tr>"));
  }

  if (deviceType == DeviceType::GEM) {
    server.sendContent(F("<tr><td><b>T</b></td><td colspan='6'>"));
    for (int i = 0; i < 8; i++) {
      server.sendContent(F("CH"));
      server.sendContent(String(i + 1));
      server.sendContent(F(": "));
      server.sendContent(String(deviceData.temp[i], 2));
      server.sendContent(F(" "));
    }
    server.sendContent(F("</td></tr><tr><td><b>P</b></td><td colspan='6'>"));
    for (int i = 0; i < 4; i++) {
      server.sendContent(F("CH"));
      server.sendContent(String(i + 1));
      server.sendContent(F(": "));
      server.sendContent(String(deviceData.pulse[i]));
      server.sendContent(F(" "));
    }
    server.sendContent(F("</td></tr>"));
  }

  if (deviceType == DeviceType::ECM) {
    server.sendContent(F("</td></tr><tr><td><b>Extra</b></td><td colspan='6'>"));
    server.sendContent(F("AUX5 DC: "));
    server.sendContent(String(deviceData.dcVoltage));
    server.sendContent(F(" "));
    server.sendContent(F("</td></tr>"));
  }

  server.sendContent(F("</table>"));
  server.sendContent("");
  server.client().stop();
}

/**
 * @brief Sends the HTML header directly to the server.
 * 
 * @param pageNum Page ID to include specific JS.
 */
void sendHTMLHeader(PageType pageNum) {
  server.sendContent(F("<!DOCTYPE html lang='en'><head><title>Brultech ESP-8266 Setup</title>"));
  server.sendContent(FPSTR(HTML_CSS));

  if (pageNum != PageType::ConfigSaved) {
    if (pageNum == PageType::Main) {
      server.sendContent(FPSTR(HTML_JS_PAGE1));
    } else if (pageNum == PageType::Reboot) {
      server.sendContent(FPSTR(HTML_JS_PAGE2));
    } else {
      server.sendContent(F("<script> document.addEventListener('DOMContentLoaded', function() {"));
      server.sendContent(F("  var selectBox = document.getElementById('ssid');"));
      server.sendContent(F("  var textbox = document.getElementById('custom_ssid');"));
      server.sendContent(F("  selectBox.addEventListener('change', function() { textbox.value = selectBox.value; });"));
      server.sendContent(F("  function copyTextToClipboard(text) {"));
      server.sendContent(F("    navigator.clipboard.writeText(text).then(function() {"));
      server.sendContent(F("      alert('Text copied to clipboard.');"));
      server.sendContent(F("    }, function(err) {"));
      server.sendContent(F("      console.error('Failed to copy text: ', err);"));
      server.sendContent(F("      alert('Failed to copy text.');"));
      server.sendContent(F("    });"));
      server.sendContent(F("  }"));
      server.sendContent(F("  var link = document.getElementById('localLink');"));
      server.sendContent(F("  link.addEventListener('click', function(event) {"));
      server.sendContent(F("    event.preventDefault();"));
      server.sendContent(F("    var linkText = link.innerText || link.textContent;"));
      server.sendContent(F("    copyTextToClipboard(linkText);"));
      server.sendContent(F("  });"));
      server.sendContent(F("  document.getElementById('saveLocal').addEventListener('click', () => {"));
      server.sendContent(F("    const textToCopy = 'http://'"));
      server.sendContent(localAddress);
      server.sendContent(F(".local/';"));
      server.sendContent(F("    copyTextToClipboard(textToCopy);"));
      server.sendContent(F("  });"));
      server.sendContent(F("}); </script>"));
    }
  }

  server.sendContent(F("</head><body>"));
}

/**
 * @brief Handles the MQTT debug page request.
 */
void handleMQTTDebug() {
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/html", F("<!DOCTYPE html>\n<html>\n<head>\n<title>MQTT Data</title>\n</head>\n<body>\n<h1>MQTT Data</h1>\n<p>Configuration Status: "));
  server.sendContent(mqttData.isConfigured ? "Configured" : "Not Configured");
  server.sendContent(F("</p>\n<p>Status: "));
  if (mqttStatus.length() > 0) {
    server.sendContent(mqttStatus);
  }
  server.sendContent(F("</p>\n"));

  // Loop through channel data and display
  for (int i = 0; i < MQTT_MAX_CHANNELS; ++i) {
    server.sendContent(F("<p>Channel "));
    server.sendContent(String(i));
    server.sendContent(F(": "));
    server.sendContent(mqttData.channelEnabled[i] ? "Enabled" : "Disabled");
    server.sendContent(F(" - "));
    if (strlen(mqttData.labels[i]) > 0) {
      server.sendContent(escapeHtml(String(mqttData.labels[i])));
    }
    if (i >= 40 && i < 44) {
      server.sendContent(F(" - "));
      if (mqttData.pulseUnits[i - 40][0] != 0) {
        server.sendContent(String(mqttData.pulseUnits[i - 40][0]));
      }
      server.sendContent(F(" - "));
      if (mqttData.pulseTypes[i - 40][0] != 0) {
        server.sendContent(String(mqttData.pulseTypes[i - 40][0]));
      }
    }
    server.sendContent(F("</p>\n"));
  }

  // Add any other data you want to display

  server.sendContent(F("</body>\n</html>\n"));
  server.client().stop();
}

/**
 * @brief Handles the data request.
 */
void handleData() {
  getData();
}

/**
 * @brief Handles the serial debug request.
 */
void handleSerialDebug() {
  serialDebug();
}

/**
 * @brief Handles the start real-time data request.
 */
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
    server.send(200, "text/html", F("<html><body><p>Real Time Started, redirecting to main...</p><script>setTimeout(function(){ window.location.href = '/main'; }, 2000);</script></body></html>"));
  });
}

/**
 * @brief Handles the ECM reset request.
 */
void handleECMReset() {
  byte data = 0xFC;  // binary 0xFC
  Serial.write(data);
  delay(50);
  Serial.write("RQS");
  delay(50);
  Serial.write("RKW");
  delay(50);

  server.on("/", HTTP_GET, []() {
    server.send(200, "text/html", F("<html><body><p>ECM Counters Reset, redirecting to main...</p><script>setTimeout(function(){ window.location.href = '/main'; }, 2000);</script></body></html>"));
  });
}

/**
 * @brief Handles the stop real-time data request.
 */
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
    server.send(200, "text/html", F("<html><body><p>Real Time Stopped, redirecting to main...</p><script>setTimeout(function(){ window.location.href = '/main'; }, 2000);</script></body></html>"));
  });
}

/**
 * @brief Handles the ECM settings update request.
 */
void handleECMSettings() {
  uint8_t packetSendInterval = server.arg("packet_send").toInt();
  uint8_t channel1Type = server.arg("ch1type").toInt();
  uint8_t channel1Range = server.arg("ch1range").toInt();
  uint8_t channel2Type = server.arg("ch2type").toInt();
  uint8_t channel2Range = server.arg("ch2range").toInt();
  uint8_t ptType = server.arg("pttype").toInt();
  uint8_t ptRange = server.arg("ptrange").toInt();
  bool auxX2[5] = { server.arg("aux1x2").equals("on"), server.arg("aux2x2").equals("on"), server.arg("aux3x2").equals("on"), server.arg("aux4x2").equals("on"), server.arg("aux5x2").equals("on") };
  uint8_t aux5Config = server.arg("aux5option").toInt();
  double fwVer = (double)ecmSettings.firmwareVersion;
  String sendSettings = "";
  char formatBuffer[16];

  bool isAuxChanged = false;

  uint8_t aux5Bitmask = 0;
  int auxIndex = 0;

  for (; auxIndex < 5; auxIndex++) {
    if (auxX2[auxIndex]) {
      aux5Bitmask |= 1 << auxIndex;
    }

    if (auxX2[auxIndex] != ecmSettings.auxX2[auxIndex]) {
      isAuxChanged = true;
    }
  }

  if (aux5Config == 1) {
    aux5Bitmask |= 1 << auxIndex;
  } else if (aux5Config == 3) {
    aux5Bitmask |= 1 << ++auxIndex;
  }

  if (aux5Config != ecmSettings.aux5Option) {
    isAuxChanged = true;
  }

  int aux5ConfigValue = aux5Bitmask;
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/html", "");
  sendHTMLHeader(PageType::Reboot);
  server.sendContent(F("<div>"));

  // Generate the HTML page with the variable values
  if (channel1Type > 0 && channel2Type > 0 && channel1Range > 0 && channel2Range > 0 && ptType > 0 && ptRange > 0 && packetSendInterval > 0) {
    String commandsSent = "";
    byte data = 0xFC;  // binary 0xFC
    if (fwVer > 5) {
      if (isAuxChanged || packetSendInterval != ecmSettings.sendInterval || channel1Type != ecmSettings.ch1Set[0] || channel1Range != ecmSettings.ch1Set[1] || channel2Type != ecmSettings.ch2Set[0] || channel2Range != ecmSettings.ch2Set[1] || ptType != ecmSettings.ptSet[0] || ptRange != ecmSettings.ptSet[1]) {
        sendSettings = "1,";
        snprintf(formatBuffer, sizeof(formatBuffer), "%03d,", channel1Type);
        sendSettings += formatBuffer;
        snprintf(formatBuffer, sizeof(formatBuffer), "%03d,", channel1Range);
        sendSettings += formatBuffer;
        snprintf(formatBuffer, sizeof(formatBuffer), "%03d,", channel2Type);
        sendSettings += formatBuffer;
        snprintf(formatBuffer, sizeof(formatBuffer), "%03d,", channel2Range);
        sendSettings += formatBuffer;
        snprintf(formatBuffer, sizeof(formatBuffer), "%03d,", ptType);
        sendSettings += formatBuffer;
        snprintf(formatBuffer, sizeof(formatBuffer), "%03d,", ptRange);
        sendSettings += formatBuffer;
        snprintf(formatBuffer, sizeof(formatBuffer), "%03d,", aux5ConfigValue);
        sendSettings += formatBuffer;
        snprintf(formatBuffer, sizeof(formatBuffer), "%03d", packetSendInterval);
        sendSettings += formatBuffer;
        commandsSent += "SETALL" + sendSettings;
        Serial.write(data);
        delay(50);
        Serial.write(sendSettings.c_str());
        server.sendContent(F("<h3>Settings saved.</h3><br>"));
      } else {
        server.sendContent(F("<h3>No change detected.</h3><br>"));
      }
    } else {
      bool change = false;
      if (packetSendInterval != ecmSettings.sendInterval) {
        Serial.write(data);
        delay(50);
        Serial.write("SET");
        delay(50);
        Serial.write("IV2");
        delay(50);
        Serial.write((char)packetSendInterval);
        delay(50);

        commandsSent += "SETIV2" + String(packetSendInterval) + "<br>";
        change = true;
      }

      if (channel1Type != ecmSettings.ch1Set[0] || channel1Range != ecmSettings.ch1Set[1]) {
        Serial.write(data);
        delay(50);
        Serial.write("SET");
        delay(50);
        Serial.write("CT1");
        delay(50);
        Serial.write("TYP");
        delay(50);
        Serial.write((char)channel1Type);
        delay(50);
        Serial.write("RNG");
        delay(50);
        Serial.write((char)channel1Range);
        delay(50);
        commandsSent += "SETCT1TYP" + String(channel1Type) + "RNG" + String(channel1Range) + "<br>";
        change = true;
      }

      if (channel2Type != ecmSettings.ch2Set[0] || channel2Range != ecmSettings.ch2Set[1]) {
        Serial.write(data);
        delay(50);
        Serial.write("SET");
        delay(50);
        Serial.write("CT2");
        delay(50);
        Serial.write("TYP");
        delay(50);
        Serial.write((char)channel2Type);
        delay(50);
        Serial.write("RNG");
        delay(50);
        Serial.write((char)channel2Range);
        delay(50);
        commandsSent += "SETCT2TYP" + String(channel2Type) + "RNG" + String(channel2Range) + "<br>";
        change = true;
      }

      if (ptType != ecmSettings.ptSet[0] || ptRange != ecmSettings.ptSet[1]) {
        Serial.write(data);
        delay(50);
        Serial.write("SET");
        delay(50);
        Serial.write("PTT");
        delay(50);
        Serial.write((char)ptType);
        delay(50);
        Serial.write("PTR");
        delay(50);
        Serial.write((char)ptRange);
        delay(50);
        commandsSent += "SETPTT" + String(ptType) + "PTR" + String(ptRange) + "<br>";
        change = true;
      }

      if (isAuxChanged) {
        Serial.write(data);
        delay(50);
        Serial.write("SET");
        delay(50);
        Serial.write("OPT");
        delay(50);
        Serial.write((char)aux5ConfigValue);
        commandsSent += "SETOPT" + String(aux5ConfigValue) + "<br>";
        change = true;
      }

      if (change) {
        server.sendContent(F("<h3>Settings saved.</h3><br>"));
      } else {
        server.sendContent(F("<h3>No change detected.</h3><br>"));
      }
    }

    if (commandsSent.length() > 0) {
      server.sendContent(commandsSent);
    }
  } else {
    server.sendContent(F("All values must be non-zero."));
  }
  server.sendContent(F("Click <a href='/main'>here</a> to return to the settings page."));

  server.sendContent(F("</div></body></html>"));
  server.client().stop();
}

/**
 * @brief Converts boolean to text representation.
 * 
 * @param check The boolean value.
 * @param input Whether it's an input field attribute.
 * @return String Text representation.
 */
const char* boolToText(bool check, bool input) {
  if (check) return input ? "checked='checked'" : "True";
  return input ? " " : "False";
}

/**
 * @brief Handles the root path request.
 */
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

/**
 * @brief Sends the login page.
 * 
 * @param error Whether to show an error message.
 */
void sendLogin(bool error) {
  // Send the login HTML page if not authenticated
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/html", "");
  sendHTMLHeader(PageType::Login);
  server.sendContent(F("<div"));

  if (error) {
    server.sendContent(F("<p class='error'>Invalid username or password.</p>"));
  }
  server.sendContent(F("<form action='/login' method='post'><label for='username'>Username:</label> <input class='full' type='text' id='username' name='username'> <label for='password'>Password:</label> <input class='full' type='password' id='password' name='password'> <button class='button' type='submit'>Login</button> </form> </div> </body> </html>"));
  server.client().stop();
}

/**
 * @brief Handles the login submission.
 */
void handleLogin() {
  String loginUsername = server.arg("username");
  String loginPassword = server.arg("password");

  // Replace with your authentication logic
  if (loginUsername == sysConfig.loginUser && loginPassword == sysConfig.loginPass) {
    // Set the session cookie and redirect to the dashboard page
    globalSessionId = String(random(0x7FFFFFFF), HEX);
    server.sendHeader("Set-Cookie", "session_id=" + globalSessionId + "; Max-Age=7200; HttpOnly");
    server.sendHeader("Location", "/main");
    server.send(302);
  } else {
    // Send the login HTML page with an error message if authentication failed
    sendLogin(true);
  }
}

/**
 * @brief Handles the Serial to TCP client configuration.
 */
void handleSerialToTcp() {
  // Serial to TCP client connection
  IPAddress ip;
  int tcpPortArg = server.arg("port").toInt();
  IPAddress ipStore;

  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/html", "");
  sendHTMLHeader(PageType::Reboot);

  // Error test the client connection
  if (ip.fromString(server.arg("ip")) && tcpPortArg > 1024 && tcpPortArg < 65536) {

    storeIP(tcpIPAddress, ip);
    EEPROM.put(tcpPortAddress, tcpPortArg);
    EEPROM.commit();

    tcpIP = ip;
    sysConfig.tcpPort = tcpPortArg;

    server.sendContent(F("<h2>IP: "));
    server.sendContent(ip.toString());
    server.sendContent(F(" Port: "));
    server.sendContent(String(tcpPortArg));
    server.sendContent(F(" saved to EEPROM.  Starting server...</h2>"));

    if (tcpClient.connected()) {
      tcpClient.stop();
      tcpClient.setTimeout(100);

      if (tcpClient.connect(tcpIP, sysConfig.tcpPort)) {
        server.sendContent(F("<h5>Connected to TCP server</h5>"));
      } else {
        server.sendContent(F("<h5>Connection failed</h5>"));
      }
    }

  } else if (server.arg("ip") == "") {
    tcpIP = IPAddress(0, 0, 0, 0);
    storeIP(tcpIPAddress, IPAddress(0, 0, 0, 0));
  } else {
    server.sendContent(F("<h2>Invalid IP Address or Port</h2>"));
  }

  server.sendContent(F("</body></html>"));
  server.client().stop();
}

/**
 * @brief Handles the Serial to TCP Server configuration.
 */
void handleSerialToTcpServer() {
  // Serial to TCP client connection
  int tcpServerPortArg = server.arg("port").toInt();
  uint16_t idleTimeSeconds = server.arg("idle_time").toInt();

  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/html", "");
  sendHTMLHeader(PageType::Reboot);

  // Error test the client connection
  if ((tcpServerPortArg > 1024 && tcpServerPortArg < 65536 && idleTimeSeconds > 0 && idleTimeSeconds < 6001) || tcpServerPortArg == 0) {

    EEPROM.put(tcpServerPortAddress, tcpServerPortArg);
    EEPROM.commit();
    EEPROM.put(idleTimeAddress, idleTimeSeconds);
    EEPROM.commit();

    sysConfig.idleTime = idleTimeSeconds;

    sysConfig.tcpServerPort = tcpServerPortArg;

    if (tcpServerPortArg != 0) {
      server.sendContent(F("<h2>Port saved to EEPROM.  Starting server...</h2>"));
    } else {
      server.sendContent(F("<h2>TCP Server Mode has been disabled.</h2>"));
    }

    ecmServer.stop();
    ecmServer.begin(sysConfig.tcpServerPort);
    //ecmServer.setNoDelay(true);
  } else {
    server.sendContent(F("<h2>Port or Idle Time is out of range.</h2>"));
  }

  server.sendContent(F("</body></html>"));
  server.client().stop();
}


/**
 * @brief Handles the NTP Server configuration.
 */
void handleNTPServer() {
  String tempNTP = server.arg("ntp_server");

  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/html", "");
  sendHTMLHeader(PageType::Reboot);

  if (strlen(tempNTP.c_str()) < 41) {
    tempNTP.toCharArray(sysConfig.ntpServer, 40);
    EEPROM.put(ntpServerAddress, sysConfig.ntpServer);
    EEPROM.commit();

    server.sendContent(F("<h2>"));
    server.sendContent(String(sysConfig.ntpServer));
    server.sendContent(F(" saved to EEPROM.  Starting server...</h2>"));

    if (strlen(sysConfig.ntpServer) > 0) {
      configTime(0, 0, sysConfig.ntpServer);
    }
  } else {
    server.sendContent(F("<h2>NTP Server Address is too longer then 50 characters.</h2>"));
  }

  server.sendContent(F("</body></html>"));
  server.client().stop();
}

/**
 * @brief Handles the MQTT configuration.
 */
void handleMqtt() {

  // Serial to TCP client connection
  IPAddress ip;
  String mqttUsername = server.arg("user");
  String mqttPassword = server.arg("pass");
  int mqttPortArg = server.arg("port").toInt();

  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/html", "");
  sendHTMLHeader(PageType::Reboot);

  // Error test the client connection
  if (mqttPortArg > 1024 && mqttPortArg < 65536 && ip.fromString(server.arg("ip"))) {
    EEPROM.commit();

    mqttServer = ip;
    mqttPort = mqttPortArg;
    strcpy(mqttUser, mqttUsername.c_str());
    strcpy(mqttPass, mqttPassword.c_str());

    storeIP(mqttServerAddress, mqttServer);
    EEPROM.put(mqttPortAddress, mqttPort);
    EEPROM.put(mqttUserAddress, mqttUser);
    EEPROM.put(mqttPassAddress, mqttPass);

    EEPROM.commit();

    mqttClient.disconnect();

    mqttClient.setServer(mqttServer, mqttPort);

    server.sendContent(F("<div><h3>Address: "));
    server.sendContent(mqttServer.toString());
    server.sendContent(F(" Port: "));
    server.sendContent(String(mqttPortArg));
    server.sendContent(F(" User: "));
    server.sendContent(String(mqttUser));
    server.sendContent(F(" Pass:  "));
    server.sendContent(String(mqttPass));
    server.sendContent(F(" saved to EEPROM.</h3></div>"));

    if (!mqttClient.connect(mqttClientID, mqttUser, mqttPass)) {
      server.sendContent(F("<div><h3>MQTT couldn't connect, please check your settings.</h3></div>"));
    } else {
      mqttClient.disconnect();
    }
  } else if (server.arg("ip") == "") {
    mqttServer = IPAddress(0, 0, 0, 0);
    storeIP(mqttServerAddress, IPAddress(0, 0, 0, 0));
  } else {
    server.sendContent(F("<h2>Invalid IP Address or Port</h2>"));
  }

  server.sendContent(F("</body></html>"));
  server.client().stop();
}

/**
 * @brief Handles the Baud Rate configuration.
 */
void handleBaud() {
  // Serial to TCP client connection
  uint32_t baudRate = server.arg("baud").toInt();

  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/html", "");
  sendHTMLHeader(PageType::Reboot);

  // Error test the client connection
  if (baudRate == 19200 || baudRate == 115200) {
    EEPROM.put(baudAddress, baudRate);
    EEPROM.commit();

    if (baudRate != sysConfig.baud) {
      Serial.updateBaudRate(baudRate);
    }

    sysConfig.baud = baudRate;

    server.sendContent(F("<div><h3>Baudrate "));
    server.sendContent(String(baudRate));
    server.sendContent(F(" saved.</h3></div>"));
  } else {
    server.sendContent(F("<div><h3>Invalid Baudrate</h3></div>"));
  }

  server.sendContent(F("</body></html>"));
  server.client().stop();
}

/**
 * @brief Handles the IP configuration.
 */
void handleIPConfig() {
  IPAddressConfig config;
  uint8_t type = server.arg("type").toInt();
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/html", "");
  sendHTMLHeader(PageType::Reboot);

  // Parse form data and validate IP settings
  if (type == 1) {
    if (config.ip.fromString(server.arg("ip")) && config.subnet.fromString(server.arg("subnet")) && config.gateway.fromString(server.arg("gateway")) && config.dns.fromString(server.arg("dns"))) {
      config.isConfigured = true;

      // Write the IP settings structure to EEPROM
      EEPROM.put(ipConfigAddress, config);

      EEPROM.commit();  // Save changes to EEPROM

      storedIPConfig = config;

      server.sendContent(F("<div><h3>IP settings saved, click <a href='http://"));
      server.sendContent(config.ip.toString());
      server.sendContent(F("/'>here</a> to access the unit.<br><br>If you can't access the module afterwards it can be reset by using the push button.</h3></div></body></html>"));
      server.client().stop();

      delay(250);

      WiFi.config(config.ip, config.dns, config.gateway, config.subnet);
    } else {
      server.sendContent(F("<div><h3>Invalid IP Address, Subnet, Gateway, or DNS.</h3></div></body></html>"));
      server.client().stop();
    }
  } else {
    storedIPConfig.isConfigured = false;

    // Write the IP settings structure to EEPROM
    EEPROM.put(ipConfigAddress, storedIPConfig);

    EEPROM.commit();  // Save changes to EEPROM

    server.sendContent(F("<div><h3>IP settings changed to DHCP, click <a href='http://brultechesp.local/'>here</a> to access the unit.<br><br>If you can't access the module afterwards it can be reset by using the push button.</h3></div></body></html>"));
    server.client().stop();

    delay(250);

    WiFi.config(0, 0, 0);
  }
}

/**
 * @brief Handles the login settings configuration.
 */
void handleLoginSettings() {
  // Serial to TCP client connection
  String loginUsername = server.arg("user");
  String loginPassword = server.arg("pass");

  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/html", "");
  sendHTMLHeader(PageType::Reboot);

  strcpy(sysConfig.loginUser, loginUsername.c_str());
  strcpy(sysConfig.loginPass, loginPassword.c_str());

  EEPROM.put(loginUserAddress, sysConfig.loginUser);
  EEPROM.put(loginPassAddress, sysConfig.loginPass);
  EEPROM.commit();

  httpUpdater.setup(&server, sysConfig.loginUser, sysConfig.loginPass);

  server.sendContent(F("<div><h3>User: "));
  server.sendContent(String(sysConfig.loginUser));
  server.sendContent(F(" Pass:  "));
  server.sendContent(String(sysConfig.loginPass));
  server.sendContent(F(" saved to EEPROM.</h3></div></body></html>"));
  server.client().stop();
}

/**
 * @brief Stores an IP address in EEPROM.
 * 
 * @param addressOffset EEPROM address offset.
 * @param ip IPAddress to store.
 */
void storeIP(int eepromAddress, const IPAddress& ip) {
  for (int i = 0; i < 4; i++) {
    EEPROM.write(eepromAddress + i, ip[i]);
  }

  EEPROM.commit();
}

/**
 * @brief Reads a string from EEPROM.
 * 
 * @param location EEPROM address.
 * @return String Read string.
 */
String getString(int eepromAddress) {
  String readString = "";
  char c = EEPROM.read(eepromAddress++);
  while (c != '\0' && eepromAddress < EEPROM.length()) {
    readString += c;
    c = EEPROM.read(eepromAddress++);
  }

  return readString.c_str();
}

/**
 * @brief Checks if the user is authenticated.
 * 
 * @return true If authenticated.
 * @return false If not authenticated.
 */
bool isAuthenticated() {
  // 1. If no username AND password are configured, allow access immediately
  if (!(strlen(sysConfig.loginUser) > 0 && strlen(sysConfig.loginPass) > 0)) {
    return true;
  }

  // 2. If credentials ARE configured, check for a valid session
  if (globalSessionId.length() == 0) {
    return false;
  }

  // 3. Check if the session cookie matches the active session
  String cookie = server.header("Cookie");
  if (cookie.indexOf("session_id=" + globalSessionId) != -1) {
    return true;
  }

  return false;
}

/**
 * @brief Reads an IP address from EEPROM.
 * 
 * @param location EEPROM address.
 * @return IPAddress Read IP address.
 */
IPAddress getIP(int eepromAddress) {
  uint8_t storedIPBytes[4];
  for (int i = 0; i < 4; i++) {
    storedIPBytes[i] = EEPROM.read(eepromAddress + i);
  }

  if (IPAddress(storedIPBytes).isSet()) {
    return IPAddress(storedIPBytes);
  } else {
    return IPAddress(0, 0, 0, 0);
  }
}

/**
 * @brief Gets the current formatted date and time.
 * 
 * @return String Formatted date and time.
 */
String getFormattedDate() {
  time_t now = time(nullptr);
  if (now < 10000) return "Unable to retrieve date, check NTP Server settings.";

  struct tm* timeinfo = gmtime(&now);
  char buffer[25];
  snprintf(buffer, sizeof(buffer), "%04d-%02d-%02dT%02d:%02d:%02dZ",
           timeinfo->tm_year + 1900, timeinfo->tm_mon + 1, timeinfo->tm_mday,
           timeinfo->tm_hour, timeinfo->tm_min, timeinfo->tm_sec);
  return String(buffer);
}

// Static CSS (shared across all pages)
const char HTML_CSS[] PROGMEM = R"(
<style>
body { background-color:#c3c3c3; font-family: Arial, sans-serif; }
select { font-size: 16px; font-family: Arial, sans-serif; }
div { background-color: #fff; border: 1px solid #ccc; box-shadow: 0 2px 2px rgba(0, 0, 0, 0.1); margin: 50px auto; width: 600px; padding: 20px; text-align: center; }
h1 { margin: 0 0 20px 0; }
label { padding-top:5px; display: block; font-size: 16px; font-weight: bold; margin-bottom: 5px; text-align: left; }
.full { box-sizing: border-box; border: 1px solid #ccc; font-size: 16px; padding: 10px; width: 100% ; }
.small { box-sizing: border-box; border: 1px solid #ccc; font-size: 16px; padding: 10px; width: 100px ; }
.mid { box-sizing: border-box; border: 1px solid #ccc; font-size: 16px; padding-left: 10px; padding-right: 10px; width: 150px ; }
.button { background-color: #4CAF50; border: none; color: #fff; cursor: pointer; font-size: 16px; margin-top: 20px; padding: 10px; display:inline-block; margin:5px; text-decoration: none; }
.button:hover { background-color: #45a049; }
p.error { color: #f00; font-size: 14px; margin: 10px 0; text-align: left; }
#chanTable {width: 100%; border-collapse: collapse;}
#chanTable th, td {border: 1px solid black; padding: 8px; text-align: center;}
#chanTable th {background-color: #f2f2f2;}
#infoTable { margin: 0 auto; border:none; }
#infoTable td { border:none; }
#pleaseWait { width:500px; }
legend { font-weight: bold; }
</style>
)";

// JS for STA Page
const char HTML_JS_PAGE1[] PROGMEM = R"(
<script>
var ip = location.host;

document.addEventListener('DOMContentLoaded', function() {
    var selectBox = document.getElementById('ssid');
    var textbox = document.getElementById('custom_ssid');
    
    selectBox.addEventListener('change', function() {
        textbox.value = selectBox.value;
    });
    
    // Add scan button functionality
    var scanButton = document.getElementById('scan');
    if (scanButton) {
        scanButton.addEventListener('click', function() {
            event.preventDefault(); // <-- Prevent form submission
            scanButton.style.display = 'none'; // Hide button
            scanNetworks(scanButton);
        });
    }
});

function scanNetworks(buttonToShow) {
    const xhr = new XMLHttpRequest();
    xhr.onreadystatechange = function() {
      if (this.readyState === 4) {
        if (this.status === 200) {
            document.getElementById('ssid').innerHTML = this.responseText;
        } else {
            alert('Failed to scan networks.');
        }
        if (buttonToShow) {
            buttonToShow.style.display = 'inline-block';
        }
      }
    };

    xhr.open('GET', '/scan');
    xhr.send();
}

function updateDiv() {
    const xhr = new XMLHttpRequest();
    xhr.onreadystatechange = function() {
        if (this.readyState === 4 && this.status === 200) {
            document.getElementById("data").innerHTML = this.responseText;
        }
    };
    xhr.open("GET", "/data");
    xhr.send();
    
    const xhrTwo = new XMLHttpRequest();
    xhrTwo.onreadystatechange = function() {
        if (this.readyState === 4 && this.status === 200) {
            document.getElementById("serialDebug").innerHTML = this.responseText;
        }
    };
    xhrTwo.open("GET", "/serial-debug");
    xhrTwo.send();
}

setInterval(updateDiv, 5000);
</script>
)";

// JS to auto-kick back on settings confirmation
const char HTML_JS_PAGE2[] PROGMEM = R"(
<script>
function goBack() { 
    history.back(); 
}
setInterval(goBack, 5000);
</script>
)";
