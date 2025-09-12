#include <Arduino.h>
#include <MKRNB.h>

NB nbAccess;
NBModem modem;
NBClient client;

String apn = "iot.tele2.com"; // Tele2 IoT APN

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
  sendAT("AT+CEREG=2");
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

  // Forceer LTE-M (URAT=7)
  Serial.println("Configuring modem for LTE-M...");
  sendAT("AT+CFUN=0");
  delay(500);
  sendAT("AT+URAT=7");
  delay(500);
  sendAT("AT+CFUN=1");
  delay(4000);

  // Check status
  bool reg = checkStatus();

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
    bool reg = checkStatus();
    if (reg) {
      Serial.print("-- Attaching with APN: ");
      Serial.println(apn);
      ensureAPN(apn);
      // Pre-attach diagnostics
      sendAT("AT+CGATT?");
      sendAT("AT+CGACT?");
      sendAT("AT+CGPADDR");
      sendAT("AT+CGCONTRDP");

      int status = nbAccess.begin(apn.c_str(), "", "");
      if (status == NB_READY) {
        Serial.println("APN attach successful!");
        // Post-attach diagnostics
        sendAT("AT+CGPADDR");
        sendAT("AT+CGCONTRDP");
        attached = true;
      } else {
        Serial.print("APN attach failed, status=");
        Serial.println(status);
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
