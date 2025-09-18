#include <Arduino.h>
#include <MKRNB.h>
#include <MQTT.h>

NB nbAccess;
NBModem modem;
NBClient client;
MQTTClient mqttClient(128);

String apn = "m2m.tele2.com"; // Tele2 IoT APN

// Helper om AT-commando's te sturen (met ruwe console logging)
String sendAT(const char *cmd, unsigned long timeout = 2500) {
  // Leeg eventuele buffer (URCs)
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
  sendAT("AT+CEREG=4");
  sendAT("AT+CGEREP=2,1");
}

static void ensureAPN(const String &apn) {
  String cgdc = sendAT("AT+CGDCONT?");
  if (cgdc.indexOf(apn) < 0) {
    Serial.print("Setting APN to: "); Serial.println(apn);
    String cmd = String("AT+CGDCONT=1,\"IP\",\"") + apn + "\"";
    sendAT(cmd.c_str());
    sendAT("AT+CGDCONT?");
  }
}

// --- Modem aliveness and manual PDP attach helpers ---
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

static void forceRatWithReboot(int rat) {
  Serial.print("Configuring modem for RAT ");
  Serial.println(rat);

  sendAT("AT+CFUN=0", 4000);
  delay(500);

  String cmd = String("AT+URAT=") + rat;
  sendAT(cmd.c_str(), 4000);
  delay(500);

  Serial.println("Rebooting modem to apply RAT setting...");
  sendAT("AT+CFUN=1,1", 8000);

  // Allow the modem to restart fully
  delay(7000);
  waitModemAlive(15000);

  ensureURCsVerbose();
}

static bool manualPdpAttach(const String &apn, int cid = 1) {
  if (!ensureModemOn()) {
    Serial.println("Failed to ensure modem on");
    return false;
  }
  ensureURCsVerbose();
  ensureAPN(apn);

  // Attach packet domain
  sendAT("AT+CGATT=1", 5000);
  if (!waitForAttach(60000)) {
    Serial.println("Timed out waiting for CGATT:1");
    return false;
  }

  // Activate PDP context
  char cmd[24];
  snprintf(cmd, sizeof(cmd), "AT+CGACT=1,%d", cid);
  sendAT(cmd, 10000);
  if (!waitPdpActive(cid, 60000)) {
    Serial.println("Timed out waiting for CGACT active");
    return false;
  }

  // Show resulting IP parameters
  sendAT("AT+CGPADDR", 3000);
  sendAT("AT+CGCONTRDP", 3000);
  return true;
}

bool checkStatus() {
  bool registered = false;

  // SIM check
  String cpin = sendAT("AT+CPIN?");
  if (cpin.indexOf("READY") >= 0) {
    Serial.println("✅ SIM ready");
  } else {
    Serial.println("❌ SIM status: " + cpin);
  }

  // Signaal check
  String csq = sendAT("AT+CSQ");
  if (csq.indexOf("+CSQ:") >= 0) {
    int comma = csq.indexOf(',');
    int val = csq.substring(csq.indexOf(':') + 1, comma).toInt();
    if (val == 99) {
      Serial.println("❌ No signal");
    } else {
      Serial.print("📶 Signal RSSI index: ");
      Serial.println(val);
    }
  }

  // Registratie check
  String cereg = sendAT("AT+CEREG?");
  if (cereg.indexOf(",1") >= 0) {
    Serial.println("✅ Registered (home)");
    registered = true;
  } else if (cereg.indexOf(",5") >= 0) {
    Serial.println("✅ Registered (roaming)");
    registered = true;
  } else if (cereg.indexOf(",2") >= 0) {
    Serial.println("🔄 Searching network...");
  } else {
    Serial.println("ℹ️ CEREG: " + cereg);
  }

  return registered;
}

void setup() {
  Serial.begin(115200);
  while (!Serial) {}

  Serial.println("=== Tele2 IoT LTE-M Attach Test ===");

  if (!modem.begin()) {
    Serial.println("❌ Modem not responding");
    while (1);
  }

  // Enable library AT debug and verbose URCs
  MODEM.debug(Serial);
  Serial.println("MODEM.debug enabled");
  ensureURCsVerbose();

  // Force LTE-M (URAT=8) with a controlled reboot so the setting sticks
  forceRatWithReboot(8);
  ensureAPN(apn);

  // Check status (loop will handle attach sequencing)
  (void)checkStatus();
  Serial.println("(Info) Attach attempts will start in loop using manual PDP activation.");
}

void loop() {
  static bool attached = false;
  static bool published = false;
  static uint8_t manualAttempts = 0;
  static unsigned long nextManualAttemptMs = 0;

  if (!attached) {
    bool reg = checkStatus();
    if (reg) {
      unsigned long now = millis();
      if (now >= nextManualAttemptMs) {
        manualAttempts++;
        Serial.print("-- Manual attach attempt ");
        Serial.println(manualAttempts);
        ensureAPN(apn);
        bool ok = manualPdpAttach(apn, 1);
        if (ok) {
          Serial.println("APN attach successful (manual)!");
          attached = true;
          manualAttempts = 0;
          nextManualAttemptMs = 0;
        } else {
          Serial.println("APN attach failed (manual). Diagnostics:");
          sendAT("AT+CGATT?");
          sendAT("AT+CGACT?");
          sendAT("AT+CGPADDR");
          sendAT("AT+CGCONTRDP");
          sendAT("AT+CEREG?");
          sendAT("AT+CSQ");
          // Back off before the next attempt to respect network timers (e.g. T3396)
          nextManualAttemptMs = now + 30000UL;
          if (manualAttempts >= 3) {
            Serial.println("Multiple manual failures; reinitialising RAT and modem...");
            forceRatWithReboot(8);
            manualAttempts = 0;
            nextManualAttemptMs = millis() + 10000UL;
          }
        }
      }
    } else {
      Serial.println("Not registered yet; will retry...");
    }

    // Visible retry pattern
    for (int i = 0; i < 5; i++) {
      digitalWrite(LED_BUILTIN, HIGH); delay(120);
      digitalWrite(LED_BUILTIN, LOW);  delay(180);
    }
    delay(2000);
  } else {
    if (!published) {
      Serial.println("Connecting to MQTT broker to publish heartbeat...");
      client.stop();
      mqttClient.begin("mqtt.syquens.com", 1883, client);

      String clientId = String("MKR1500NB-") + String(millis(), HEX);
      if (mqttClient.connect(clientId.c_str())) {
        Serial.println("MQTT connected, publishing message");
        if (mqttClient.publish("/mkr1500nb/live", "hi there")) {
          Serial.println("Message published to /mkr1500nb/live");
        } else {
          Serial.println("Failed to publish message");
        }
        mqttClient.disconnect();
      } else {
        Serial.print("MQTT connect failed, error=");
        Serial.println((int)mqttClient.lastError());
      }
      published = true;
    }

    // Heartbeat when attached
    digitalWrite(LED_BUILTIN, HIGH); delay(40);
    digitalWrite(LED_BUILTIN, LOW);  delay(1960);
  }
}
