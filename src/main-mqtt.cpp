#include <MKRNB.h>
#include <MQTT.h>
#include <NBScanner.h>
#include <time.h>
#include <string.h>

NB nbAccess;
NBModem modem;
NBClient client;
MQTTClient mqttClient(256);

const char apn[] = "m2m.tele2.com";
const char mqttServer[] = "mqtt.syquens.com";
const uint16_t mqttPort = 1883;
const char mqttUsername[] = "";
const char mqttPassword[] = "";
const char startTopic[] = "MKR1500NB/Status/StartSession";
const char connectionSqTopic[] = "MKR1500NB/Status/ConnectionSQ";
const unsigned long kSignalPublishIntervalMs = 30000;
const int timezoneStandardOffsetHours = 1;  // CET (Amsterdam standard time)
const int timezoneDstOffsetHours = 2;       // CEST (Amsterdam daylight time)
const long manualEpochOffsetSeconds = 2 * 3600L;  // Adjust for cellular network time drift (2h)

namespace {
struct DateTime {
  int year;
  int month;
  int day;
  int hour;
  int minute;
  int second;
};

struct StampDiagnostics {
  bool valid = false;
  unsigned long rawEpoch = 0;
  unsigned long correctedEpoch = 0;
  DateTime utc{};
  DateTime local{};
  bool dstActive = false;
  int offsetHours = 0;
  long manualOffsetSeconds = 0;
};

StampDiagnostics gStampDiag;
unsigned long gLastSignalPublishMs = 0;
NBScanner signalScanner(false);

bool isLeapYear(int year) {
  if (year % 400 == 0) {
    return true;
  }
  if (year % 100 == 0) {
    return false;
  }
  return (year % 4) == 0;
}

int daysInMonth(int year, int month) {
  static const int kDaysPerMonth[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (month == 2 && isLeapYear(year)) {
    return 29;
  }
  return kDaysPerMonth[month - 1];
}

int dayOfWeek(int year, int month, int day) {
  int m = month;
  int y = year;
  if (m < 3) {
    m += 12;
    --y;
  }
  const int K = y % 100;
  const int J = y / 100;
  const int h = (day + (13 * (m + 1)) / 5 + K + K / 4 + J / 4 + 5 * J) % 7;
  return (h + 6) % 7;  // Convert to 0=Sunday, 6=Saturday
}

int lastSundayOfMonth(int year, int month) {
  int day = daysInMonth(year, month);
  while (dayOfWeek(year, month, day) != 0) {
    --day;
  }
  return day;
}

bool isCentralEuropeDst(const tm &utc) {
  const int year = utc.tm_year + 1900;
  const int month = utc.tm_mon + 1;
  const int day = utc.tm_mday;
  const int hour = utc.tm_hour;

  if (month < 3 || month > 10) {
    return false;
  }
  if (month > 3 && month < 10) {
    return true;
  }

  if (month == 3) {
    const int transitionDay = lastSundayOfMonth(year, 3);
    if (day > transitionDay) {
      return true;
    }
    if (day < transitionDay) {
      return false;
    }
    return hour >= 1;  // DST starts at 01:00 UTC
  }

  const int transitionDay = lastSundayOfMonth(year, 10);
  if (day < transitionDay) {
    return true;
  }
  if (day > transitionDay) {
    return false;
  }
  return hour < 1;  // DST ends at 01:00 UTC
}

DateTime toDateTime(const tm &utc) {
  DateTime dt;
  dt.year = utc.tm_year + 1900;
  dt.month = utc.tm_mon + 1;
  dt.day = utc.tm_mday;
  dt.hour = utc.tm_hour;
  dt.minute = utc.tm_min;
  dt.second = utc.tm_sec;
  return dt;
}

DateTime applyOffset(const DateTime &utc, int offsetMinutes) {
  DateTime local = utc;
  long totalMinutes = static_cast<long>(local.hour) * 60L + local.minute + static_cast<long>(offsetMinutes);
  const long kMinutesPerDay = 24L * 60L;

  while (totalMinutes >= kMinutesPerDay) {
    totalMinutes -= kMinutesPerDay;
    ++local.day;
    int dim = daysInMonth(local.year, local.month);
    if (local.day > dim) {
      local.day = 1;
      ++local.month;
      if (local.month > 12) {
        local.month = 1;
        ++local.year;
      }
    }
  }

  while (totalMinutes < 0) {
    totalMinutes += kMinutesPerDay;
    --local.day;
    if (local.day < 1) {
      --local.month;
      if (local.month < 1) {
        local.month = 12;
        --local.year;
      }
      local.day = daysInMonth(local.year, local.month);
    }
  }

  local.hour = static_cast<int>(totalMinutes / 60L);
  local.minute = static_cast<int>(totalMinutes % 60L);
  return local;
}

String formatDateTime(const DateTime &dt) {
  char buffer[20];
  snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d %02d:%02d:%02d",
           dt.year, dt.month, dt.day, dt.hour, dt.minute, dt.second);
  return String(buffer);
}

}  // namespace

String buildStartSessionStamp() {
  gStampDiag = {};
  const unsigned long epoch = nbAccess.getTime();
  gStampDiag.rawEpoch = epoch;

  if (epoch > 0) {
    const long correctedEpoch = static_cast<long>(epoch) + manualEpochOffsetSeconds;
    gStampDiag.manualOffsetSeconds = manualEpochOffsetSeconds;
    gStampDiag.correctedEpoch = correctedEpoch > 0 ? static_cast<unsigned long>(correctedEpoch) : 0;

    if (correctedEpoch > 0) {
      time_t raw = static_cast<time_t>(correctedEpoch);
      tm *utcPtr = gmtime(&raw);
      if (utcPtr != nullptr) {
        const tm utcTm = *utcPtr;
        const bool dstActive = isCentralEuropeDst(utcTm);
        const int offsetHours = dstActive ? timezoneDstOffsetHours : timezoneStandardOffsetHours;
        const int totalOffsetMinutes = offsetHours * 60;
        const DateTime utc = toDateTime(utcTm);
        const DateTime local = applyOffset(utc, totalOffsetMinutes);

        gStampDiag.valid = true;
        gStampDiag.utc = utc;
        gStampDiag.local = local;
        gStampDiag.dstActive = dstActive;
        gStampDiag.offsetHours = offsetHours;

        char buffer[16];
        snprintf(buffer, sizeof(buffer), "%04d%02d%02d-%02d%02d",
                 local.year,
                 local.month,
                 local.day,
                 local.hour,
                 local.minute);
        return String(buffer);
      }
    }
  }

  char fallback[16];
  snprintf(fallback, sizeof(fallback), "00000000-%04lu", millis() % 10000UL);
  return String(fallback);
}

void logStampDiagnostics() {
  Serial.print(F("Epoch (raw):      "));
  Serial.println(gStampDiag.rawEpoch);
  Serial.print(F("Epoch (corrected): "));
  Serial.println(gStampDiag.correctedEpoch);
  Serial.print(F("Manual offset:    "));
  Serial.print(gStampDiag.manualOffsetSeconds);
  Serial.println(F(" seconds"));
  if (!gStampDiag.valid) {
    Serial.println(F("Time conversion not available; using fallback timestamp."));
    return;
  }
  Serial.print(F("UTC:              "));
  Serial.println(formatDateTime(gStampDiag.utc));
  Serial.print(F("Local:            "));
  Serial.println(formatDateTime(gStampDiag.local));
  Serial.print(F("DST active:       "));
  Serial.println(gStampDiag.dstActive ? F("yes") : F("no"));
  Serial.print(F("Offset applied:   "));
  Serial.print(gStampDiag.offsetHours);
  Serial.println(F(" hours"));
}

void publishConnectionSignalQuality() {
  if (!mqttClient.connected()) {
    return;
  }

  const unsigned long now = millis();
  if (now - gLastSignalPublishMs < kSignalPublishIntervalMs) {
    return;
  }
  gLastSignalPublishMs = now;

  String csq = signalScanner.getSignalStrength();
  if (csq.length() == 0) {
    csq = "99";
  }
  mqttClient.publish(connectionSqTopic, csq.c_str());
}

void setup() {
  Serial.begin(115200);
  while (!Serial) {}

  Serial.println("Connecting NB...");
  if (!nbAccess.begin("", apn)) {
    Serial.println("NB attach failed");
    while (1) {}
  }
  Serial.println("NB attach OK");

  mqttClient.begin(mqttServer, mqttPort, client);
  const String clientId = "MKR1500NB-" + String(millis(), HEX);

  bool connected = false;
  const bool hasCredentials = (mqttUsername[0] != '\0') || (mqttPassword[0] != '\0');
  if (hasCredentials) {
    connected = mqttClient.connect(clientId.c_str(), mqttUsername, mqttPassword);
  } else {
    connected = mqttClient.connect(clientId.c_str());
  }

  if (connected) {
    Serial.println("MQTT connect OK");
    const String payload = buildStartSessionStamp();
    logStampDiagnostics();
    mqttClient.publish(startTopic, payload.c_str());
  } else {
    Serial.print("MQTT connect failed, error=");
    Serial.println((int)mqttClient.lastError());
  }
}

void loop() {
  mqttClient.loop();
  publishConnectionSignalQuality();
  delay(1000);
}

