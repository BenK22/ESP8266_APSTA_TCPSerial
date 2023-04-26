#include <ESP8266WiFi.h>
#include <EEPROM.h>
#include <ESP8266WebServer.h>
#include <PubSubClient.h>
#include <ESP8266mDNS.h>

struct EcmData {
  double voltage;
  uint64_t wattSeconds[7];
  uint64_t prevWattSeconds[7];
  uint32_t deltaWattSeconds[7];
  uint64_t polWattSeconds[2];
  uint64_t prevPolWattSeconds[2];
  uint16_t seconds;
  uint16_t prevSeconds;
  String serialNumber;
  uint16_t watts[7];
  uint16_t netWatts[2];
  double kwh[7];
};

EcmData ecmData = {
  0.0,                      // voltage
  { 0, 0, 0, 0, 0, 0, 0 },  // wattSeconds
  { 0, 0, 0, 0, 0, 0, 0 },  // prevWattSeconds
  { 0, 0, 0, 0, 0, 0, 0 },  // prevWattSeconds
  { 0, 0 },                 // polWattSeconds
  { 0, 0 },                 // prevPolWattSeconds
  0,                        // serialNumber
  0,                        // seconds
  "0",                      // prevSeconds
  { 0, 0, 0, 0, 0, 0, 0 },  // wattSeconds
  { 0, 0 },                 // prevWattSeconds
  { 0, 0, 0, 0, 0, 0, 0 }   // prevWattSeconds
};

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
const byte isFirstRunValue = 0xAA;

// Serial buffer
const int MAX_DATA_LENGTH = 512;  // Set the maximum length of the data
char buffer[MAX_DATA_LENGTH];     // Declare the array to store the data
int dataLength = 0;               // Declare a variable to keep track of the length of the data

WiFiClient client;
WiFiClient clientTwo;

ESP8266WebServer server(80);
PubSubClient mqttClient(clientTwo);

String tcpIp = "";
int tcpPort = 0;

int tcpServerPort = 8000;
WiFiServer ecmServer(5555);

String mqttServer = "";
String mqttUser = "";
String mqttPass = "";
String mqttClientID = "";
int mqttPort = 1883;

uint8_t mac[6];

String loginUser = "";
String loginPass = "";

const int LED_PIN = 0;  // GPIO0
bool ledState = false;

const String css = "<style> body { background-color:#f2f2f2; font-family: Arial, sans-serif; } div { background-color: #fff; border: 1px solid #ccc; box-shadow: 0 2px 2px rgba(0, 0, 0, 0.1); margin: 50px auto; max-width: 400px; padding: 20px; text-align: center; } h1 { margin: 0 0 20px 0; } label { padding-top:5px; display: block; font-size: 16px; font-weight: bold; margin-bottom: 5px; text-align: left; } input { box-sizing: border-box; border: 1px solid #ccc; font-size: 16px; padding: 10px; width: 100% ; } button { background-color: #4CAF50; border: none; color: #fff; cursor: pointer; font-size: 16px; margin-top: 20px; padding: 10px; width: 100% ; } button: hover { background-color: #45a049; } p.error { color: #f00; font-size: 14px; margin: 10px 0; text-align: left; } </style>";

void setup() {
  pinMode(LED_PIN, OUTPUT);

  WiFi.macAddress(mac);
  mqttClientID = "ecm1240-" + String(mac[3], HEX) + String(mac[4], HEX) + String(mac[5], HEX);
  // Start EEPROM
  EEPROM.begin(eepromSize);

  // Start serial port
  Serial.begin(19200);
  Serial.flush();
  Serial.setTimeout(500);

  // Check if this is the first run
  byte isFirstRun = EEPROM.read(isFirstRunAddress);
  if (isFirstRun != isFirstRunValue) {
    for (int i = 0; i < eepromSize; i++) {
      EEPROM.write(i, 0);
    }
    EEPROM.write(isFirstRunAddress, isFirstRunValue);
    EEPROM.commit();
  }

  // Read settings from EEPROM
  EEPROM.get(ssidAddress, ssid);
  EEPROM.get(passwordAddress, password);

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

  Serial.println(loginUser);
  Serial.println(loginPass);

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
    server.on("/main", handleStationMode);
    server.on("/config", handleConfig);
    server.on("/login-settings", handleLoginSettings);
    server.on("/serial-to-tcp", handleSerialToTcp);
    server.on("/serial-to-tcp-server", handleSerialToTcpServer);
    server.on("/mqtt", handleMqtt);
    server.on("/send-ha", handleHA);
    MDNS.begin("esp8266");
    delay(1000);
  }

  server.on("/", handleRoot);
  server.on("/login", handleLogin);
  //here the list of headers to be recorded
  const char* headerkeys[] = { "User-Agent", "Cookie" };
  size_t headerkeyssize = sizeof(headerkeys) / sizeof(char*);
  //ask server to track these headers
  server.collectHeaders(headerkeys, headerkeyssize);
  server.begin();

  ecmServer.stop();
  ecmServer.begin(tcpServerPort);
}

void loop() {
  MDNS.update();
  server.handleClient();

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
  }

  if (client.connected()) {

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
    }
  }

  delay(50);
}

void handlePacket() {
  if (dataLength >= 65) {
    // Validate the first three bytes and last 2 bytes
    if (buffer[0] == 0xFE && buffer[1] == 0xFF && buffer[2] == 0x03 && buffer[62] == 0xFF && buffer[63] == 0xFE) {
      memcpy(ecmData.prevWattSeconds, ecmData.wattSeconds, sizeof(ecmData.prevWattSeconds));
      memcpy(ecmData.prevPolWattSeconds, ecmData.polWattSeconds, sizeof(ecmData.prevPolWattSeconds));

      ecmData.prevSeconds = ecmData.seconds;

      // Extract the voltage value as an unsigned integer
      ecmData.voltage = static_cast<float>((buffer[3] << 8) | buffer[4]) / 10;
      ecmData.seconds = ((uint16_t)buffer[39] << 16) | ((uint16_t)buffer[38] << 8) | (uint16_t)buffer[37];


      // Extract the 4-byte value as an unsigned integer
      ecmData.wattSeconds[0] = ((uint64_t)buffer[9] << 32) | ((uint64_t)buffer[8] << 24) | ((uint64_t)buffer[7] << 16) | ((uint64_t)buffer[6] << 8) | (uint64_t)buffer[5];
      ecmData.wattSeconds[1] = ((uint64_t)buffer[14] << 32) | ((uint64_t)buffer[13] << 24) | ((uint64_t)buffer[12] << 16) | ((uint64_t)buffer[11] << 8) | (uint64_t)buffer[10];
      ecmData.wattSeconds[2] = ((uint64_t)buffer[43] << 24) | ((uint64_t)buffer[42] << 16) | ((uint64_t)buffer[41] << 8) | (uint64_t)buffer[40];
      ecmData.wattSeconds[3] = ((uint64_t)buffer[47] << 24) | ((uint64_t)buffer[46] << 16) | ((uint64_t)buffer[45] << 8) | (uint64_t)buffer[44];
      ecmData.wattSeconds[4] = ((uint64_t)buffer[51] << 24) | ((uint64_t)buffer[50] << 16) | ((uint64_t)buffer[49] << 8) | (uint64_t)buffer[48];
      ecmData.wattSeconds[5] = ((uint64_t)buffer[55] << 24) | ((uint64_t)buffer[54] << 16) | ((uint64_t)buffer[53] << 8) | (uint64_t)buffer[52];
      ecmData.wattSeconds[6] = ((uint64_t)buffer[59] << 24) | ((uint64_t)buffer[58] << 16) | ((uint64_t)buffer[57] << 8) | (uint64_t)buffer[56];

      ecmData.polWattSeconds[0] = ((uint64_t)buffer[19] << 32) | ((uint64_t)buffer[18] << 24) | ((uint64_t)buffer[17] << 16) | ((uint64_t)buffer[16] << 8) | (uint64_t)buffer[15];
      ecmData.polWattSeconds[1] = ((uint64_t)buffer[24] << 32) | ((uint64_t)buffer[23] << 24) | ((uint64_t)buffer[22] << 16) | ((uint64_t)buffer[21] << 8) | (uint64_t)buffer[20];

      ecmData.serialNumber = String((uint16_t)buffer[32]) + String(((buffer[29] << 8) | buffer[30]));

      for (int i = 0; i < 7; i++) {
        if (ecmData.prevWattSeconds[i] != 0) {
          processPacket();
          break;
        }
      }
    }
  }
}

void processPacket() {
  uint16_t secDiff = 0;

  if (ecmData.prevSeconds > ecmData.seconds) {
    secDiff = ecmData.seconds + 256 ^ 3 - ecmData.prevSeconds;
  } else {
    secDiff = ecmData.seconds - ecmData.prevSeconds;
  }

  uint64_t wattSecDiff = 0;
  uint64_t polWattSecDiff = 0;

  for (int x = 0; x < 7; x++) {
    if (ecmData.prevWattSeconds[x] > ecmData.wattSeconds[x]) {
      ecmData.deltaWattSeconds[x] = (ecmData.wattSeconds[x] + 256 ^ 5 - ecmData.prevWattSeconds[x]);
    } else {
      ecmData.deltaWattSeconds[x] = (ecmData.wattSeconds[x] - ecmData.prevWattSeconds[x]);
    }

    if (ecmData.prevPolWattSeconds[x] > ecmData.polWattSeconds[x]) {
      polWattSecDiff = (ecmData.polWattSeconds[x] + 256 ^ 5 - ecmData.prevPolWattSeconds[x]);
    } else {
      polWattSecDiff = (ecmData.polWattSeconds[x] - ecmData.prevPolWattSeconds[x]);
    }

    if (x < 2) {
      ecmData.netWatts[x] = ((2 * polWattSecDiff) - ecmData.deltaWattSeconds[x]) / secDiff;
    }

    ecmData.watts[x] = ecmData.deltaWattSeconds[x] / secDiff;
    ecmData.kwh[x] = static_cast<float>(ecmData.deltaWattSeconds[x]) / 360000;

    mqttPost();
  }
}

void mqttPost() {
  if (WiFi.status() == WL_CONNECTED) {
    mqttClient.setServer(mqttServer.c_str(), mqttPort);

    if (!mqttClient.connect(mqttClientID.c_str(), mqttUser.c_str(), mqttPass.c_str())) {
      Serial.println("Failed to connect to MQTT broker");
      return;
    }

    // Publish topics
    mqttClient.publish(("ecm1240-" + String(ecmData.serialNumber) + "/v").c_str(), String(ecmData.voltage).c_str());

    for (int i = 0; i < 7; i++) {
      mqttClient.publish(("ecm1240-" + String(ecmData.serialNumber) + "/c" + String(i + 1) + "/watt").c_str(), String(ecmData.watts[i]).c_str());
      mqttClient.publish(("ecm1240-" + String(ecmData.serialNumber) + "/c" + String(i + 1) + "/kwh").c_str(), String(ecmData.kwh[i]).c_str());
      mqttClient.publish(("ecm1240-" + String(ecmData.serialNumber) + "/c" + String(i + 1) + "/ws").c_str(), String(ecmData.wattSeconds[i]).c_str());
      mqttClient.publish(("ecm1240-" + String(ecmData.serialNumber) + "/c" + String(i + 1) + "/pws").c_str(), String(ecmData.polWattSeconds[i]).c_str());
      mqttClient.publish(("ecm1240-" + String(ecmData.serialNumber) + "/c" + String(i + 1) + "/dws").c_str(), String(ecmData.deltaWattSeconds[i]).c_str());
    }

    // Disconnect from MQTT broker
    mqttClient.disconnect();
  }
}

void handleHA() {
  if (WiFi.status() == WL_CONNECTED) {
    mqttClient.setServer(mqttServer.c_str(), mqttPort);

    if (!mqttClient.connect(mqttClientID.c_str(), mqttUser.c_str(), mqttPass.c_str())) {
      return;
    }

    String payload = "{\"name\":\"ECM1240 " + ecmData.serialNumber + " Volts\",\"state_topic\":\"ecm1240-" + ecmData.serialNumber + "/v\",\"unit_of_measurement\":\"V\"}";
    String topic = "homeassistant/sensor/ecm1240-" + ecmData.serialNumber + "/voltage_sensor/config";
    mqttClient.publish(topic.c_str(), payload.c_str(), true);

    for (int x = 0; x < 7; x++) {
      payload = "{\"name\":\"ECM1240 " + ecmData.serialNumber + " CH" + (x + 1) + " kWh\",\"state_topic\":\"ecm1240-" + ecmData.serialNumber + "/c" + (x + 1) + "/kwh\",\"unit_of_measurement\":\"kWh\"}";
      topic = "homeassistant/sensor/ecm1240-" + ecmData.serialNumber + "/CH" + (x + 1) + "-kWh/config";
      mqttClient.publish(topic.c_str(), payload.c_str(), true);

      
      payload = "{\"name\":\"ECM1240 " + ecmData.serialNumber + " CH" + (x + 1) + " WattSeconds\",\"state_topic\":\"ecm1240-" + ecmData.serialNumber + "/c" + (x + 1) + "/dws\",\"unit_of_measurement\":\"WS\"}";
      topic = "homeassistant/sensor/ecm1240-" + ecmData.serialNumber + "/CH" + (x + 1) + "-WS/config";
      mqttClient.publish(topic.c_str(), payload.c_str(), true);

      
      payload = "{\"name\":\"ECM1240 " + ecmData.serialNumber + " CH" + (x + 1) + " Watts\",\"state_topic\":\"ecm1240-" + ecmData.serialNumber + "/c" + (x + 1) + "/watt\",\"unit_of_measurement\":\"W\"}";
      topic = "homeassistant/sensor/ecm1240-" + ecmData.serialNumber + "/CH" + (x + 1) + "-Watts/config";
      mqttClient.publish(topic.c_str(), payload.c_str(), true);
    }

    // Publish the auto-discovery payload to the MQTT broker

    mqttClient.disconnect();
  }
}

void handleAP() {
  if (!isAuthenticated()) {
    sendLogin(false);
  } else {
    // Root webpage
    String html = "<html><body>";
    html += "<h2>ESP8266 Configuration</h2>";
    html += "<form action='/config'>";
    html += "Select a network: <select name='ssid'>";
    int networksFound = WiFi.scanNetworks();
    for (int i = 0; i < networksFound; i++) {
      html += "<option value='" + String(WiFi.SSID(i)) + "'>" + String(WiFi.SSID(i)) + "</option>";
    }
    html += "</select><br>";
    html += "Or enter SSID: <input type='text' name='custom_ssid'><br>";
    html += "Password: <input type='password' name='password'><br>";
    html += "<input type='submit' value='Login'>";
    html += "</form>";
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
  server.send(200, "text/html", "Configuration saved, rebooting  module, please wait...");

  // Reboot module
  ESP.restart();
}

void handleStationMode() {
  if (!isAuthenticated()) {
    sendLogin(false);
  } else {
    // Station mode webpage
    String html = "<html><head>";
    html += css;
    html += "</head><body>";
    html += "<div><form action='/config'><h3>Network Settings</h3>";
    html += "<h4>Connected to: " + String(WiFi.SSID()) + "</h4>";
    html += "<label>Select a network: <select name='ssid'>";
    int networksFound = WiFi.scanNetworks();
    for (int i = 0; i < networksFound; i++) {
      html += "<option value='" + String(WiFi.SSID(i)) + "'>" + String(WiFi.SSID(i)) + "</option>";
    }
    html += "</select>";
    html += "<label>or enter SSID:</label><input type='text' name='custom_ssid' value='" + String(ssid) + "'>";
    html += "<label>Password:</label><input type='password' name='password' value=''>";
    html += "<button>Submit</button>";
    html += "</form></div>";
    html += "<div><form action='/serial-to-tcp'><h3>Serial to TCP Client</h3>";
    html += "<label>IP address:</label><input type='text' name='ip' value=''>";
    html += "<label>Port:</label><input type='number' name='port' value=''>";
    html += "<button>Connect</button>";
    html += "</form></div>";
    html += "<div><form action='/serial-to-tcp-server'><h3>TCP Server Connection</h3>";
    html += "<label>Port:</label><input type='number' name='port' value='" + String(tcpServerPort) + "'>";
    html += "<button>Save</button>";
    html += "</form></div>";
    html += "<div><form action='/mqtt'><h3>MQTT Server Connection</h3>";
    html += "<label>IP address/Domain:</label><input type='text' name='ip' value='" + String(mqttServer) + "'>";
    html += "<label>Port:</label><input type='number' name='port' value='" + String(mqttPort) + "'>";
    html += "<label>User:</label><input type='text' name='user' value='" + String(mqttUser) + "'>";
    html += "<label>Password:</label><input type='pass' name='pass' value=''>";
    html += "<button>Save</button>";
    html += "</form><form action='/send-ha'><h3>Home-Assistant Config</h3>";
    html += "<button>Send Config</button>";
    html += "</form></div>";
    html += "<div><form action='/login-settings'><h3>Login Information</h3>";
    html += "<label>User:</label><input type='text' name='user' value='" + String(loginUser) + "'>";
    html += "<label>Password:</label><input type='pass' name='pass' value=''>";
    html += "<button>Save</button>";
    html += "</form></div>";
    html += "<div><h3>ECM-1240 Data:</h3><br>Serial: " + String(ecmData.serialNumber) + "  V: " + String(ecmData.voltage) + "  S: " + String(ecmData.seconds) + "  PS: " + String(ecmData.prevSeconds);
    html += "<br><br>Wattseconds:<br>CH1: " + String(ecmData.wattSeconds[0]) + " CH2: " + String(ecmData.wattSeconds[1]);
    html += "<br>CH3: " + String(ecmData.wattSeconds[2]) + " CH4: " + String(ecmData.wattSeconds[3]);
    html += "<br>CH5: " + String(ecmData.wattSeconds[4]) + " CH6: " + String(ecmData.wattSeconds[5]);
    html += "<br>CH7: " + String(ecmData.wattSeconds[6]);
    html += "<br><br>Prev Wattseconds:<br>CH1: " + String(ecmData.prevWattSeconds[0]) + " CH2: " + String(ecmData.prevWattSeconds[1]);
    html += "<br>CH3: " + String(ecmData.prevWattSeconds[2]) + " CH4: " + String(ecmData.prevWattSeconds[3]);
    html += "<br>CH5: " + String(ecmData.prevWattSeconds[4]) + " CH6: " + String(ecmData.prevWattSeconds[5]);
    html += "<br>CH7: " + String(ecmData.prevWattSeconds[6]);
    html += "<br><br>Polarized Wattseconds:<br>CH1: " + String(ecmData.polWattSeconds[0]) + " CH2: " + String(ecmData.polWattSeconds[1]);
    html += "<br><br>Watts:<br>CH1: " + String(ecmData.watts[0]) + " CH2: " + String(ecmData.watts[1]);
    html += "<br>CH3: " + String(ecmData.watts[2]) + " CH4: " + String(ecmData.watts[3]);
    html += "<br>CH5: " + String(ecmData.watts[4]) + " CH6: " + String(ecmData.watts[5]);
    html += "<br>CH7: " + String(ecmData.watts[6]);
    html += "<br><br>Net Watts:<br>CH1: " + String(ecmData.netWatts[0]) + " CH2: " + String(ecmData.netWatts[1]);
    html += "<br><br>kWh:<br>CH1: " + String(ecmData.kwh[0], 5) + " CH2: " + String(ecmData.kwh[1], 5);
    html += "<br>CH3: " + String(ecmData.kwh[2], 5) + " CH4: " + String(ecmData.kwh[3], 5);
    html += "<br>CH5: " + String(ecmData.kwh[4], 5) + " CH6: " + String(ecmData.kwh[5], 5);
    html += "<br>CH7: " + String(ecmData.kwh[6], 5);
    html += "<br><br>Delta Wattsec:<br>CH1: " + String(ecmData.deltaWattSeconds[0], 5) + " CH2: " + String(ecmData.deltaWattSeconds[1], 5);
    html += "<br>CH3: " + String(ecmData.deltaWattSeconds[2], 5) + " CH4: " + String(ecmData.deltaWattSeconds[3], 5);
    html += "<br>CH5: " + String(ecmData.deltaWattSeconds[4], 5) + " CH6: " + String(ecmData.deltaWattSeconds[5], 5);
    html += "<br>CH7: " + String(ecmData.deltaWattSeconds[6], 5);
    html += "</body></html>";
    server.send(200, "text/html", html);
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
  String html = "<!DOCTYPE html><html><head><title>Login</title>";
  html += css;
  html += "</head> <body> <div class='login'> <h1> Login</h1>";


  if (error) {
    html += "<p class='error'>Invalid username or password.</p>";
  }
  html += "<form action='/login' method='post'> <label for='username'>Username:</label> <input type='text' id='username' name='username'> <label for='password'>Password:</label> <input type='password' id='password' name='password'> <button type='submit'>Login</button> </form> </div> </body> </html>";
  server.send(200, "text/html", html);
}

void handleLogin() {
  String username = server.arg("username");
  String password = server.arg("password");

  // Replace with your authentication logic
  if (username == loginUser && password == loginPass) {
    // Set the session cookie and redirect to the dashboard page
    server.sendHeader("Set-Cookie", "session_id=1; HttpOnly");
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

  String html = "<html><body>";

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

  String html = "<html><body>";

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

  String html = "<html><body>";

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

    html += "<h2>Address: " + mqttServer + " Port: " + String(port) + " User: " + String(mqttUser) + " Pass:  " + String(mqttPass) + " saved to EEPROM.</h2>";
  } else {
    html += "<h2>Invalid IP Address or Port</h2>";
  }

  html += "</body></html>";
  server.send(200, "text/html", html);
}

void handleLoginSettings() {
  // Serial to TCP client connection
  String user = server.arg("user");
  String pass = server.arg("pass");

  String html = "<html><body>";

  storeString(user, loginUserAddress);
  storeString(pass, loginPassAddress);

  loginUser = user;
  loginPass = pass;

  html += " User: " + String(loginUser) + " Pass:  " + String(loginPass) + " saved to EEPROM.</h2></body></html>";

  server.send(200, "text/html", html);
}

void storeString(String store, int location) {
  for (int i = 0; i < store.length(); i++) {
    EEPROM.write(location++, store[i]);
  }

  EEPROM.write(location, '\0');  // add null terminator to end of string
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

bool isValidIP(String str) {
  IPAddress ipCheck;
  return ipCheck.fromString(str);
}

bool isAuthenticated() {
  // Check if the session cookie is set and valid
  String cookie = server.header("Cookie");

  if (cookie.indexOf("session_id=1") != -1) {
    return true;
  } else {
    return false;
  }
}
