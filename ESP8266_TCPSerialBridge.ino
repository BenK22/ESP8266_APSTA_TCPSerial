#include <ESP8266WiFi.h>
#include <EEPROM.h>
#include <ESP8266WebServer.h>

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
const byte isFirstRunValue = 0xAA;

WiFiClient client;

ESP8266WebServer server(80);

String tcpIp = "";
int tcpPort = 0;

void setup() {
  // Start EEPROM
  EEPROM.begin(eepromSize);

  // Start serial port
  Serial.begin(19200);
  Serial.flush();

  // Check if this is the first run
  byte isFirstRun = EEPROM.read(isFirstRunAddress);
  if (isFirstRun != isFirstRunValue) {
    Serial.println("Clearing EEPROM...");
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

  EEPROM.get(tcpPortAddress, tcpPort);

  // Connect to saved network
  if (strcmp(ssid, "") != 0 && strcmp(password, "") != 0) {
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, password);

    int x = 0;

    while (WiFi.status() != WL_CONNECTED) {
      delay(1000);
      Serial.print(".");

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
    server.on("/", handleRoot);
    server.on("/config", handleConfig);
  } else {
    Serial.println(WiFi.localIP());

    server.on("/", handleStationMode);
    server.on("/config", handleConfig);
    server.on("/serial-to-tcp", handleSerialToTcp);
  }

  server.begin();
}

void loop() {
  server.handleClient();

  if (client.connected()) {

    // Handle the data passthru
    if (Serial.available()) {
      while (Serial.available()) {
        client.write(Serial.read());
        delay(5);
      }

      delay(50);
    }

    if (client.available()) {
      while (client.available()) {
        Serial.print((char)client.read());
        delay(5);
      }

      delay(50);
    }
  } else {
    if (!client.connected() && isValidIP(tcpIp) && tcpPort > 1024 && tcpPort < 65536) {
      client.connect(tcpIp.c_str(), tcpPort);
    }
  }

  delay(50);
}

void handleRoot() {
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
  // Station mode webpage
  String html = "<html><body>";
  html += "<h2>ESP8266 Station Mode</h2>";
  html += "<p>Connected to: " + String(WiFi.SSID()) + "</p>";
  html += "<form action='/config'>";
  html += "SSID: <input type='text' name='ssid' value='" + String(ssid) + "'><br>";
  html += "Password: <input type='password' name='password' value='" + String(password) + "'><br>";
  html += "<input type='submit' value='Connect'>";
  html += "</form>";
  html += "<h3>Serial to TCP Client</h3>";
  html += "<form action='/serial-to-tcp'>";
  html += "IP address: <input type='text' name='ip' value=''><br>";
  html += "Port: <input type='number' name='port' value=''><br>";
  html += "<input type='submit' value='Connect'>";
  html += "</form>";
  html += "</body></html>";
  server.send(200, "text/html", html);
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

bool isValidIP(String str) {
  IPAddress ipCheck;
  return ipCheck.fromString(str);
}
