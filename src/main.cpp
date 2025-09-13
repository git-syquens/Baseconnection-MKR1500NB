// MKR NB1500: MQTT over WSS, publish RSSI every 30s and print DemoInput every 10s
#include <Arduino.h>
#include <MKRNB.h>
#include <ArduinoHttpClient.h>
#include <ArduinoMqttClient.h>

// Network config
static const char* SIM_PIN  = "";                 // SIM PIN (leave empty if none)
static const char* APN      = "iot.tele2.com";    // Tele2 IoT APN
static const char* APN_USER = "";
static const char* APN_PASS = "";

// MQTT over WebSockets TLS config
static const char* MQTT_HOST = "mqtt.syquens.com";
static const int   MQTT_PORT = 443;               // WSS
static const char* WS_PATH   = "/";              // Broker WebSocket path (root for mqtt.syquens.com)
static const char* TOPIC_RSSI  = "/SQIoT/Demo/MKR1500NB/Network/RSSI";
static const char* TOPIC_INPUT = "/SQIoT/Demo/MKR1500NB/Input/DemoInput";

// Intervals
static const unsigned long PUBLISH_MS = 30000;    // 30s RSSI publish
static const unsigned long PRINT_MS   = 10000;    // 10s print DemoInput value

NB nbAccess;
NBSSLClient tlsClient;                 // TLS socket via the modem
WebSocketClient wsClient(tlsClient, MQTT_HOST, MQTT_PORT);
MqttClient mqttClient(wsClient);

String lastDemoInput;

// Minimal AT helper only for CSQ reading
static String sendAT(const char* cmd, unsigned long timeout = 1500) {
  while (SerialSARA.available()) { SerialSARA.read(); }
  SerialSARA.print(cmd);
  SerialSARA.print("\r");
  String resp;
  unsigned long start = millis();
  while (millis() - start < timeout) {
    while (SerialSARA.available()) {
      resp += (char)SerialSARA.read();
    }
    delay(5);
  }
  return resp;
}

static int readCSQ() {
  String r = sendAT("AT+CSQ", 1200);
  int p = r.indexOf("+CSQ:");
  if (p < 0) return -1;
  int colon = r.indexOf(':', p);
  int comma = r.indexOf(',', colon + 1);
  if (colon < 0 || comma < 0) return -1;
  String valStr = r.substring(colon + 1, comma);
  valStr.trim();
  int val = valStr.toInt();
  return val; // 0..31, 99=unknown
}

static bool readRSSIdBm(int& outDbm) {
  int csq = readCSQ();
  if (csq < 0 || csq == 99) return false;
  outDbm = -113 + 2 * csq; // 3GPP CSQ to dBm mapping
  return true;
}

static void onMqttMessage(int messageSize) {
  // Read all available bytes into payload
  String payload;
  while (mqttClient.available()) {
    payload += (char)mqttClient.read();
  }
  lastDemoInput = payload;
}

static bool ensureMqttConnected() {
  if (mqttClient.connected()) return true;

  if (!wsClient.connected()) {
    if (!wsClient.begin(WS_PATH, "mqtt")) { // request MQTT subprotocol
      Serial.println("WebSocket: begin failed");
      return false;
    }
  }

  // Connect MQTT over the already-open WebSocket
  if (!mqttClient.connect("MKR1500NB")) {
    Serial.print("MQTT connect failed, err=");
    Serial.println(mqttClient.connectError());
    return false;
  }

  mqttClient.onMessage(onMqttMessage);
  mqttClient.subscribe(TOPIC_INPUT);
  Serial.println("MQTT connected");
  return true;
}

void setup() {
  Serial.begin(115200);
  while (!Serial) {}
  Serial.println("MKR NB1500: MQTT over WSS, RSSI publisher");

  // Attach to the cellular network (synchronous)
  NB_NetworkStatus_t st = nbAccess.begin(SIM_PIN, APN, APN_USER, APN_PASS, true, true);
  if (st != NB_READY) {
    Serial.print("Network attach failed, status=");
    Serial.println((int)st);
    while (true) { delay(1000); }
  }
  Serial.println("Network ready");

  ensureMqttConnected();
}

void loop() {
  static unsigned long lastPublish = 0;
  static unsigned long lastPrint = 0;

  // Keep MQTT session alive and process incoming messages
  if (!ensureMqttConnected()) {
    delay(2000);
    return;
  }
  mqttClient.poll();

  unsigned long now = millis();

  // Publish RSSI (dBm) every 30s
  if (now - lastPublish >= PUBLISH_MS) {
    lastPublish = now;
    int dbm;
    String payload;
    if (readRSSIdBm(dbm)) payload = String(dbm);
    else payload = "NA";
    if (mqttClient.beginMessage(TOPIC_RSSI)) {
      mqttClient.print(payload);
      mqttClient.endMessage();
      Serial.print("Published RSSI dBm: ");
      Serial.println(payload);
    } else {
      Serial.println("Publish failed: RSSI");
    }
  }

  // Print last DemoInput value every 10s
  if (now - lastPrint >= PRINT_MS) {
    lastPrint = now;
    Serial.print("DemoInput: ");
    if (lastDemoInput.length()) Serial.println(lastDemoInput);
    else Serial.println("(none)");
  }
}
