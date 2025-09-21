#include <MKRNB.h>
#include <MQTT.h>

NB nbAccess;
NBModem modem;
NBClient client;
MQTTClient mqttClient(256);   // grotere buffer

const char apn[] = "m2m.tele2.com";

void setup() {
  Serial.begin(115200);
  while (!Serial) {}

  Serial.println("Connecting NB...");
  if (!nbAccess.begin("", apn)) {
    Serial.println("NB attach failed");
    while (1);
  }
  Serial.println("NB attach OK");

  mqttClient.begin("mqtt.syquens.com", 1883, client);
  String clientId = "MKR1500NB-" + String(millis(), HEX);
  if (mqttClient.connect(clientId.c_str())) {
    Serial.println("MQTT connect OK");
    mqttClient.publish("MKR1500NB/live", "hello from MKR1500NB");
  } else {
    Serial.print("MQTT connect failed, error=");
    Serial.println((int)mqttClient.lastError());
  }
}

void loop() {
  mqttClient.loop();
  delay(10000);
  mqttClient.publish("/mkr1500nb/live", "heartbeat");
}
