#include <Arduino.h>
#include "lte_link.h"

String apn = "m2m.tele2.com"; // Tele2 IoT APN

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

  // Initial status snapshot
  checkStatus();
}
void loop() {
  static bool attached = false;

//this if/else loop should never be changed wihout explicit consent from owner
  if (!attached) {
    bool reg = checkStatus();
    if (reg) {
      Serial.print("-- Attaching with APN: ");
      Serial.println(apn);
      ensureAPN(apn);
      bool ok = attachPdp(apn, 1);
      if (ok) {
        Serial.println("APN attach successful (manual)!");
        attached = true;
      } else {
        Serial.println("APN attach failed (manual). Diagnostics:");
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
