#include <Arduino.h>
#include <MKRNB.h>

NB nbAccess;
NBModem modem;
NBClient client;
NB_SMS sms;

String apn = "iot.tele2.com"; // Tele2 IoT APN
static const bool VERBOSE_AT = true; // zet op true voor uitgebreide logging
// SMS configuratie: pas nummer en tekst aan
String smsNumber = "number";               // NL 06-nummer
String smsMessage = "Hallo vanaf MKR NB1500";  // Berichttekst

// Helper om AT-commando's te sturen (met ruwe console logging)
String sendAT(const char *cmd, unsigned long timeout = 2500) {
  // Leeg eventuele buffer (URCs)
  while (SerialSARA.available()) {
    char c = SerialSARA.read();
    if (VERBOSE_AT) Serial.write(c);
  }
  if (VERBOSE_AT) {
    Serial.print("AT> ");
    Serial.println(cmd);
  }
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
          if (VERBOSE_AT) {
            Serial.print("AT< ");
            Serial.println(line);
          }
          line = "";
        }
      } else {
        line += c;
      }
    }
    yield();
  }
  if (line.length() && VERBOSE_AT) {
    Serial.print("AT< ");
    Serial.println(line);
  }
  resp.trim();
  return resp;
}

static void ensureURCsVerbose() {
  if (VERBOSE_AT) {
    sendAT("AT+CMEE=2");
    sendAT("AT+CEREG=2");
    sendAT("AT+CGEREP=2,1");
  } else {
    sendAT("AT+CMEE=1");
    sendAT("AT+CEREG=0");
  }
}

static void initSmsStack() {
  // Text mode SMS and character set; show SMSC for diagnostics
  sendAT("AT+CMGF=1");
  sendAT("AT+CSCS=\"GSM\"");
  if (VERBOSE_AT) {
    sendAT("AT+CSMS?", 2000);
    sendAT("AT+CSCA?", 2000);
  }
}

static void ensureAPN(const String &apn) {
  String cgdc = sendAT("AT+CGDCONT?");
  if (cgdc.indexOf(apn) < 0) {
    if (VERBOSE_AT) { Serial.print("Setting APN to: "); Serial.println(apn); }
    String cmd = String("AT+CGDCONT=1,\"IP\",\"") + apn + "\"";
    sendAT(cmd.c_str());
    if (VERBOSE_AT) sendAT("AT+CGDCONT?");
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
  if (VERBOSE_AT) Serial.println("Modem not responding; powering on via modem.begin()...");
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
    if (VERBOSE_AT) Serial.println("Timed out waiting for CGATT:1");
    return false;
  }

  // Activate PDP context
  char cmd[24];
  snprintf(cmd, sizeof(cmd), "AT+CGACT=1,%d", cid);
  sendAT(cmd, 10000);
  if (!waitPdpActive(cid, 60000)) {
    if (VERBOSE_AT) Serial.println("Timed out waiting for CGACT active");
    return false;
  }

  // Show resulting IP parameters
  if (VERBOSE_AT) {
    sendAT("AT+CGPADDR", 3000);
    sendAT("AT+CGCONTRDP", 3000);
  }
  return true;
}

static String normalizeNLNumber(const String &num) {
  String n = num; n.trim();
  if (n.startsWith("+")) return n;
  if (n.startsWith("06")) {
    String rest = n.substring(1);
    return String("+31") + rest;
  }
  return n;
}

static String getLocalIPOnce() {
  String r = sendAT("AT+CGPADDR", 2000);
  int q1 = r.indexOf('"');
  int q2 = r.indexOf('"', q1 + 1);
  if (q1 >= 0 && q2 > q1) {
    return r.substring(q1 + 1, q2);
  }
  r = sendAT("AT+CGCONTRDP", 2000);
  q1 = r.indexOf('"'); q2 = r.indexOf('"', q1 + 1);
  if (q1 >= 0 && q2 > q1) return r.substring(q1 + 1, q2);
  return String("");
}

static bool checkStatusMinimal() {
  String cereg = sendAT("AT+CEREG?");
  if (cereg.indexOf(",1") >= 0) return true;
  if (cereg.indexOf(",5") >= 0) return true;
  return false;
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
  pinMode(LED_BUILTIN, OUTPUT);
  Serial.println("Boot: MKR NB1500");

  if (!modem.begin()) {
    Serial.println("❌ Modem not responding");
    while (1);
  }

  // Enable library AT debug en URCs
  if (VERBOSE_AT) {
    MODEM.debug(Serial);
    Serial.println("MODEM.debug enabled");
  }
  ensureURCsVerbose();

  // Forceer LTE-M (URAT=7)
  sendAT("AT+CFUN=0");
  delay(300);
  sendAT("AT+URAT=7");
  delay(300);
  sendAT("AT+CFUN=1");
  delay(3000);

  // Check status
  bool reg = checkStatus();
  // Skip library attach in setup; use manual attach in loop
  Serial.println("(Info) Skipping nbAccess.begin in setup; using manual attach in loop.");
  return;

  // Probeer APN attach
  if (reg) {
    Serial.print("➡️ Attaching with APN: ");
    Serial.println(apn);
    int status = nbAccess.begin(apn.c_str(), "", "");
    if (status == NB_READY) {
      Serial.println("✅ APN attach successful!");
      String pdp = sendAT("AT+CGPADDR");
      Serial.println("PDP context: " + pdp);
    } else {
      Serial.print("❌ APN attach failed, status=");
      Serial.println(status);
    }
  } else {
    Serial.println("⚠️ Not registered, skipping APN attach");
  }
}

void loop() {
  static bool attached = false;

  if (!attached) {
    bool reg = checkStatusMinimal();
    if (reg) {
      Serial.print("Attach: "); Serial.println(apn);
      ensureAPN(apn);
      bool ok = manualPdpAttach(apn, 1);
      if (ok) {
        String ip = getLocalIPOnce();
        if (ip.length()) Serial.println(String("IP: ") + ip);
        else Serial.println("IP: (unknown)");
        attached = true;
        static bool smsSent = false;
        if (!smsSent) {
          // Ensure NB library state is initialized for SMS without modem restart
          NB_NetworkStatus_t st = nbAccess.begin(0, false, true);
          if (VERBOSE_AT) {
            Serial.print("NB.begin status: "); Serial.println((int)st);
          }
          initSmsStack();
          String e164 = normalizeNLNumber(smsNumber);
          Serial.print("SMS: sturen naar "); Serial.println(e164);
          if (sms.beginSMS(e164.c_str())) {
            sms.print(smsMessage);
            if (sms.endSMS()) {
              Serial.println("SMS: OK");
            } else {
              Serial.println("SMS: FAILED (end)");
              if (VERBOSE_AT) sendAT("AT+CEER", 2000);
            }
          } else {
            Serial.println("SMS: FAILED (begin)");
            if (VERBOSE_AT) sendAT("AT+CEER", 2000);
          }
          smsSent = true;
        }
      } else {
        Serial.println("Attach: FAILED");
      }
    } else {
      // wachten op registratie, geen verbose meldingen
    }

    // Visible retry pattern
    for (int i = 0; i < 5; i++) {
      digitalWrite(LED_BUILTIN, HIGH); delay(120);
      digitalWrite(LED_BUILTIN, LOW);  delay(180);
    }
    delay(2000);
  } else {
    // Heartbeat when attached
    digitalWrite(LED_BUILTIN, HIGH); delay(40);
    digitalWrite(LED_BUILTIN, LOW);  delay(1960);
  }
}
