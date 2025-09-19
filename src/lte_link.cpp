#include "lte_link.h"

NB nbAccess;
NBModem modem;
NBClient client;

String sendAT(const char *cmd, unsigned long timeout) {
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

void ensureURCsVerbose() {
  sendAT("AT+CMEE=2");
  sendAT("AT+CEREG=2");
  sendAT("AT+CGEREP=2,1");
}

void ensureAPN(const String &apn) {
  String cgdc = sendAT("AT+CGDCONT?");
  if (cgdc.indexOf(apn) < 0) {
    Serial.print("Setting APN to: ");
    Serial.println(apn);
    String cmd = String("AT+CGDCONT=1,\"IP\",\"") + apn + "\"";
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

bool attachPdp(const String &apn, int cid) {
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
