#include <Arduino.h>
#include <ArduinoJson.h>
#include <PN5180ISO14443.h>
#include <PN5180ISO15693.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <esp_system.h>

namespace {
// PN5180 uses the SPI bus.
constexpr uint8_t PIN_NFC_NSS = 10;
constexpr uint8_t PIN_NFC_MOSI = 11;
constexpr uint8_t PIN_NFC_SCK = 12;
constexpr uint8_t PIN_NFC_MISO = 13;
constexpr uint8_t PIN_NFC_BUSY = 14;
constexpr uint8_t PIN_NFC_RST = 15;

// CMCU-06 uses an RS485-to-TTL converter on UART1.
constexpr uint8_t PIN_SCALE_RX = 5;
constexpr uint8_t PIN_SCALE_TX = 4;
constexpr uint32_t SCALE_BAUD = 9600;
constexpr float SCALE_GRAMS_PER_COUNT = 1.0006f;

// The through-beam receiver output is active-low when the beam is blocked.
constexpr uint8_t PIN_BEAM_SIGNAL = 16;
constexpr uint32_t BEAM_SAMPLE_INTERVAL_MS = 5;
constexpr uint32_t BEAM_DEBOUNCE_MS = 50;
constexpr uint32_t BEAM_MIN_BLOCK_MS = 150;
constexpr uint32_t BEAM_REARM_CLEAR_MS = 350;

// The 8048S070C display P1 UART is connected to ESP32 UART2.
constexpr uint8_t PIN_DISPLAY_RX = 17;
constexpr uint8_t PIN_DISPLAY_TX = 18;
constexpr uint32_t DISPLAY_BAUD = 115200;
constexpr uint32_t DISPLAY_REPORT_INTERVAL_MS = 100;
constexpr uint32_t SCALE_SUCCESS_DELAY_MS = 50;
constexpr uint32_t SCALE_ERROR_DELAY_MS = 500;
constexpr uint32_t CARD_ACK_RETRY_MS = 200;
constexpr uint8_t CARD_ACK_MAX_ATTEMPTS = 5;
constexpr uint32_t CARD_PRESENCE_CONFIRM_MS = 500;
constexpr uint32_t CARD_ABSENCE_CONFIRM_MS = 400;
constexpr uint8_t PIN_DTU_RX = 7;
constexpr uint8_t PIN_DTU_TX = 6;
constexpr uint32_t DTU_BAUD = 115200;
constexpr uint32_t DEPOSIT_TIMEOUT_MS = 60UL * 1000UL;
constexpr uint32_t DTU_RETRY_MS = 2000;
constexpr uint8_t DTU_MAX_ATTEMPTS = 30;
constexpr uint8_t DTU_QUEUE_SIZE = 6;

const uint8_t READ_WEIGHT_REQUEST[] = {
    0x01, 0x03, 0x00, 0x00, 0x00, 0x02, 0xC4, 0x0B};

PN5180ISO14443 nfcA(PIN_NFC_NSS, PIN_NFC_BUSY, PIN_NFC_RST);
PN5180ISO15693 nfcV(PIN_NFC_NSS, PIN_NFC_BUSY, PIN_NFC_RST);
HardwareSerial dtuSerial(0);

struct ScaleState {
  int32_t raw;
  float grams;
  bool valid;
  uint32_t lastSuccessMs;
  uint32_t transactionCount;
};

ScaleState scaleState = {0, 0.0f, false, 0, 0};
SemaphoreHandle_t scaleMutex = nullptr;
bool nfcReady = false;

struct BeamState {
  bool blocked;
  uint32_t changeCount;
  uint32_t bottleCount;
  uint32_t lastChangeMs;
};

BeamState beamState = {false, 0, 0, 0};
SemaphoreHandle_t beamMutex = nullptr;

uint16_t modbusCrc(const uint8_t *data, size_t length) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < length; ++i) {
    crc ^= data[i];
    for (uint8_t bit = 0; bit < 8; ++bit) {
      crc = (crc & 1) ? ((crc >> 1) ^ 0xA001) : (crc >> 1);
    }
  }
  return crc;
}

bool readWeightRaw(int32_t &signedRaw, uint16_t &register0,
                   uint16_t &register1) {
  while (Serial1.available()) {
    Serial1.read();
  }

  Serial1.write(READ_WEIGHT_REQUEST, sizeof(READ_WEIGHT_REQUEST));
  Serial1.flush();

  uint8_t response[32] = {0};
  size_t length = 0;
  const uint32_t deadline = millis() + 350;
  while (millis() < deadline && length < sizeof(response)) {
    if (Serial1.available()) {
      response[length++] = static_cast<uint8_t>(Serial1.read());
    } else {
      delay(2);
    }
  }

  if (length < 9 || response[0] != 0x01 || response[1] != 0x03 ||
      response[2] != 0x04) {
    return false;
  }

  const uint16_t receivedCrc =
      static_cast<uint16_t>(response[7]) |
      (static_cast<uint16_t>(response[8]) << 8);
  if (receivedCrc != modbusCrc(response, 7)) {
    return false;
  }

  register0 =
      (static_cast<uint16_t>(response[3]) << 8) | response[4];
  register1 =
      (static_cast<uint16_t>(response[5]) << 8) | response[6];
  const uint32_t raw =
      (static_cast<uint32_t>(register1) << 16) | register0;
  signedRaw = static_cast<int32_t>(raw);
  return true;
}

void scaleTask(void *) {
  for (;;) {
    int32_t raw = 0;
    uint16_t register0 = 0;
    uint16_t register1 = 0;
    const bool ok = readWeightRaw(raw, register0, register1);

    if (xSemaphoreTake(scaleMutex, portMAX_DELAY) == pdTRUE) {
      ++scaleState.transactionCount;
      scaleState.valid = ok;
      if (ok) {
        scaleState.raw = raw;
        scaleState.grams =
            static_cast<float>(raw) * SCALE_GRAMS_PER_COUNT;
        scaleState.lastSuccessMs = millis();
      }
      xSemaphoreGive(scaleMutex);
    }

    vTaskDelay(pdMS_TO_TICKS(
        ok ? SCALE_SUCCESS_DELAY_MS : SCALE_ERROR_DELAY_MS));
  }
}

void beamTask(void *) {
  bool candidateLevel = digitalRead(PIN_BEAM_SIGNAL);
  bool stableLevel = candidateLevel;
  uint32_t candidateSinceMs = millis();
  uint32_t blockedSinceMs = stableLevel == LOW ? candidateSinceMs : 0;
  uint32_t clearSinceMs = stableLevel == HIGH ? candidateSinceMs : 0;
  // Do not count a beam that is already blocked at boot. Once it stays clear
  // for the re-arm period, normal bottle counting resumes.
  bool countedThisBlock = stableLevel == LOW;

  for (;;) {
    const uint32_t now = millis();
    const bool rawLevel = digitalRead(PIN_BEAM_SIGNAL);

    if (rawLevel != candidateLevel) {
      candidateLevel = rawLevel;
      candidateSinceMs = now;
    }

    if (candidateLevel != stableLevel &&
        now - candidateSinceMs >= BEAM_DEBOUNCE_MS) {
      stableLevel = candidateLevel;
      if (xSemaphoreTake(beamMutex, portMAX_DELAY) == pdTRUE) {
        beamState.blocked = stableLevel == LOW;
        ++beamState.changeCount;
        beamState.lastChangeMs = now;
        xSemaphoreGive(beamMutex);
      }
      if (stableLevel == LOW) {
        blockedSinceMs = now;
        clearSinceMs = 0;
      } else {
        clearSinceMs = now;
        blockedSinceMs = 0;
      }
    }

    if (stableLevel == LOW && !countedThisBlock &&
        blockedSinceMs != 0 &&
        now - blockedSinceMs >= BEAM_MIN_BLOCK_MS) {
      if (xSemaphoreTake(beamMutex, portMAX_DELAY) == pdTRUE) {
        ++beamState.bottleCount;
        xSemaphoreGive(beamMutex);
      }
      countedThisBlock = true;
    } else if (stableLevel == HIGH && countedThisBlock &&
               clearSinceMs != 0 &&
               now - clearSinceMs >= BEAM_REARM_CLEAR_MS) {
      countedThisBlock = false;
    }

    vTaskDelay(pdMS_TO_TICKS(BEAM_SAMPLE_INTERVAL_MS));
  }
}

String uidToString(const uint8_t *uid, uint8_t length, bool reverse) {
  String result;
  for (uint8_t i = 0; i < length; ++i) {
    if (i) result += ':';
    const uint8_t value = reverse ? uid[length - 1 - i] : uid[i];
    if (value < 0x10) result += '0';
    result += String(value, HEX);
  }
  result.toUpperCase();
  return result;
}

bool isValidIso15693Uid(const uint8_t *uid) {
  bool hasNonZero = false;
  bool allOnes = true;
  for (uint8_t i = 0; i < 8; ++i) {
    hasNonZero = hasNonZero || uid[i] != 0x00;
    allOnes = allOnes && uid[i] == 0xFF;
  }
  return hasNonZero && !allOnes;
}

uint32_t cardEventSequence = 0;
uint32_t pendingCardEventId = 0;
uint32_t pendingCardLastSendMs = 0;
uint32_t pendingCardFirstSendMs = 0;
uint8_t pendingCardAttempts = 0;
bool cardEventPending = false;
String pendingCardUid;

void sendPendingCardEvent() {
  if (!cardEventPending) {
    return;
  }
  Serial.printf(
      "CARD EVENT TX id=%lu attempt=%u t=%lu\n",
      static_cast<unsigned long>(pendingCardEventId),
      static_cast<unsigned int>(pendingCardAttempts + 1),
      static_cast<unsigned long>(millis()));
  Serial2.printf("CARD_EVENT|%s|%lu\r\n", pendingCardUid.c_str(),
                 static_cast<unsigned long>(pendingCardEventId));
  pendingCardLastSendMs = millis();
  ++pendingCardAttempts;
}

void queueCardEvent(const String &uid) {
  ++cardEventSequence;
  pendingCardEventId = cardEventSequence;
  pendingCardUid = uid;
  pendingCardAttempts = 0;
  pendingCardFirstSendMs = millis();
  cardEventPending = true;
  sendPendingCardEvent();
}

void serviceCardAck() {
  static String line;
  while (Serial2.available()) {
    const char character = static_cast<char>(Serial2.read());
    if (character == '\r') {
      continue;
    }
    if (character == '\n') {
      if (line.startsWith("ACK|CARD_EVENT|")) {
        const uint32_t eventId =
            static_cast<uint32_t>(line.substring(15).toInt());
        if (cardEventPending && eventId == pendingCardEventId) {
          Serial.printf(
              "CARD EVENT ACK id=%lu latency_ms=%lu\n",
              static_cast<unsigned long>(eventId),
              static_cast<unsigned long>(
                  millis() - pendingCardFirstSendMs));
          cardEventPending = false;
        }
      }
      line = "";
    } else if (line.length() < 96) {
      line += character;
    }
  }

  if (cardEventPending &&
      millis() - pendingCardLastSendMs >= CARD_ACK_RETRY_MS) {
    if (pendingCardAttempts >= CARD_ACK_MAX_ATTEMPTS) {
      Serial.printf(
          "CARD EVENT NO ACK id=%lu attempts=%u\n",
          static_cast<unsigned long>(pendingCardEventId),
          static_cast<unsigned int>(pendingCardAttempts));
      cardEventPending = false;
    } else {
      sendPendingCardEvent();
    }
  }
}

String deviceId;
uint32_t bootNonce = 0;
uint32_t dtuMessageSequence = 0;

struct SensorSnapshot {
  int32_t raw;
  float grams;
  bool scaleValid;
  uint32_t bottles;
};

struct DepositSession {
  bool active = false;
  String cardUid;
  String studentName;
  int32_t startRaw = 0;
  bool startScaleValid = false;
  uint32_t startBottleCount = 0;
  uint32_t startedMs = 0;
  uint32_t sessionNumber = 0;
};

DepositSession depositSession;
int lastKnownTotalPoints = -1;

struct DtuPendingMessage {
  bool active = false;
  uint32_t messageId = 0;
  uint32_t lastSentMs = 0;
  uint8_t attempts = 0;
  String type;
  String cardUid;
  String payload;
};

DtuPendingMessage dtuPending[DTU_QUEUE_SIZE];

SensorSnapshot readSensorSnapshot() {
  SensorSnapshot snapshot = {0, 0.0f, false, 0};
  if (xSemaphoreTake(scaleMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
    snapshot.raw = scaleState.raw;
    snapshot.grams = scaleState.grams;
    snapshot.scaleValid = scaleState.valid;
    xSemaphoreGive(scaleMutex);
  }
  if (xSemaphoreTake(beamMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
    snapshot.bottles = beamState.bottleCount;
    xSemaphoreGive(beamMutex);
  }
  return snapshot;
}

void sendStudentInfoToDisplay(const String &uid, const String &name,
                              int points) {
  if (points < 0) {
    return;
  }
  String displayName = name;
  displayName.replace("|", " ");
  displayName.replace("\r", " ");
  displayName.replace("\n", " ");
  if (displayName.length() == 0) {
    displayName = "未登记学生";
  }
  Serial2.printf("STUDENT_INFO|%s|%d|%s\r\n", uid.c_str(), points,
                 displayName.c_str());
  // Retain the old message so current display behavior remains compatible.
  Serial2.printf("STUDENT_POINTS|%s|%d\r\n", uid.c_str(), points);
}

void sendSettlementResultToDisplay(const String &uid, int totalPoints,
                                   bool confirmed) {
  Serial2.printf("SETTLE_RESULT|%s|%d|%d\r\n", uid.c_str(), totalPoints,
                 confirmed ? 1 : 0);
}

void sendDtuPending(uint8_t index) {
  DtuPendingMessage &pending = dtuPending[index];
  if (!pending.active) {
    return;
  }
  Serial.printf("DTU TX msg=%lu attempt=%u\n",
                static_cast<unsigned long>(pending.messageId),
                static_cast<unsigned int>(pending.attempts + 1));
  dtuSerial.print(pending.payload);
  dtuSerial.write('\n');
  pending.lastSentMs = millis();
  ++pending.attempts;
}

bool queueDtuMessage(const char *type, JsonDocument &document) {
  int freeIndex = -1;
  for (uint8_t i = 0; i < DTU_QUEUE_SIZE; ++i) {
    if (!dtuPending[i].active) {
      freeIndex = i;
      break;
    }
  }
  if (freeIndex < 0) {
    Serial.println("DTU queue full; dropping oldest message");
    freeIndex = 0;
  }

  const uint32_t messageId = ++dtuMessageSequence;
  document["type"] = type;
  document["msg_id"] = messageId;
  document["device_id"] = deviceId;
  document["uptime_ms"] = millis();

  String payload;
  serializeJson(document, payload);
  DtuPendingMessage &pending = dtuPending[freeIndex];
  pending.active = true;
  pending.messageId = messageId;
  pending.attempts = 0;
  pending.type = type;
  pending.cardUid = String(document["card_uid"] | "");
  pending.payload = payload;
  sendDtuPending(static_cast<uint8_t>(freeIndex));
  return true;
}

String makeSessionEventId(uint32_t sessionNumber) {
  char buffer[80];
  snprintf(buffer, sizeof(buffer), "%s-%08lX-%lu", deviceId.c_str(),
           static_cast<unsigned long>(bootNonce),
           static_cast<unsigned long>(sessionNumber));
  return String(buffer);
}

void sendCardStartRequest() {
  JsonDocument request;
  request["card_uid"] = depositSession.cardUid;
  request["event_id"] = makeSessionEventId(depositSession.sessionNumber);
  queueDtuMessage("card_start", request);
  Serial.printf("SERVER QUERY: card=%s\n", depositSession.cardUid.c_str());
}

void finishDepositForServer(bool timedOut) {
  if (!depositSession.active) {
    return;
  }

  const SensorSnapshot end = readSensorSnapshot();
  const int32_t rawDelta =
      end.raw > depositSession.startRaw ? end.raw - depositSession.startRaw : 0;
  const float grams = static_cast<float>(rawDelta) * SCALE_GRAMS_PER_COUNT;
  const float weightKg = grams / 1000.0f;
  const uint32_t bottles =
      end.bottles > depositSession.startBottleCount
          ? end.bottles - depositSession.startBottleCount
          : 0;
  const int points = static_cast<int>(bottles) * 5 +
                     static_cast<int>(lroundf(weightKg * 10.0f));
  const String eventId = makeSessionEventId(depositSession.sessionNumber);

  JsonDocument request;
  request["event_id"] = eventId;
  request["card_uid"] = depositSession.cardUid;
  request["name"] = depositSession.studentName;
  request["weight_kg"] = weightKg;
  request["bottles"] = bottles;
  request["points"] = points;
  request["started_uptime_ms"] = depositSession.startedMs;
  request["ended_uptime_ms"] = millis();
  request["scale_valid"] = depositSession.startScaleValid && end.scaleValid;
  request["timed_out"] = timedOut;
  queueDtuMessage("settle", request);

  Serial.printf(
      "SERVER SETTLE: card=%s weight=%.3fkg bottles=%lu points=%d%s\n",
      depositSession.cardUid.c_str(), static_cast<double>(weightKg),
      static_cast<unsigned long>(bottles), points,
      timedOut ? " (timeout)" : "");
  depositSession.active = false;
}

void beginDepositForServer(const String &uid) {
  const SensorSnapshot snapshot = readSensorSnapshot();
  ++depositSession.sessionNumber;
  depositSession.active = true;
  depositSession.cardUid = uid;
  depositSession.studentName = "";
  depositSession.startRaw = snapshot.raw;
  depositSession.startScaleValid = snapshot.scaleValid;
  depositSession.startBottleCount = snapshot.bottles;
  depositSession.startedMs = millis();
  sendCardStartRequest();
}

void handleCardSwipe(const String &uid) {
  if (depositSession.active && depositSession.cardUid == uid) {
    finishDepositForServer(false);
    return;
  }
  if (depositSession.active) {
    finishDepositForServer(false);
  }
  beginDepositForServer(uid);
}

void serviceDtuServer() {
  static String line;
  while (dtuSerial.available()) {
    const char character = static_cast<char>(dtuSerial.read());
    if (character == '\r') {
      continue;
    }
    if (character == '\n') {
      JsonDocument response;
      const DeserializationError error = deserializeJson(response, line);
      if (!error) {
        const uint32_t messageId =
            static_cast<uint32_t>(response["msg_id"] | 0UL);
        String acknowledgedType;
        String acknowledgedUid;
        for (uint8_t i = 0; i < DTU_QUEUE_SIZE; ++i) {
          if (dtuPending[i].active &&
              dtuPending[i].messageId == messageId) {
            acknowledgedType = dtuPending[i].type;
            acknowledgedUid = dtuPending[i].cardUid;
            dtuPending[i].active = false;
          }
        }

        const String type = String(response["type"] | "");
        if (type == "student") {
          const String uid = String(response["card_uid"] | "");
          const String name = String(response["name"] | "");
          const int totalPoints = response["total_points"] | -1;
          if (depositSession.active && uid == depositSession.cardUid) {
            depositSession.studentName = name;
          }
          lastKnownTotalPoints = totalPoints;
          sendStudentInfoToDisplay(uid, name, totalPoints);
          Serial.printf("SERVER STUDENT: card=%s name=%s points=%d\n",
                        uid.c_str(), name.c_str(), totalPoints);
        } else if (type == "settle_ack") {
          const String uid = String(response["card_uid"] | "");
          const int totalPoints = response["total_points"] | -1;
          lastKnownTotalPoints = totalPoints;
          sendStudentInfoToDisplay(uid, depositSession.studentName,
                                   totalPoints);
          sendSettlementResultToDisplay(uid, totalPoints, true);
          Serial.printf("SERVER SAVED: event=%s points=%d\n",
                        static_cast<const char *>(response["event_id"] | ""),
                        totalPoints);
        } else if (type == "error") {
          if (acknowledgedType == "settle") {
            sendSettlementResultToDisplay(acknowledgedUid, -1, false);
          }
          Serial.printf("SERVER ERROR: %s\n",
                        static_cast<const char *>(response["error"] | "unknown"));
        }
      }
      line = "";
    } else if (line.length() < 700) {
      line += character;
    }
  }

  for (uint8_t i = 0; i < DTU_QUEUE_SIZE; ++i) {
    DtuPendingMessage &pending = dtuPending[i];
    if (!pending.active ||
        millis() - pending.lastSentMs < DTU_RETRY_MS) {
      continue;
    }
    if (pending.attempts >= DTU_MAX_ATTEMPTS) {
      Serial.printf("DTU NO ACK msg=%lu\n",
                    static_cast<unsigned long>(pending.messageId));
      if (pending.type == "settle") {
        sendSettlementResultToDisplay(pending.cardUid, -1, false);
      }
      pending.active = false;
    } else {
      sendDtuPending(i);
    }
  }
}

bool initializeNfc() {
  Serial.println("NFC: initializing PN5180");
  nfcA.begin(PIN_NFC_SCK, PIN_NFC_MISO, PIN_NFC_MOSI, PIN_NFC_NSS);
  nfcA.commandTimeout = 200;
  nfcV.commandTimeout = 80;

  pinMode(PIN_NFC_BUSY, INPUT_PULLUP);
  pinMode(PIN_NFC_RST, OUTPUT);
  digitalWrite(PIN_NFC_RST, LOW);
  delay(20);
  digitalWrite(PIN_NFC_RST, HIGH);
  delay(30);

  if (digitalRead(PIN_NFC_BUSY) != LOW) {
    Serial.println("NFC: BUSY check failed; check PN5180 power and wiring");
    return false;
  }

  nfcA.reset();

  uint8_t product[2] = {0xFF, 0xFF};
  uint8_t firmware[2] = {0xFF, 0xFF};
  nfcA.readEEprom(PRODUCT_VERSION, product, sizeof(product));
  nfcA.readEEprom(FIRMWARE_VERSION, firmware, sizeof(firmware));
  if (product[0] == 0xFF && product[1] == 0xFF) {
    Serial.println("NFC: PN5180 not detected");
    return false;
  }

  if (!nfcV.setupRF()) {
    Serial.println("NFC: ISO15693 RF setup failed");
    return false;
  }

  Serial.printf(
      "NFC: product=%u.%u firmware=%u.%u; fast ISO15693 scan\n",
      product[1], product[0], firmware[1], firmware[0]);
  return true;
}

void reportCard(const char *protocol, const String &uid,
                String &lastCard, uint32_t &lastStatus) {
  const String detected = String(protocol) + "|" + uid;
  const uint32_t now = millis();
  const bool newlyDetected = detected != lastCard;
  if (detected != lastCard || now - lastStatus >= 1500) {
    Serial.printf("CARD PROTOCOL: %s\nCARD UID: %s\n", protocol,
                  uid.c_str());
    if (newlyDetected) {
      handleCardSwipe(uid);
      queueCardEvent(uid);
    }
    lastCard = detected;
    lastStatus = now;
  }
}

enum class NfcProtocol {
  None,
  Iso14443A,
  Iso15693,
};

bool scanIso14443A(String &uidText) {
  uint8_t uidA[10] = {0};
  nfcA.clearIRQStatus(0xFFFFFFFF);
  const int8_t lengthA = nfcA.readCardSerial(uidA);
  const bool foundA = lengthA >= 4;

  if (foundA) {
    uidText = uidToString(uidA, static_cast<uint8_t>(lengthA), false);
    nfcA.mifareHalt();
  }

  if (lengthA == -1) {
    nfcA.reset();
  } else {
    nfcA.setRF_off();
  }
  return foundA;
}

bool scanIso15693(String &uidText) {
  uint8_t uidV[8] = {0};
  const ISO15693ErrorCode resultV =
      nfcV.getInventory(uidV);
  const bool foundV =
      resultV == ISO15693_EC_OK && isValidIso15693Uid(uidV);
  if (foundV) {
    uidText = uidToString(uidV, sizeof(uidV), true);
  }
  return foundV;
}

void scanNfc() {
  static uint32_t cycleCount = 0;
  static uint32_t lastScanReport = 0;
  static String lastCard;
  static String candidateUid;
  static uint32_t candidateSinceMs = 0;
  static bool absenceTracking = false;
  static uint32_t absenceSinceMs = 0;
  ++cycleCount;

  String uidText;
  bool found = false;
  NfcProtocol detectedProtocol = NfcProtocol::None;
  const uint32_t now = millis();

  found = scanIso15693(uidText);
  detectedProtocol = NfcProtocol::Iso15693;

  if (found) {
    absenceTracking = false;
    if (uidText != candidateUid) {
      candidateUid = uidText;
      candidateSinceMs = now;
    }
    if (now - candidateSinceMs >= CARD_PRESENCE_CONFIRM_MS) {
      reportCard(detectedProtocol == NfcProtocol::Iso14443A
                     ? "ISO14443-A"
                     : "ISO15693",
                 uidText, lastCard, lastScanReport);
    }
  } else {
    candidateUid = "";
    candidateSinceMs = 0;
    if (lastCard.length() != 0) {
      if (!absenceTracking) {
        absenceTracking = true;
        absenceSinceMs = now;
      } else if (now - absenceSinceMs >= CARD_ABSENCE_CONFIRM_MS) {
        lastCard = "";
        absenceTracking = false;
      }
    }
    if (now - lastScanReport >= 3000) {
      Serial.printf("NFC SCAN #%lu waiting\n",
                    static_cast<unsigned long>(cycleCount));
      lastScanReport = now;
    }
  }
}

void printWeightIfChanged() {
  static int32_t lastRaw = INT32_MIN;
  static bool lastValid = false;
  static uint32_t lastPrintMs = 0;
  static uint32_t lastNoResponseMs = 0;

  ScaleState snapshot = {0, 0.0f, false, 0, 0};
  if (xSemaphoreTake(scaleMutex, pdMS_TO_TICKS(20)) != pdTRUE) {
    return;
  }
  snapshot = scaleState;
  xSemaphoreGive(scaleMutex);

  const uint32_t now = millis();
  if (snapshot.valid) {
    const bool changed =
        !lastValid || abs(snapshot.raw - lastRaw) >= 1;
    const bool periodic = now - lastPrintMs >= 2000;
    if (changed || periodic) {
      const uint32_t age =
          snapshot.lastSuccessMs == 0
              ? 0
              : now - snapshot.lastSuccessMs;
      Serial.printf(
          "WEIGHT status=OK raw=%ld grams=%.1f age_ms=%lu "
          "samples=%lu\n",
          static_cast<long>(snapshot.raw),
          static_cast<double>(snapshot.grams),
          static_cast<unsigned long>(age),
          static_cast<unsigned long>(snapshot.transactionCount));
      lastRaw = snapshot.raw;
      lastValid = true;
      lastPrintMs = now;
    }
  } else if (lastValid || now - lastNoResponseMs >= 2000) {
    Serial.printf(
        "WEIGHT status=NO_RESPONSE attempts=%lu\n",
        static_cast<unsigned long>(snapshot.transactionCount));
    lastValid = false;
    lastNoResponseMs = now;
  }
}

void printBeamIfChanged() {
  static uint32_t lastChangeCount = 0;

  BeamState snapshot = {false, 0, 0, 0};
  if (xSemaphoreTake(beamMutex, pdMS_TO_TICKS(20)) != pdTRUE) {
    return;
  }
  snapshot = beamState;
  xSemaphoreGive(beamMutex);

  if (snapshot.changeCount == lastChangeCount) {
    return;
  }

  Serial.printf(
      "BEAM status=%s changes=%lu bottles=%lu last_change_ms=%lu\n",
      snapshot.blocked ? "BLOCKED" : "CLEAR",
      static_cast<unsigned long>(snapshot.changeCount),
      static_cast<unsigned long>(snapshot.bottleCount),
      static_cast<unsigned long>(snapshot.lastChangeMs));
  lastChangeCount = snapshot.changeCount;
}

void sendDisplaySnapshot() {
  ScaleState scaleSnapshot = {0, 0.0f, false, 0, 0};
  if (xSemaphoreTake(scaleMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
    scaleSnapshot = scaleState;
    xSemaphoreGive(scaleMutex);
  }

  BeamState beamSnapshot = {false, 0, 0, 0};
  if (xSemaphoreTake(beamMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
    beamSnapshot = beamState;
    xSemaphoreGive(beamMutex);
  }

  if (scaleSnapshot.valid) {
    Serial2.printf("SENSOR|%.3f|%lu\r\n",
                   static_cast<double>(scaleSnapshot.grams / 1000.0f),
                   static_cast<unsigned long>(beamSnapshot.bottleCount));
  } else {
    Serial2.printf("SENSOR|--|%lu\r\n",
                   static_cast<unsigned long>(beamSnapshot.bottleCount));
  }
}

void displayTask(void *) {
  const uint32_t taskStartedMs = millis();
  uint32_t lastResetMs = 0;
  for (;;) {
    // A controller reset loses the active deposit session. Notify the display
    // during startup so it cannot remain stuck on an orphaned session screen.
    if (millis() - taskStartedMs < 6000 &&
        millis() - lastResetMs >= 500) {
      Serial2.print("RESET\r\n");
      lastResetMs = millis();
    }
    sendDisplaySnapshot();
    vTaskDelay(pdMS_TO_TICKS(DISPLAY_REPORT_INTERVAL_MS));
  }
}
}  // namespace

void setup() {
  Serial.begin(115200);
  delay(1200);
  char deviceIdBuffer[32];
  snprintf(deviceIdBuffer, sizeof(deviceIdBuffer), "RECYCLE-%08lX",
           static_cast<unsigned long>(
               static_cast<uint32_t>(ESP.getEfuseMac())));
  deviceId = deviceIdBuffer;
  bootNonce = esp_random();

  Serial.println();
  Serial.println(
      "CAMPUS RECYCLING: NFC + CMCU-06 WEIGHT + IR BEAM + 4G DTU");
  Serial.printf("DEVICE ID: %s\n", deviceId.c_str());
  Serial.printf(
      "NFC SPI: NSS=%u MOSI=%u SCK=%u MISO=%u BUSY=%u RST=%u\n",
      PIN_NFC_NSS, PIN_NFC_MOSI, PIN_NFC_SCK, PIN_NFC_MISO,
      PIN_NFC_BUSY, PIN_NFC_RST);
  Serial.printf(
      "SCALE UART1: ESP32 TX=GPIO%u -> converter RX, "
      "ESP32 RX=GPIO%u <- converter TX, 9600 8N1\n",
      PIN_SCALE_TX, PIN_SCALE_RX);
  Serial.printf(
      "DISPLAY UART2: ESP32 TX=GPIO%u -> screen RX0, "
      "ESP32 RX=GPIO%u <- screen TX0, 115200 8N1\n",
      PIN_DISPLAY_TX, PIN_DISPLAY_RX);
  Serial.printf(
      "DTU UART0: ESP32 TX=GPIO%u -> DTU RXD, "
      "ESP32 RX=GPIO%u <- DTU TXD, %lu 8N1\n",
      PIN_DTU_TX, PIN_DTU_RX,
      static_cast<unsigned long>(DTU_BAUD));

  pinMode(PIN_BEAM_SIGNAL, INPUT_PULLUP);
  beamState.blocked = digitalRead(PIN_BEAM_SIGNAL) == LOW;
  beamState.lastChangeMs = millis();
  Serial.printf(
      "BEAM GPIO%u: initial=%s, blocked=LOW, debounce=%lu ms\n",
      PIN_BEAM_SIGNAL, beamState.blocked ? "BLOCKED" : "CLEAR",
      static_cast<unsigned long>(BEAM_DEBOUNCE_MS));

  Serial1.begin(SCALE_BAUD, SERIAL_8N1, PIN_SCALE_RX, PIN_SCALE_TX);
  Serial2.begin(DISPLAY_BAUD, SERIAL_8N1, PIN_DISPLAY_RX,
                PIN_DISPLAY_TX);
  dtuSerial.setRxBufferSize(1024);
  dtuSerial.begin(DTU_BAUD, SERIAL_8N1, PIN_DTU_RX, PIN_DTU_TX);
  scaleMutex = xSemaphoreCreateMutex();
  if (scaleMutex == nullptr) {
    Serial.println("FATAL: could not create scale mutex");
    while (true) delay(1000);
  }

  beamMutex = xSemaphoreCreateMutex();
  if (beamMutex == nullptr) {
    Serial.println("FATAL: could not create beam mutex");
    while (true) delay(1000);
  }

  xTaskCreatePinnedToCore(scaleTask, "scaleTask", 4096, nullptr, 2,
                          nullptr, 0);
  xTaskCreatePinnedToCore(beamTask, "beamTask", 2048, nullptr, 2,
                          nullptr, 1);
  xTaskCreatePinnedToCore(displayTask, "displayTask", 3072, nullptr,
                          1, nullptr, 1);

  nfcReady = initializeNfc();
  if (!nfcReady) {
    Serial.println(
        "NFC unavailable; scale and beam services remain active");
  }
  Serial.println("SYSTEM READY");
}

void loop() {
  static uint32_t lastNfcRetry = 0;

  serviceCardAck();
  serviceDtuServer();
  printWeightIfChanged();
  printBeamIfChanged();

  if (depositSession.active &&
      millis() - depositSession.startedMs >= DEPOSIT_TIMEOUT_MS) {
    finishDepositForServer(true);
  }

  if (!nfcReady) {
    const uint32_t now = millis();
    if (now - lastNfcRetry >= 5000) {
      lastNfcRetry = now;
      nfcReady = initializeNfc();
    }
    delay(100);
    return;
  }

  scanNfc();
  serviceCardAck();
  delay(20);
}
