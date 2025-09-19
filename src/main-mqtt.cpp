#include <Arduino.h>
#include <MKRNB.h>
#include <MQTT.h>

NB nbAccess;
NBModem modem;
NBClient client;
MQTTClient mqttClient(256);

String apn = "m2m.tele2.com"; // Tele2 IoT APN

// MQTT server parameters (adjust in setup if needed)
String mqttHost = "mqtt.syquens.com";
uint16_t mqttPort = 1883;
String mqttUsername;
String mqttPassword;

// MQTT message bookkeeping
String lastMqttTopic;
String lastMqttPayload;
String lastDemoInput;
String listenTopicActive;
String* listenResultPtr = nullptr;

// Demo publish target
static const char* DEMO_TOPIC   = "/mkr1500nb/live/";
static const char* DEMO_PAYLOAD = "hello again";

// Helper om AT-commando's te sturen (met ruwe console logging)
String sendAT(const char *cmd, unsigned long timeout = 2500) {
  while (SerialSARA.available()) {
    char c = SerialSARA.read();
    Serial.write(c);
  }
  Serial.print("AT> ");
  Serial.println(cmd);
  SerialSARA.print(cmd);
  SerialSARA.print("\r");

  String resp;
  String line;
  unsigned long start = millis();
  while (millis() - start < timeout) {
    while (SerialSARA.available()) {
      char c = SerialSARA.read();
      resp += c;
      if (c == '\r') continue;
      if (c == '\n') {
        if (line.length()) {
          Serial.print("AT< ");
          Serial.println(line);
          line = "";
        }
      } else {
        line += c;
      }
    }
    yield();
  }
  if (line.length()) {
    Serial.print("AT< ");
    Serial.println(line);
  }
  resp.trim();
  return resp;
}

static void ensureURCsVerbose() {
  sendAT("AT+CMEE=2");
  sendAT("AT+CEREG=2");
  sendAT("AT+CGEREP=2,1");
}

static void ensureAPN(const String &apnValue) {
  String cgdc = sendAT("AT+CGDCONT?");
  if (cgdc.indexOf(apnValue) < 0) {
    Serial.print("Setting APN to: "); Serial.println(apnValue);
    String cmd = String("AT+CGDCONT=1,\"IP\",\"") + apnValue + "\"";
    sendAT(cmd.c_str());
    sendAT("AT+CGDCONT?");
  }
}

static bool waitModemAlive(unsigned long timeoutMs = 15000) {
  unsigned long start = millis();
  while (millis() - start < timeoutMs) {
    String r = sendAT("AT", 800);
    if (r.indexOf("OK") >= 0) return true;
    delay(200);
  }
  return false;
}

static bool ensureModemOn() {
  if (waitModemAlive(2000)) return true;
  Serial.println("Modem not responding; powering on via modem.begin()...");
  if (!modem.begin()) return false;
  delay(1000);
  return waitModemAlive(10000);
}

static bool waitForAttach(unsigned long timeoutMs = 30000) {
  unsigned long start = millis();
  while (millis() - start < timeoutMs) {
    String r = sendAT("AT+CGATT?", 1000);
    if (r.indexOf("+CGATT: 1") >= 0) return true;
    delay(500);
  }
  return false;
}

static bool waitPdpActive(int cid = 1, unsigned long timeoutMs = 30000) {
  unsigned long start = millis();
  while (millis() - start < timeoutMs) {
    String r = sendAT("AT+CGACT?", 1000);
    String needle = String("+CGACT: ") + cid + ",1";
    if (r.indexOf(needle) >= 0) return true;
    delay(500);
  }
  return false;
}

static bool attachPdp(const String &apnValue, int cid = 1) {
  if (!ensureModemOn()) {
    Serial.println("Failed to ensure modem on");
    return false;
  }
  ensureURCsVerbose();
  ensureAPN(apnValue);

  sendAT("AT+CGATT=1", 5000);
  if (!waitForAttach(60000)) {
    Serial.println("Timed out waiting for CGATT:1");
    return false;
  }

  char cmd[24];
  snprintf(cmd, sizeof(cmd), "AT+CGACT=1,%d", cid);
  sendAT(cmd, 10000);
  if (!waitPdpActive(cid, 60000)) {
    Serial.println("Timed out waiting for CGACT active");
    return false;
  }

  sendAT("AT+CGPADDR", 3000);
  sendAT("AT+CGCONTRDP", 3000);
  return true;
}

static void onMqttMessage(String &topic, String &payload) {
  lastMqttTopic = topic;
  lastMqttPayload = payload;
  lastDemoInput = payload;
  if (listenResultPtr && topic == listenTopicActive) {
    *listenResultPtr = payload;
    listenResultPtr = nullptr;
    listenTopicActive = "";
  }
}



static bool connectPrimaryMqtt(bool subscribeInput = true) {
  if (mqttClient.connected()) {
    return true;
  }
  client.stop();
  mqttClient.begin(mqttHost.c_str(), mqttPort, client);
  mqttClient.onMessage(onMqttMessage);
  mqttClient.setKeepAlive(60);
  mqttClient.setTimeout(8000);
  String clientId = String("MKR1500NB-") + String(millis(), HEX);
  bool connected = mqttUsername.length()
                    ? mqttClient.connect(clientId.c_str(), mqttUsername.c_str(), mqttPassword.c_str())
                    : mqttClient.connect(clientId.c_str());
  if (!connected) {
    Serial.print("MQTT connect failed; lastError=");
    Serial.print((int)mqttClient.lastError());
    Serial.print(", returnCode=");
    Serial.println((int)mqttClient.returnCode());
    client.stop();
    return false;
  }
  if (subscribeInput) {
    mqttClient.loop();
  }
  return true;
}


bool sendMqtt(const String &topic, const String &payload, bool retain, int qos) {
  if (!connectPrimaryMqtt(false)) {
    return false;
  }
  bool ok = mqttClient.publish(topic, payload, retain, qos);
  if (!ok) {
    Serial.println("MQTT publish failed");
  }
  mqttClient.loop();
  return ok;
}

bool sendMqtt(const String &topic, const String &payload) {
  return sendMqtt(topic, payload, true, 1);
}

bool listenMqtt(const String &topic, String &outValue, unsigned long timeoutMs) {
  if (!connectPrimaryMqtt(false)) {
    return false;
  }
  listenTopicActive = topic;
  listenResultPtr = &outValue;
  outValue = "";
  mqttClient.subscribe(topic);
  unsigned long startWait = millis();
  while (millis() - startWait < timeoutMs) {
    mqttClient.loop();
    if (!listenResultPtr) {
      mqttClient.unsubscribe(topic);
      listenTopicActive = "";
      return true;
    }
    delay(50);
  }
  mqttClient.unsubscribe(topic);
  listenTopicActive = "";
  listenResultPtr = nullptr;
  return false;
}



bool listenMqtt(const String &topic, String &outValue) {
  return listenMqtt(topic, outValue, 10000UL);
}


bool checkStatus() {
  bool registered = false;

  String cpin = sendAT("AT+CPIN?");
  if (cpin.indexOf("READY") >= 0) {
    Serial.println("? SIM ready");
  } else {
    Serial.println("? SIM status: " + cpin);
  }

  String csq = sendAT("AT+CSQ");
  if (csq.indexOf("+CSQ:") >= 0) {
    int comma = csq.indexOf(',');
    int val = csq.substring(csq.indexOf(':') + 1, comma).toInt();
    if (val == 99) {
      Serial.println("? No signal");
    } else {
      Serial.print("?? Signal RSSI index: ");
      Serial.println(val);
    }
  }

  String cereg = sendAT("AT+CEREG?");
  if (cereg.indexOf(",1") >= 0) {
    Serial.println("? Registered (home)");
    registered = true;
  } else if (cereg.indexOf(",5") >= 0) {
    Serial.println("? Registered (roaming)");
    registered = true;
  } else if (cereg.indexOf(",2") >= 0) {
    Serial.println("?? Searching network...");
  } else {
    Serial.println("?? CEREG: " + cereg);
  }

  return registered;
}

void setup() {
  Serial.begin(115200);
  while (!Serial) {}
  pinMode(LED_BUILTIN, OUTPUT);

  Serial.println("=== MKR NB1500 LTE-M + MQTT ===");

  if (!modem.begin()) {
    Serial.println("? Modem not responding");
    while (1) {}
  }

  MODEM.debug(Serial);
  Serial.println("MODEM.debug enabled");
  ensureURCsVerbose();

  Serial.println("Configuring modem for LTE-M...");
  sendAT("AT+CFUN=0");
  delay(500);
  sendAT("AT+URAT=7");
  delay(500);
  sendAT("AT+CFUN=1");
  delay(4000);

  // Initial status snapshot
  checkStatus();
}

void loop() {
  static bool attached = false;
  static bool mqttDemoSent = false;

  if (!attached) {
    bool reg = checkStatus();
    if (reg) {
      Serial.print("-- Attaching with APN: ");
      Serial.println(apn);
      ensureAPN(apn);
      if (attachPdp(apn, 1)) {
        Serial.println("APN attach successful!");
        attached = true;
      } else {
        Serial.println("APN attach failed. Diagnostics:");
        sendAT("AT+CGATT?");
        sendAT("AT+CGACT?");
        sendAT("AT+CGPADDR");
        sendAT("AT+CGCONTRDP");
        sendAT("AT+CEREG?");
        sendAT("AT+CSQ");
      }
    } else {
      Serial.println("Not registered yet; will retry...");
    }

    for (int i = 0; i < 5; ++i) {
      digitalWrite(LED_BUILTIN, HIGH); delay(120);
      digitalWrite(LED_BUILTIN, LOW);  delay(180);
    }
    delay(2000);
    return;
  }

  if (!mqttDemoSent) {
    Serial.println("MQTT: sending demo message...");
    if (sendMqtt(DEMO_TOPIC, DEMO_PAYLOAD)) {
      Serial.println("MQTT: publish success");
    } else {
      Serial.println("MQTT: publish failed");
    }
    mqttDemoSent = true;
  }

  digitalWrite(LED_BUILTIN, HIGH); delay(40);
  digitalWrite(LED_BUILTIN, LOW);  delay(1960);
}
