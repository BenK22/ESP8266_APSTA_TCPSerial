# ESP8266_APSTA_TCPSerial

Arduino F/W for the ESP8266, starts up in AP, allows you to connect to a network, and provides a Serial-to-TCP Client connection.  All browser configurable.

Added ECM-1240 capability:

- Processes packets.
- Sends data (watts, kwh, dws, volts) to an MQTT server.
- One-click config to have those values as entities in Home-Assistant.

Coming soon:
- ECM-1240 configuration thru the module.
