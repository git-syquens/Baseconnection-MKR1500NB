// MKR NB1500: MQTT over TCP (1883), publish RSSI every 30s and print DemoInput every 10s
#include <Arduino.h>
#include <MKRNB.h>
#include <MQTT.h>
#include <ArduinoHttpClient.h>

// Network config (compile-time selectable)
// Define one of these in build flags to choose RAT/APN:
//   -DUSE_LTEM   -> RAT 7 (LTE-M)
//   -DUSE_NBIOT  -> RAT 8 (NB-IoT) [default]
// Optionally override APN via: -DAPN_STR="your.apn.here"

static const char* SIM_PIN  = "";                 // SIM PIN (leave empty if none)

#ifndef RAT_MODE
  #ifdef USE_LTEM
    #define RAT_MODE 7
  #elif defined(USE_NBIOT)
    #define RAT_MODE 8
  #else
    // Default to NB-IoT if nothing specified
    #define RAT_MODE 8
  #endif
#endif

#ifdef APN_STR
  static const char* APN = APN_STR;
#else
  #if RAT_MODE == 7
    // Placeholder LTE-M APN; change via -DAPN_STR if needed
    static const char* APN = "ltem.apn";
  #else
    // Default NB-IoT APN example
    static const char* APN = "nbiot.thingsdata";
  #endif
#endif
static const char* APN_USER = "";
static const char* APN_PASS = "";

// MQTT over TCP config
static const char* MQTT_HOST = "mqtt.syquens.com";
static const int   MQTT_PORT = 1883;              // Plain MQTT
// Public test broker for diagnostics
static const char* TEST_MQTT_HOST = "test.mosquitto.org";
static const int   TEST_MQTT_PORT = 1883;
static const char* TOPIC_RSSI  = "/SQIoT/Demo/MKR1500NB/Network/RSSI";
static const char* TOPIC_INPUT = "/SQIoT/Demo/MKR1500NB/Input/DemoInput";
static const char* TOPIC_STATUS = "/SQIoT/Demo/MKR1500NB/Status/Alive";
static const char* TOPIC_DIAG   = "MKRNB1500/diag/status";  // for test broker

// Intervals
static const unsigned long PUBLISH_MS = 30000;    // 30s RSSI publish
static const unsigned long PRINT_MS   = 10000;    // 10s print DemoInput value

NB nbAccess;
NBClient netClient;                    // Plain TCP via the modem
MQTTClient mqttClient(256);            // 256dpi MQTT client

String lastDemoInput;
String cachedModemId;

// Forward declarations for helpers used below
static String sendAT(const char* cmd, unsigned long timeout = 1500);
static bool readRSSIdBm(int& outDbm);
static String getModemId();
static void onMqttMessage(String &topic, String &payload);

// --- Diagnostics helpers ---
static void printBasicNetworkInfo() {
  int dbm;
  if (readRSSIdBm(dbm)) {
    Serial.print("RSSI dBm: "); Serial.println(dbm);
  } else {
    Serial.println("RSSI dBm: NA");
  }
  String cereg = sendAT("AT+CEREG?");
  Serial.print("CEREG: "); Serial.println(cereg);
  String cops = sendAT("AT+COPS?");
  Serial.print("COPS:  "); Serial.println(cops);
}

static bool resolveHostIP(const char* host, String &outIp) {
  String cmd = String("AT+UDNSRN=0,\"") + host + "\""; // 0: IPv4
  String resp = sendAT(cmd.c_str(), 6000);
  // Response typically includes the IP on a line
  int q1 = resp.indexOf('"');
  int q2 = resp.indexOf('"', q1 + 1);
  if (q1 >= 0 && q2 > q1) {
    outIp = resp.substring(q1 + 1, q2);
  } else {
    // Fallback: take last non-empty line
    resp.trim();
    int nl = resp.lastIndexOf('\n');
    outIp = (nl >= 0) ? resp.substring(nl + 1) : resp;
  }
  outIp.trim();
  return outIp.length() > 0 && outIp != "ERROR";
}

static bool testTcpConnect(const char* host, int port, unsigned long timeoutMs = 10000) {
  NBClient probe;
  unsigned long t0 = millis();
  int ret = probe.connect(host, (uint16_t)port);
  if (ret == 1) { probe.stop(); return true; }
  // Some firmwares may take time; poll connected() briefly
  while (millis() - t0 < timeoutMs && !probe.connected()) { delay(50); }
  bool ok = probe.connected();
  probe.stop();
  return ok;
}

static void runConnectivityDiagnostics() {
  Serial.println("-- Connectivity diagnostics --");
  printBasicNetworkInfo();

  String ip;
  if (resolveHostIP(MQTT_HOST, ip)) {
    Serial.print("DNS "); Serial.print(MQTT_HOST); Serial.print(" -> "); Serial.println(ip);
  } else {
    Serial.print("DNS failed for "); Serial.println(MQTT_HOST);
  }

  bool tcp1883 = testTcpConnect(MQTT_HOST, MQTT_PORT, 8000);
  Serial.print("TCP "); Serial.print(MQTT_HOST); Serial.print(":"); Serial.print(MQTT_PORT);
  Serial.print(" connect: "); Serial.println(tcp1883 ? "OK" : "FAIL");
  Serial.println("-- End diagnostics --");
}

// --- IP data plane diagnostics: HTTP GET and AT ping ---
static bool atTcpProbe(const char* host, int port, unsigned long timeoutMs = 12000) {
  Serial.print("AT TCP probe "); Serial.print(host); Serial.print(":"); Serial.println(port);
  String ip;
  if (!resolveHostIP(host, ip)) {
    Serial.println(" DNS resolve failed");
    return false;
  }
  Serial.print(" IP: "); Serial.println(ip);
  // Create TCP socket (6)
  String r = sendAT("AT+USOCR=6", 2000);
  int sock = -1;
  // Parse last digits in response
  for (int i = r.length()-1; i >= 0; --i) {
    if (isDigit(r[i])) { int j=i; while (j>=0 && isDigit(r[j])) j--; sock = r.substring(j+1, i+1).toInt(); break; }
  }
  if (sock < 0) { Serial.println(" USOCR failed"); return false; }
  String cmd = String("AT+USOCO=") + sock + ",\"" + ip + "\"," + port;
  r = sendAT(cmd.c_str(), timeoutMs);
  bool ok = (r.indexOf("+UUSOCO:") >= 0) || (r.indexOf("OK") >= 0);
  if (!ok) Serial.print(" USOCO resp: "), Serial.println(r);
  // Close socket regardless
  String cls = String("AT+USOCL=") + sock;
  sendAT(cls.c_str(), 2000);
  Serial.print(" Result: "); Serial.println(ok ? "OK" : "FAIL");
  return ok;
}

// Print PDP context, IP and DNS to understand data-plane configuration
static void printPdpContextInfo() {
  Serial.println("-- PDP context info --");
  Serial.println(sendAT("AT+CGATT?", 2000));
  Serial.println(sendAT("AT+CGACT?", 2000));
  Serial.println(sendAT("AT+CGPADDR", 3000));
  Serial.println(sendAT("AT+CGCONTRDP", 4000));
  Serial.println("-- End PDP context info --");
}

static void runIpDataDiagnostics() {
  Serial.println("-- IP data diagnostics --");

  bool ok = atTcpProbe("example.com", 80);
  if (!ok) Serial.println("HTTP port probe failed");

  bool okMqtt = atTcpProbe(MQTT_HOST, MQTT_PORT);
  if (!okMqtt) Serial.println("MQTT port probe failed");

  // AT-level ICMP ping via modem (may be blocked by APN but useful if allowed)
  Serial.println("AT+UPING 8.8.8.8 (2 packets, 4s timeout)...");
  String ping = sendAT("AT+UPING=\"8.8.8.8\",2,32,4000", 15000);
  Serial.println(ping);

  Serial.println("-- End IP data diagnostics --");
}

// Force NB-IoT RAT on the modem if not already selected
// Ensure requested RAT on the modem (LTE-M or NB-IoT)
static void ensureRATMode(int rat) {
  Serial.println("-- RAT check --");
  sendAT("AT+CMEE=2", 800);
  String urat = sendAT("AT+URAT?", 2000);
  Serial.print("URAT?: "); Serial.println(urat);
  bool already = (rat == 7) ? (urat.indexOf("+URAT: 7") >= 0) : (urat.indexOf("+URAT: 8") >= 0);
  if (!already) {
    Serial.print("Setting URAT="); Serial.print(rat); Serial.println(" and rebooting modem...");
    sendAT("AT+CFUN=0", 2000);              // minimum functionality
    String cmd = String("AT+URAT=") + rat;   // 7=LTE-M, 8=NB-IoT
    sendAT(cmd.c_str(), 3000);
    sendAT("AT+CFUN=1,1", 2000);            // full functionality + reset
    // Give modem time to reboot
    delay(6000);
  } else {
    Serial.print("URAT already set to "); Serial.println(rat);
  }
  if (rat == 7) {
    Serial.print("Note: Ensure APN suits LTE-M. Current APN: "); Serial.println(APN);
  }
}

static String getModemId() {
  if (cachedModemId.length()) return cachedModemId;
  String r = sendAT("AT+CGSN", 1500);
  r.trim();
  String digits;
  for (size_t i = 0; i < r.length(); ++i) {
    if (isDigit(r[i])) digits += r[i];
  }
  if (digits.length() >= 6) {
    cachedModemId = digits;
  } else {
    cachedModemId = String("NA") + String((uint32_t)millis(), HEX);
  }
  return cachedModemId;
}

static String makeClientId(const char* prefix) {
  String id = String(prefix) + "-";
  String imei = getModemId();
  if (imei.length() >= 6) id += imei.substring(imei.length() - 6);
  else id += imei;
  id += "-";
  id += String((uint32_t)millis(), HEX);
  return id;
}

static bool connectAndTestMqtt(const char* host, int port, bool subscribeInput, const char* statusTopic) {
  netClient.stop();
  mqttClient.begin(host, port, netClient);
  Serial.print("MQTT begin: "); Serial.print(host); Serial.print(":"); Serial.println(port);
  mqttClient.onMessage(onMqttMessage);
  mqttClient.setKeepAlive(60);
  mqttClient.setTimeout(8000);

  String cid = makeClientId("MKR1500NB");
  Serial.print("CONNECT as "); Serial.println(cid);
  if (!mqttClient.connect(cid.c_str())) {
    Serial.print("MQTT connect failed; lastError=");
    Serial.print((int)mqttClient.lastError());
    Serial.print(", returnCode=");
    Serial.println((int)mqttClient.returnCode());
    return false;
  }

  Serial.println("MQTT CONNECT returned OK; publish test...");
  const char* topic = statusTopic ? statusTopic : TOPIC_STATUS;
  String payload = String("online@"); payload += host;
  if (!mqttClient.publish(topic, payload)) {
    Serial.println("Publish test failed; disconnecting");
    mqttClient.disconnect();
    return false;
  }
  mqttClient.loop();
  delay(50);
  if (!mqttClient.connected()) {
    Serial.println("Disconnected immediately after publish; treating as failure");
    return false;
  }

  if (subscribeInput) mqttClient.subscribe(TOPIC_INPUT);
  Serial.println("MQTT connected (publish OK)");
  return true;
}

// Minimal AT helper only for CSQ reading
static String sendAT(const char* cmd, unsigned long timeout) {
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

static void onMqttMessage(String &topic, String &payload) {
  if (topic == TOPIC_INPUT) {
    lastDemoInput = payload;
  }
}

static bool ensureMqttConnected() {
  if (mqttClient.connected()) return true;

  Serial.println("MQTT diagnostics: trying public test broker first...");
  bool testOk = connectAndTestMqtt(TEST_MQTT_HOST, TEST_MQTT_PORT, false, TOPIC_DIAG);
  if (testOk) {
    Serial.println("Public test broker OK; disconnecting test session");
    mqttClient.disconnect();
  } else {
    Serial.println("Public test broker failed; likely policy/network issue");
  }

  Serial.println("Now connecting to target broker...");
  bool prodOk = connectAndTestMqtt(MQTT_HOST, MQTT_PORT, true, TOPIC_STATUS);
  if (!prodOk) {
    Serial.println("Target broker connection failed after test; will retry later");
  }
  return prodOk;
}

void setup() {
  Serial.begin(115200);
  while (!Serial) {}
  Serial.println("MKR NB1500: MQTT over WSS, RSSI publisher");

  // Ensure modem is in requested RAT before attach
  ensureRATMode(RAT_MODE);

  // Attach to the cellular network (synchronous)
  NB_NetworkStatus_t st = nbAccess.begin(SIM_PIN, APN, APN_USER, APN_PASS, true, true);
  if (st != NB_READY) {
    Serial.print("Network attach failed, status=");
    Serial.println((int)st);
    while (true) { delay(1000); }
  }
  Serial.println("Network ready");
  // One-time connectivity diagnostics before MQTT
  runConnectivityDiagnostics();
  printPdpContextInfo();
  runIpDataDiagnostics();
  Serial.println("Diagnostics done; calling ensureMqttConnected()...");
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
  // service MQTT
  mqttClient.loop();

  unsigned long now = millis();

  // Publish RSSI (dBm) every 30s
  if (now - lastPublish >= PUBLISH_MS) {
    lastPublish = now;
    int dbm;
    String payload;
    if (readRSSIdBm(dbm)) payload = String(dbm);
    else payload = "NA";
    if (mqttClient.publish(TOPIC_RSSI, payload)) {
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
