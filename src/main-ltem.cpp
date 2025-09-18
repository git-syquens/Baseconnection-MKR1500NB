// MKR NB1500: MQTT over TCP (1883), publish RSSI every 30s and print DemoInput every 10s
#include <Arduino.h>
#include <MKRNB.h>
#include <MQTT.h>
#include <ArduinoHttpClient.h>

// Network config (compile-time selectable)
// Optionally override APN via: -DAPN_STR="your.apn.here"

static const int   TARGET_RAT = 8;                 // LTE-M per updated mapping
#ifdef APN_STR
  static const char* APN = APN_STR;
#else
  static const char* APN = "m2m.tele2.com";
#endif

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

NBModem modem;
NBClient netClient;                    // Plain TCP via the modem
MQTTClient mqttClient(256);            // 256dpi MQTT client

String lastDemoInput;
String cachedModemId;

struct RegistrationInfo {
  int stat = -1;
  String tac;
  String ci;
  int act = -1;
};

struct OperatorInfo {
  int mode = -1;
  int format = -1;
  String oper;
  int act = -1;
};

// Forward declarations for helpers used below
static String sendAT(const char* cmd, unsigned long timeout = 2500);
static void ensureURCsVerbose();
static void ensureAPN(const char* apn);
static bool readRSSIdBm(int& outDbm);
// Keep forcing LTE-M mode until the network reports it's active
static void ensureRATMode();

static String getModemId();
static void onMqttMessage(String &topic, String &payload);

static const char* describeRegistrationStat(int stat) {
  switch (stat) {
    case 0: return "not registered";
    case 1: return "registered (home)";
    case 2: return "searching";
    case 3: return "registration denied";
    case 4: return "unknown";
    case 5: return "registered (roaming)";
    case 6: return "registered (SMS only, home)";
    case 7: return "registered (SMS only, roaming)";
    case 8: return "emergency only";
    case 9: return "registered (CSFB not preferred)";
    default: return "unknown";
  }
}

static const char* describeAccessTech(int act) {
  switch (act) {
    case 0: return "GSM";
    case 1: return "GSM Compact";
    case 2: return "UTRAN";
    case 3: return "GSM + EGPRS";
    case 4: return "UTRAN + HSDPA";
    case 5: return "UTRAN + HSUPA";
    case 6: return "UTRAN + HSDPA/HSUPA";
    case 7: return "LTE (legacy mapping)";
    case 8: return "LTE-M (Cat-M1)";
    case 9: return "NB-IoT (Cat-NB1)";
    default: return "unknown";
  }
}

static bool tokenizeCsv(const String& body, String* tokens, size_t maxTokens, int& outCount) {
  outCount = 0;
  String current;
  bool inQuote = false;
  for (size_t i = 0; i < (size_t)body.length(); ++i) {
    char c = body.charAt(i);
    if (c == '\"') {
      inQuote = !inQuote;
      continue;
    }
    if (c == ',' && !inQuote) {
      if (outCount < (int)maxTokens) {
        String tmp = current;
        tmp.trim();
        tokens[outCount++] = tmp;
      }
      current = "";
      continue;
    }
    current += c;
  }
  if (current.length()) {
    if (outCount < (int)maxTokens) {
      String tmp = current;
      tmp.trim();
      tokens[outCount++] = tmp;
    }
  }
  return outCount > 0;
}

static bool parseCeregResponse(const String& resp, RegistrationInfo &info) {
  int marker = resp.indexOf("+CEREG:");
  if (marker < 0) {
    return false;
  }
  int lineEnd = resp.indexOf('\n', marker);
  String line = resp.substring(marker, (lineEnd >= 0) ? lineEnd : resp.length());
  line.replace('\r', ' ');
  int colon = line.indexOf(':');
  if (colon < 0) {
    return false;
  }
  String body = line.substring(colon + 1);
  body.trim();

  String tokens[8];
  int count = 0;
  if (!tokenizeCsv(body, tokens, 8, count) || count < 2) {
    return false;
  }

  info.stat = tokens[1].toInt();
  info.tac  = (count >= 3) ? tokens[2] : "";
  info.ci   = (count >= 4) ? tokens[3] : "";
  info.act  = (count >= 5) ? tokens[4].toInt() : -1;
  return true;
}

static bool parseCopsResponse(const String& resp, OperatorInfo &info) {
  int marker = resp.indexOf("+COPS:");
  if (marker < 0) {
    return false;
  }
  int lineEnd = resp.indexOf('\n', marker);
  String line = resp.substring(marker, (lineEnd >= 0) ? lineEnd : resp.length());
  line.replace('\r', ' ');
  int colon = line.indexOf(':');
  if (colon < 0) {
    return false;
  }
  String body = line.substring(colon + 1);
  body.trim();

  String tokens[6];
  int count = 0;
  if (!tokenizeCsv(body, tokens, 6, count) || count < 1) {
    return false;
  }

  info.mode   = tokens[0].toInt();
  info.format = (count >= 2) ? tokens[1].toInt() : -1;
  info.oper   = (count >= 3) ? tokens[2] : "";
  info.act    = (count >= 4) ? tokens[3].toInt() : -1;
  return true;
}

// --- Diagnostics helpers ---
static void printBasicNetworkInfo() {
  int dbm;
  if (readRSSIdBm(dbm)) {
    Serial.print("RSSI dBm: "); Serial.println(dbm);
  } else {
    Serial.println("RSSI dBm: NA");
  }

  String cereg = sendAT("AT+CEREG?", 4000);
  RegistrationInfo reg;
  if (parseCeregResponse(cereg, reg)) {
    Serial.print("CEREG stat: "); Serial.print(reg.stat); Serial.print(" (");
    Serial.print(describeRegistrationStat(reg.stat)); Serial.println(")");
    if (reg.tac.length()) { Serial.print("  TAC: "); Serial.println(reg.tac); }
    if (reg.ci.length())  { Serial.print("  Cell ID: "); Serial.println(reg.ci); }
    if (reg.act >= 0) {
      Serial.print("  Access tech: ");
      Serial.print(reg.act);
      Serial.print(" (");
      Serial.print(describeAccessTech(reg.act));
      Serial.println(")");
      if (reg.act != -1 && reg.act != TARGET_RAT) {
        Serial.print("  ⚠️ Access-tech mismatch: expected ");
        Serial.print(describeAccessTech(TARGET_RAT));
        Serial.print(", network reports ");
        Serial.println(describeAccessTech(reg.act));
      }
    }
  } else {
    Serial.print("CEREG raw: "); Serial.println(cereg);
  }

  String cops = sendAT("AT+COPS?", 4000);
  OperatorInfo op;
  if (parseCopsResponse(cops, op)) {
    Serial.print("COPS operator: ");
    if (op.oper.length()) Serial.print(op.oper);
    else Serial.print("(unknown)");
    Serial.print(", mode="); Serial.print(op.mode);
    if (op.act >= 0) {
      Serial.print(", act="); Serial.print(op.act);
      Serial.print(" ("); Serial.print(describeAccessTech(op.act)); Serial.print(")");
    }
    Serial.println();
    if (op.act >= 0 && op.act != TARGET_RAT) {
      Serial.print("  ⚠️ Operator reports act ");
      Serial.print(describeAccessTech(op.act));
      Serial.println(" (different from LTE-M lock)");
    }
  } else {
    Serial.print("COPS raw:  "); Serial.println(cops);
  }
}

static bool resolveHostIP(const char* host, String &outIp) {
  String cmd = String("AT+UDNSRN=0,\"") + host + "\""; // 0: IPv4
  String resp = sendAT(cmd.c_str(), 6000);
  if (resp.indexOf("ERROR") >= 0) {
    outIp = "";
    return false;
  }

  int marker = resp.indexOf("+UDNSRN:");
  if (marker >= 0) {
    int firstQuote = resp.indexOf('"', marker);
    int secondQuote = resp.indexOf('"', firstQuote + 1);
    if (firstQuote >= 0 && secondQuote > firstQuote) {
      outIp = resp.substring(firstQuote + 1, secondQuote);
      outIp.trim();
      bool hasDigit = false;
      for (size_t i = 0; i < (size_t)outIp.length(); ++i) {
        if (isDigit(outIp.charAt(i))) {
          hasDigit = true;
          break;
        }
      }
      if (hasDigit && outIp != host) {
        return true;
      }
    }
  }

  outIp = "";
  return false;
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
  bool dnsOk = resolveHostIP(MQTT_HOST, ip);
  Serial.print("DNS  "); Serial.print(MQTT_HOST); Serial.print(" -> ");
  if (dnsOk) {
    Serial.println(ip);
  } else {
    Serial.println("FAILED");
  }

  String testIp;
  bool dnsTestOk = resolveHostIP(TEST_MQTT_HOST, testIp);
  Serial.print("DNS  "); Serial.print(TEST_MQTT_HOST); Serial.print(" -> ");
  if (dnsTestOk) {
    Serial.println(testIp);
  } else {
    Serial.println("FAILED");
  }

  bool tcp1883 = testTcpConnect(MQTT_HOST, MQTT_PORT, 8000);
  Serial.print("TCP  "); Serial.print(MQTT_HOST); Serial.print(":"); Serial.print(MQTT_PORT);
  Serial.print(" -> "); Serial.println(tcp1883 ? "connect OK" : "connect FAIL");

  bool tcpTest = testTcpConnect(TEST_MQTT_HOST, TEST_MQTT_PORT, 8000);
  Serial.print("TCP  "); Serial.print(TEST_MQTT_HOST); Serial.print(":"); Serial.print(TEST_MQTT_PORT);
  Serial.print(" -> "); Serial.println(tcpTest ? "connect OK" : "connect FAIL");

  Serial.println("-- End connectivity diagnostics --");
}

// --- Manual modem/PDP helpers (preferred attach path) ---
static bool waitModemAlive(unsigned long timeoutMs = 15000) {
  unsigned long start = millis();
  while (millis() - start < timeoutMs) {
    String r = sendAT("AT", 800);
    if (r.indexOf("OK") >= 0) {
      return true;
    }
    delay(200);
  }
  return false;
}

static bool ensureModemOn() {
  if (waitModemAlive(2000)) {
    return true;
  }
  Serial.println("Modem not responding; powering on via modem.begin()...");
  if (!modem.begin()) {
    return false;
  }
  delay(1000);
  return waitModemAlive(10000);
}

static bool waitForAttach(unsigned long timeoutMs = 30000) {
  unsigned long start = millis();
  while (millis() - start < timeoutMs) {
    String r = sendAT("AT+CGATT?", 1000);
    if (r.indexOf("+CGATT: 1") >= 0) {
      return true;
    }
    delay(500);
  }
  return false;
}

static bool waitPdpActive(int cid = 1, unsigned long timeoutMs = 30000) {
  unsigned long start = millis();
  while (millis() - start < timeoutMs) {
    String r = sendAT("AT+CGACT?", 1000);
    String needle = String("+CGACT: ") + cid + ",1";
    if (r.indexOf(needle) >= 0) {
      return true;
    }
    delay(500);
  }
  return false;
}

static bool manualPdpAttach(const char* apn, int cid = 1) {
  if (!ensureModemOn()) {
    Serial.println("Failed to ensure modem on");
    return false;
  }

  ensureURCsVerbose();
  ensureAPN(apn);

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
  if (r.indexOf("ERROR") >= 0) {
    ok = false;
  }
  if (!ok) Serial.print(" USOCO resp: "), Serial.println(r);
  // Close socket regardless
  String cls = String("AT+USOCL=") + sock;
  sendAT(cls.c_str(), 2000);
  Serial.print(" Result: "); Serial.println(ok ? "OK" : "FAIL");
  return ok;
}

static bool httpGetProbe(const char* host, int port, const char* path, int &statusCode, int &bytesRead) {
  NBClient socket;
  HttpClient http(socket, host, port);

  statusCode = 0;
  bytesRead = 0;

  int err = http.get(path);
  if (err != 0) {
    statusCode = err;
    http.stop();
    return false;
  }

  statusCode = http.responseStatusCode();
  if (statusCode <= 0) {
    http.stop();
    return false;
  }

  http.skipResponseHeaders();

  unsigned long lastData = millis();
  while (http.connected()) {
    while (http.available()) {
      http.read();
      ++bytesRead;
      lastData = millis();
    }
    if (millis() - lastData > 2000) {
      break;
    }
    delay(10);
  }

  http.stop();
  return true;
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

  int httpStatus = 0;
  int httpBytes = 0;
  bool httpOk = httpGetProbe("example.com", 80, "/", httpStatus, httpBytes);
  Serial.print("HTTP GET example.com -> ");
  if (httpOk) {
    Serial.print("status "); Serial.print(httpStatus);
    Serial.print(", bytes downlink="); Serial.println(httpBytes);
  } else {
    Serial.print("FAILED (code="); Serial.print(httpStatus); Serial.println(")");
  }

  // AT-level ICMP ping via modem (may be blocked by APN but useful if allowed)
  Serial.println("AT+UPING 8.8.8.8 (2 packets, 4s timeout)...");
  String ping = sendAT("AT+UPING=\"8.8.8.8\",2,32,4000", 15000);
  Serial.println(ping);

  Serial.println("-- End IP data diagnostics --");
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

      if (c == '\r') {
        continue;
      }

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

static void ensureAPN(const char* apn) {
  String cgdc = sendAT("AT+CGDCONT?");
  if (cgdc.indexOf(apn) < 0) {
    Serial.print("Setting APN to: ");
    Serial.println(apn);
    String cmd = String("AT+CGDCONT=1,\"IP\",\"") + apn + "\"";
    sendAT(cmd.c_str());
    sendAT("AT+CGDCONT?");
  }
}

static void ensureRATMode() {
  Serial.print("-- RAT configuration (target ");
  Serial.print(TARGET_RAT);
  Serial.println(") --");
  ensureURCsVerbose();

  for (int attempt = 1; attempt <= 3; ++attempt) {
    String urat = sendAT("AT+URAT?", 2000);
    Serial.print("URAT? => ");
    Serial.println(urat);

    bool correctSetting = (urat.indexOf(String("+URAT: ") + TARGET_RAT) >= 0);
    if (correctSetting) {
      Serial.print("URAT already set to ");
      Serial.println(TARGET_RAT);
      break;
    }

    Serial.print("Setting URAT=");
    Serial.print(TARGET_RAT);
    Serial.print(" (attempt ");
    Serial.print(attempt);
    Serial.println(")");

    sendAT("AT+CFUN=0", 3000);
    delay(500);

    String cmd = String("AT+URAT=") + TARGET_RAT;
    sendAT(cmd.c_str(), 3000);
    delay(500);

    sendAT("AT+CFUN=1", 3000);
    delay(4000);

    if (attempt == 3) {
      Serial.println("Warning: URAT setting did not confirm after 3 attempts");
    }
  }

  String finalUrat = sendAT("AT+URAT?", 2000);
  Serial.print("URAT? (final) => ");
  Serial.println(finalUrat);

  String cops = sendAT("AT+COPS?", 4000);
  Serial.print("COPS? => ");
  Serial.println(cops);

  Serial.print("Requested LTE-M APN: ");
  Serial.println(APN);
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

  if (!modem.begin()) {
    Serial.println("Modem begin failed");
    while (true) { delay(1000); }
  }

  MODEM.debug(Serial);
  Serial.println("MODEM.debug enabled");

  ensureURCsVerbose();
  ensureAPN(APN);
  ensureRATMode();

  Serial.println("Bringing up data session via manual attach...");
  if (!manualPdpAttach(APN, 1)) {
    Serial.println("Manual PDP attach failed; halting");
    while (true) { delay(1000); }
  }
  Serial.println("Network ready (manual attach)");

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

