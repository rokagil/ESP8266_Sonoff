/*
  Based heavily on:
  https://github.com/mertenats/sonoff
*/

#include <ESP8266WiFi.h>    // https://github.com/esp8266/Arduino
#include <WiFiManager.h>    // https://github.com/tzapu/WiFiManager
#include <PubSubClient.h>   // https://github.com/knolleary/pubsubclient/releases/tag/v2.6
#include <Ticker.h>
#include <EEPROM.h>

#define           DEBUG // enable debugging

// macros for debugging
#ifdef            DEBUG
  #define         DEBUG_PRINT(x)    Serial.print(x)
  #define         DEBUG_PRINTLN(x)  Serial.println(x)
#else
  #define         DEBUG_PRINT(x)
  #define         DEBUG_PRINTLN(x)
#endif



// Sonoff properties
const uint8_t     BUTTON_PIN = 0;
const uint8_t     RELAY_PIN  = 12;
const uint8_t     LED_PIN    = 13;



#define STRUCT_CHAR_ARRAY_SIZE 32 // size of the arrays for MQTT username, password, etc.

// MQTT
char              MQTT_CLIENT_ID[11]                                 = {0};
char              MQTT_SWITCH_STATE_TOPIC[STRUCT_CHAR_ARRAY_SIZE]   = {0};
char              MQTT_SWITCH_COMMAND_TOPIC[STRUCT_CHAR_ARRAY_SIZE] = {0};
char              MQTT_SWITCH_WILL_BIRTH_TOPIC[STRUCT_CHAR_ARRAY_SIZE] = {0};
const char*       MQTT_SWITCH_ON_PAYLOAD                            = "ON";
const char*       MQTT_SWITCH_OFF_PAYLOAD                           = "OFF";
const char*       MQTT_BIRTH_PAYLOAD                                = "Connected";
const char*       MQTT_WILL_PAYLOAD                                 = "Disconnected";

// Settings for MQTT
typedef struct {
  char            mqttServer[STRUCT_CHAR_ARRAY_SIZE]                = "";//{0};
  char            mqttPort[6]                                       = "";//{0};
} Settings;

enum CMD {
  CMD_NOT_DEFINED,
  CMD_PIR_STATE_CHANGED,
  CMD_BUTTON_STATE_CHANGED,
};
volatile uint8_t cmd = CMD_NOT_DEFINED;

uint8_t           relayState                                        = HIGH;  // HIGH: closed switch
uint8_t           buttonState                                       = HIGH; // HIGH: opened switch
uint8_t           currentButtonState                                = buttonState;
long              buttonStartPressed                                = 0;
long              buttonDurationPressed                             = 0;

Settings          settings;
Ticker            ticker;
WiFiClient        wifiClient;
PubSubClient      mqttClient(wifiClient);

///////////////////////////////////////////////////////////////////////////
//   MQTT
///////////////////////////////////////////////////////////////////////////
/*
   Function called when a MQTT message arrived
   @param p_topic   The topic of the MQTT message
   @param p_payload The payload of the MQTT message
   @param p_length  The length of the payload
*/
void callback(char* p_topic, byte* p_payload, unsigned int p_length) {
  // handle the MQTT topic of the received message
  if (String(MQTT_SWITCH_COMMAND_TOPIC).equals(p_topic)) {
    if ((char)p_payload[0] == (char)MQTT_SWITCH_ON_PAYLOAD[0] && (char)p_payload[1] == (char)MQTT_SWITCH_ON_PAYLOAD[1]) {
      if (relayState != HIGH) {
        relayState = HIGH;
        setRelayState();
      }
    } else if ((char)p_payload[0] == (char)MQTT_SWITCH_OFF_PAYLOAD[0] && (char)p_payload[1] == (char)MQTT_SWITCH_OFF_PAYLOAD[1] && (char)p_payload[2] == (char)MQTT_SWITCH_OFF_PAYLOAD[2]) {
      if (relayState != LOW) {
        relayState = LOW;
        setRelayState();
      }
    }
  }
}

/*
  Function called to publish the state of the Sonoff relay
*/
void publishSwitchState() {
  if (relayState == HIGH) {
    if (mqttClient.publish(MQTT_SWITCH_STATE_TOPIC, MQTT_SWITCH_ON_PAYLOAD, true)) {
      DEBUG_PRINT(F("INFO: MQTT message publish succeeded. Topic: "));
      DEBUG_PRINT(MQTT_SWITCH_STATE_TOPIC);
      DEBUG_PRINT(F(". Payload: "));
      DEBUG_PRINTLN(MQTT_SWITCH_ON_PAYLOAD);
    } else {
      DEBUG_PRINTLN(F("ERROR: MQTT message publish failed, either connection lost, or message too large"));
    }
  } else {
    if (mqttClient.publish(MQTT_SWITCH_STATE_TOPIC, MQTT_SWITCH_OFF_PAYLOAD, true)) {
      DEBUG_PRINT(F("INFO: MQTT message publish succeeded. Topic: "));
      DEBUG_PRINT(MQTT_SWITCH_STATE_TOPIC);
      DEBUG_PRINT(F(". Payload: "));
      DEBUG_PRINTLN(MQTT_SWITCH_OFF_PAYLOAD);
    } else {
      DEBUG_PRINTLN(F("ERROR: MQTT message publish failed, either connection lost, or message too large"));
    }
  }
}

/*
  Function called to connect/reconnect to the MQTT broker
*/
void reconnect() {
  //uint8_t i = 0;
  while (!mqttClient.connected()) {
    if (mqttClient.connect(MQTT_CLIENT_ID, MQTT_SWITCH_WILL_BIRTH_TOPIC, MQTTQOS1, true, MQTT_WILL_PAYLOAD)) {
      DEBUG_PRINTLN(F("INFO: The client is successfully connected to the MQTT broker"));
    } else {
      DEBUG_PRINTLN(F("ERROR: The connection to the MQTT broker failed"));
      DEBUG_PRINT(F("Broker: "));
      DEBUG_PRINTLN(settings.mqttServer);
      /*
            delay(1000);
            if (i == 3) {
              reset();
            }
            i++;
      */
      // Wait 5 seconds before retrying
      delay(5000);
    }
  }

  if (mqttClient.publish(MQTT_SWITCH_WILL_BIRTH_TOPIC, MQTT_BIRTH_PAYLOAD, true)) {
    DEBUG_PRINT(F("INFO: MQTT birth message publish succeeded. Topic: "));
    DEBUG_PRINT(MQTT_SWITCH_WILL_BIRTH_TOPIC);
    DEBUG_PRINT(F(". Payload: "));
    DEBUG_PRINTLN("Connected");
  } else {
    DEBUG_PRINTLN(F("ERROR: MQTT birth message publish failed, either connection lost, or message too large"));
  }

  if (mqttClient.subscribe(MQTT_SWITCH_COMMAND_TOPIC)) {
    DEBUG_PRINT(F("INFO: Sending the MQTT subscribe succeeded. Topic: "));
    DEBUG_PRINTLN(MQTT_SWITCH_COMMAND_TOPIC);
  } else {
    DEBUG_PRINT(F("ERROR: Sending the MQTT subscribe failed. Topic: "));
    DEBUG_PRINTLN(MQTT_SWITCH_COMMAND_TOPIC);
  }
}

///////////////////////////////////////////////////////////////////////////
//   WiFiManager
///////////////////////////////////////////////////////////////////////////
/*
  Function called to toggle the state of the LED
*/
void tick() {
  digitalWrite(LED_PIN, !digitalRead(LED_PIN));
}

// flag for saving data
bool shouldSaveConfig = false;

// callback notifying us of the need to save config
void saveConfigCallback () {
  shouldSaveConfig = true;
}

void configModeCallback (WiFiManager *myWiFiManager) {
  ticker.attach(0.2, tick);
}


///////////////////////////////////////////////////////////////////////////
//   Sonoff switch
///////////////////////////////////////////////////////////////////////////
/*
  Function called to set the state of the relay
*/
void setRelayState() {
  digitalWrite(RELAY_PIN, relayState);
  digitalWrite(LED_PIN, (relayState + 1) % 2);
  publishSwitchState();
}

/*
  Function called to restart the switch
*/
void restart() {
  DEBUG_PRINTLN(F("INFO: Restart..."));
  ESP.reset();
  delay(1000);
}

/*
  Function called to reset the configuration of the switch
*/
void reset() {
  DEBUG_PRINTLN(F("INFO: Reset..."));
  WiFi.disconnect();
  delay(1000);
  ESP.reset();
  delay(1000);
}

///////////////////////////////////////////////////////////////////////////
//   ISR
///////////////////////////////////////////////////////////////////////////
/*
  Function called when the button is pressed/released
*/
void buttonStateChangedISR() {
  cmd = CMD_BUTTON_STATE_CHANGED;
}


///////////////////////////////////////////////////////////////////////////
//   Setup() and loop()
///////////////////////////////////////////////////////////////////////////
void setup() {
#ifdef DEBUG
  Serial.begin(115200);
#endif

  // init the I/O
  pinMode(LED_PIN,    OUTPUT);
  pinMode(RELAY_PIN,  OUTPUT);
  pinMode(BUTTON_PIN, INPUT);
  attachInterrupt(BUTTON_PIN, buttonStateChangedISR, CHANGE);
  ticker.attach(0.6, tick);

  // get the Chip ID of the switch and use it as the MQTT client ID
  sprintf(MQTT_CLIENT_ID, "ESP_%06X", ESP.getChipId());
  //sprintf(MQTT_CLIENT_ID, "%s", WiFi.hostname().c_str());
  DEBUG_PRINT(F("INFO: MQTT client ID/Hostname: "));
  DEBUG_PRINTLN(MQTT_CLIENT_ID);

  // set the state topic: <Chip ID>/switch/state
  sprintf(MQTT_SWITCH_STATE_TOPIC, "ESP_%06X/switch/state", ESP.getChipId());
  //sprintf(MQTT_SWITCH_STATE_TOPIC, "%s/switch/state", WiFi.hostname().c_str());
  DEBUG_PRINT(F("INFO: MQTT state topic: "));
  DEBUG_PRINTLN(MQTT_SWITCH_STATE_TOPIC);

  // set the command topic: <Chip ID>/switch/switch
  sprintf(MQTT_SWITCH_COMMAND_TOPIC, "ESP_%06X/switch/switch", ESP.getChipId());
  //sprintf(MQTT_SWITCH_COMMAND_TOPIC, "%s/switch/switch", WiFi.hostname().c_str());
  DEBUG_PRINT(F("INFO: MQTT command topic: "));
  DEBUG_PRINTLN(MQTT_SWITCH_COMMAND_TOPIC);

  // set the will/birth topic
  sprintf(MQTT_SWITCH_WILL_BIRTH_TOPIC, "ESP_%06X/connection/status", ESP.getChipId());
  //sprintf(MQTT_SWITCH_WILL_BIRTH_TOPIC, "%s/connection/status", WiFi.hostname().c_str());

  // load custom params
  EEPROM.begin(512);
  EEPROM.get(0, settings);
  EEPROM.end();

  WiFiManagerParameter custom_text("<p>MQTT broker IP address and broker port</p>");
  WiFiManagerParameter custom_mqtt_server("mqtt-server", "MQTT Broker IP", settings.mqttServer, STRUCT_CHAR_ARRAY_SIZE);
  WiFiManagerParameter custom_mqtt_port("mqtt-port", "MQTT Broker Port", settings.mqttPort, 6);

  WiFiManager wifiManager;

  wifiManager.addParameter(&custom_text);
  wifiManager.addParameter(&custom_mqtt_server);
  wifiManager.addParameter(&custom_mqtt_port);

  wifiManager.setAPCallback(configModeCallback);
  wifiManager.setConfigPortalTimeout(180);
  // set config save notify callback
  wifiManager.setSaveConfigCallback(saveConfigCallback);

  if (!wifiManager.autoConnect(MQTT_CLIENT_ID)) {
    ESP.reset();
    delay(1000);
  }


  if (shouldSaveConfig) {
    strcpy(settings.mqttServer,   custom_mqtt_server.getValue());
    strcpy(settings.mqttPort,     custom_mqtt_port.getValue());

    EEPROM.begin(512);
    EEPROM.put(0, settings);
    EEPROM.end();
  }

  // configure MQTT
  mqttClient.setServer(settings.mqttServer, atoi(settings.mqttPort));
  mqttClient.setCallback(callback);

  // connect to the MQTT broker
  reconnect();

  ticker.detach();

  setRelayState();
}

void loop() {

  switch (cmd) {
    case CMD_NOT_DEFINED:
      // do nothing
      break;
    case CMD_BUTTON_STATE_CHANGED:
      currentButtonState = digitalRead(BUTTON_PIN);
      if (buttonState != currentButtonState) {
        // tests if the button is released or pressed
        if (buttonState == LOW && currentButtonState == HIGH) {
          buttonDurationPressed = millis() - buttonStartPressed;
          if (buttonDurationPressed < 500) {
            relayState = relayState == HIGH ? LOW : HIGH;
            setRelayState();
          } else if (buttonDurationPressed < 3000) {
            restart();
          } else {
            reset();
          }
        } else if (buttonState == HIGH && currentButtonState == LOW) {
          buttonStartPressed = millis();
        }
        buttonState = currentButtonState;
      }
      cmd = CMD_NOT_DEFINED;
      break;
  }

  yield();

  // keep the MQTT client connected to the broker
  if (!mqttClient.connected()) {
    reconnect();
  }
  mqttClient.loop();

  yield();
}

