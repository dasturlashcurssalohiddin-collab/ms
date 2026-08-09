/*
  TUXUM MARKET — MASOFAVIY QULF TIZIMI
  ESP32 Firmware v3.0
  (4 xonali kod, Telegram bot OLIB TASHLANDI, endi Website orqali boshqariladi,
   qulf faqat "yopish" buyrug'i kelganda yopiladi — avtomatik yopilmaydi)

  Talab qilinadigan kutubxonalar (Arduino Library Manager orqali o'rnating):
    - IRremoteESP8266 (by crankyoldgit)
    - ESP32Servo (by Kevin Harrington / Jaroslav Paral)
    - Firebase ESP Client (by Mobizt) -> "Firebase Arduino Client Library for ESP8266 and ESP32"
    - SevSeg (by Dean Reading) -> xom (drayversiz) 4-xonali 7-segment displey uchun

  Ulanishlar:
    - IR qabul qilgich SIGNAL oyoqchasi -> ESP32 GPIO 15 (IR_RECV_PIN)
    - Servo signal simi          -> ESP32 GPIO 5  (SERVO_PIN)
    - Buzzer                     -> ESP32 GPIO 25 (BUZZER_PIN) — GPIO34 ISHLATILMAYDI (faqat kirish uchun)
    - Qizil LED (yopiq/xato)     -> ESP32 GPIO 26 (RED_LED_PIN), rezistor orqali, katodi GND'ga
    - Yashil LED (ochiq)         -> ESP32 GPIO 27 (GREEN_LED_PIN), rezistor orqali, katodi GND'ga

    Displey (xom 12-pinli 4-xonali, umumiy katod) — SEGMENT pinlari,
    har biriga 220-330 Ohm rezistor orqali (GPIO -> rezistor -> displey pini):
      Displey pin 11 (A) -> GPIO 4
      Displey pin 7  (B) -> GPIO 13
      Displey pin 4  (C) -> GPIO 14
      Displey pin 2  (D) -> GPIO 16
      Displey pin 1  (E) -> GPIO 17
      Displey pin 10 (F) -> GPIO 18
      Displey pin 5  (G) -> GPIO 19
      Displey pin 3  (decimal) -> ULANMAYDI, kerak emas

    Displey DIGIT pinlari — har biri NPN tranzistor (2N2222/S8050) orqali
    (GPIO -> 1k rezistor -> tranzistor bazasi; kollektor -> displey pini; emitter -> GND):
      Displey pin 12 (D1) -> GPIO 21
      Displey pin 9  (D2) -> GPIO 23
      Displey pin 8  (D3) -> GPIO 32
      Displey pin 6  (D4) -> GPIO 33

  Firebase Realtime Database strukturasi:
    /lock_code       { value: "1234", updated_at: <unix_time> }         <- ESP32 yozadi, har 60s
    /lock_status     { state: "locked"|"unlocked", opened_by, closed_by, last_changed }
    /commands        { action: "try_code"|"lock"|"none", code, requested_by, timestamp }

  Mantiq:
    - Kod 4 xonali (0-9), har 60 soniyada o'zgaradi
    - Pult orqali 4 ta raqam kiritilsa -> avtomatik solishtiriladi
    - Saytdan "try_code" buyrug'i kelsa -> xuddi shunday solishtiriladi
    - To'g'ri bo'lsa -> yashil LED, servo ochadi, DOIM OCHIQ QOLADI
    - Noto'g'ri (yoki eskirgan/oldingi) kod -> qizil LED miltillaydi, xato ovozi, "EEEE"
    - Qulf FAQAT saytdan "lock" buyrug'i kelganda yopiladi (avtomatik yopilmaydi)
*/

#include <WiFi.h>
#include <time.h>
#include <IRremoteESP8266.h>
#include <IRrecv.h>
#include <IRutils.h>
#include <ESP32Servo.h>
#include <SevSeg.h>
#include <Firebase_ESP_Client.h>
#include "addons/TokenHelper.h"
#include "addons/RTDBHelper.h"

// ---------- SOZLAMALAR (o'zingizga moslang) ----------
#define WIFI_SSID        "SIZNING_WIFI_NOMI"
#define WIFI_PASSWORD    "SIZNING_WIFI_PAROLI"

#define FIREBASE_HOST    "sizning-loyiha-nomi-default-rtdb.firebaseio.com"
#define FIREBASE_API_KEY "SIZNING_FIREBASE_API_KEY"
#define FIREBASE_EMAIL   "sizning_email@misol.com"      // Firebase Auth (Email/Password) uchun
#define FIREBASE_PASS    "sizning_firebase_paroli"

#define IR_RECV_PIN      15
#define SERVO_PIN        5
#define SERVO_LOCKED_POS   0
#define SERVO_UNLOCKED_POS 90

#define BUZZER_PIN       25
#define RED_LED_PIN      26   // yopiq / xato holati
#define GREEN_LED_PIN    27   // ochiq holati

// Kod generatsiya uchun maxfiy kalit — buni faqat o'zingiz biling, hech kimga bermang
#define SECRET_SEED      123456789UL
#define CODE_WINDOW_SEC  60      // necha soniyada kod o'zgarsin (1 daqiqa)
#define CODE_LENGTH      4       // kod necha xonali

// ---------- GLOBAL O'ZGARUVCHILAR ----------
IRrecv irrecv(IR_RECV_PIN);
decode_results irResult;
Servo lockServo;
SevSeg sevseg;

FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;

String inputBuffer = "";
String lastPushedCode = "";
unsigned long lastCommandCheck = 0;
unsigned long lastCodeCheck = 0;
bool firebaseReady = false;
bool isUnlocked = false;

// Standart 21-tugmali pultning NEC kodlari (0-9 raqamlar)
long mapButtonToDigit(uint64_t code) {
  switch (code) {
    case 0xFF6897: return 0;
    case 0xFF30CF: return 1;
    case 0xFF18E7: return 2;
    case 0xFF7A85: return 3;
    case 0xFF10EF: return 4;
    case 0xFF38C7: return 5;
    case 0xFF5AA5: return 6;
    case 0xFF42BD: return 7;
    case 0xFF4AB5: return 8;
    case 0xFF52AD: return 9;
    default: return -1; // boshqa tugma (CH, VOL, EQ va h.k.) — e'tiborsiz qoldiriladi
  }
}

// Joriy 4 xonali kodni vaqt oynasiga qarab generatsiya qilish
String generateCurrentCode() {
  time_t now;
  time(&now);
  unsigned long window = (unsigned long)now / CODE_WINDOW_SEC;

  // Oddiy, tez hisoblanadigan generator (kripto darajasida emas, lekin loyihaga yetarli)
  unsigned long value = (window * 2654435761UL + SECRET_SEED) % 10000UL; // 0000-9999

  char buf[5];
  snprintf(buf, sizeof(buf), "%04lu", value);
  return String(buf);
}

// ---------- DISPLEY FUNKSIYALARI (xom 5641AS, SevSeg orqali multipleksatsiya) ----------

void showPatternFor(const char *pattern, unsigned long durationMs) {
  sevseg.setChars(pattern);
  unsigned long start = millis();
  while (millis() - start < durationMs) {
    sevseg.refreshDisplay();
  }
}

void showIdle() {
  sevseg.setChars("----");
}

// Kiritilayotgan raqamlarni chapdan boshlab ko'rsatish (4 xona to'liq sig'adi)
void showDigits(const String &buf) {
  String padded = buf;
  while (padded.length() < 4) padded += " ";
  sevseg.setChars(padded.c_str());
}

void showSuccessBlink() {
  for (int i = 0; i < 2; i++) {
    tone(BUZZER_PIN, 1500, 150);
    showPatternFor("----", 300);
    showPatternFor("    ", 150);
  }
}

// Xato/eski kod kiritilganda: EEEE + qizil LED miltillaydi + past ovoz
void showFailBlink() {
  for (int i = 0; i < 3; i++) {
    tone(BUZZER_PIN, 300, 150);
    digitalWrite(RED_LED_PIN, HIGH);
    showPatternFor("EEEE", 200);
    digitalWrite(RED_LED_PIN, LOW);
    showPatternFor("    ", 120);
  }
  // Qulf holatiga mos LED holatini tiklash
  digitalWrite(RED_LED_PIN, isUnlocked ? LOW : HIGH);
}

void setLockLeds(bool unlocked) {
  digitalWrite(RED_LED_PIN, unlocked ? LOW : HIGH);
  digitalWrite(GREEN_LED_PIN, unlocked ? HIGH : LOW);
}

void connectWiFi() {
  Serial.print("WiFi ga ulanmoqda");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) {
    delay(400);
    Serial.print(".");
  }
  Serial.println("\nWiFi ulandi, IP: " + WiFi.localIP().toString());
}

void setupFirebase() {
  config.api_key = FIREBASE_API_KEY;
  config.database_url = FIREBASE_HOST;
  auth.user.email = FIREBASE_EMAIL;
  auth.user.password = FIREBASE_PASS;
  config.token_status_callback = tokenStatusCallback;

  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);
  firebaseReady = true;
}

// Qulfni ochish — DOIM OCHIQ QOLADI, faqat lockDoorNow() chaqirilganda yopiladi
void unlockDoor(String source) {
  Serial.println("QULF OCHILDI (" + source + ")");
  lockServo.write(SERVO_UNLOCKED_POS);
  isUnlocked = true;
  setLockLeds(true);
  showIdle();

  if (firebaseReady) {
    Firebase.RTDB.setString(&fbdo, "/lock_status/state", "unlocked");
    Firebase.RTDB.setInt(&fbdo, "/lock_status/last_changed", (int)time(nullptr));
    Firebase.RTDB.setString(&fbdo, "/lock_status/opened_by", source);
  }
}

// Qulfni yopish — faqat saytdan "lock" buyrug'i kelganda chaqiriladi
void lockDoorNow(String source) {
  Serial.println("QULF YOPILDI (" + source + ")");
  lockServo.write(SERVO_LOCKED_POS);
  isUnlocked = false;
  setLockLeds(false);
  showIdle();

  if (firebaseReady) {
    Firebase.RTDB.setString(&fbdo, "/lock_status/state", "locked");
    Firebase.RTDB.setInt(&fbdo, "/lock_status/last_changed", (int)time(nullptr));
    Firebase.RTDB.setString(&fbdo, "/lock_status/closed_by", source);
  }
}

// Kiritilgan kodni tekshirish (pult yoki sayt orqali kelgan) — umumiy funksiya
void tryCode(const String &code, const String &source) {
  String correct = generateCurrentCode();
  if (code == correct) {
    showSuccessBlink();
    unlockDoor(source);
  } else {
    Serial.println("Noto'g'ri/eski kod: " + code);
    showFailBlink();
  }
}

// Sayt yuborgan buyruqlarni tekshirish: "try_code" yoki "lock"
void checkRemoteCommand() {
  if (!firebaseReady) return;
  if (millis() - lastCommandCheck < 1500) return;
  lastCommandCheck = millis();

  if (Firebase.RTDB.getJSON(&fbdo, "/commands")) {
    FirebaseJson *json = fbdo.jsonObjectPtr();
    FirebaseJsonData actionData, codeData, sourceData;
    json->get(actionData, "action");
    json->get(codeData, "code");
    json->get(sourceData, "requested_by");

    String action = actionData.stringValue;
    String source = sourceData.stringValue.length() ? sourceData.stringValue : "website";

    if (action == "try_code") {
      tryCode(codeData.stringValue, source);
      Firebase.RTDB.setString(&fbdo, "/commands/action", "none");
    } else if (action == "lock") {
      lockDoorNow(source);
      Firebase.RTDB.setString(&fbdo, "/commands/action", "none");
    }
  }
}

void pushCurrentCodeIfChanged() {
  if (!firebaseReady) return;
  if (millis() - lastCodeCheck < 1000) return;
  lastCodeCheck = millis();

  String current = generateCurrentCode();
  if (current != lastPushedCode) {
    lastPushedCode = current;
    Firebase.RTDB.setString(&fbdo, "/lock_code/value", current);
    Firebase.RTDB.setInt(&fbdo, "/lock_code/updated_at", (int)time(nullptr));
    Serial.println("Yangi qulf kodi: " + current);
  }
}

void pushLiveInput(const String &buf) {
  if (!firebaseReady) return;
  Firebase.RTDB.setString(&fbdo, "/live_input/digits", buf);
  Firebase.RTDB.setInt(&fbdo, "/live_input/updated_at", (int)time(nullptr));
}

void handleIRInput() {
  if (!irrecv.decode(&irResult)) return;

  long digit = mapButtonToDigit(irResult.value);
  if (digit >= 0) {
    inputBuffer += String(digit);
    Serial.println("Kiritildi: " + inputBuffer);
    showDigits(inputBuffer);
    pushLiveInput(inputBuffer); // sayt jonli ko'rishi uchun

    if (inputBuffer.length() == CODE_LENGTH) {
      tryCode(inputBuffer, "ir_remote");
      inputBuffer = "";
      pushLiveInput(""); // tozalash
    }
  } else if (irResult.value == 0xFFFFFFFF) {
    // tugma bosib turilganda takrorlanuvchi signal — e'tiborsiz qoldiriladi
  } else {
    // masalan "CH" tugmasi bosilsa — kiritilganlarni bekor qilish
    inputBuffer = "";
    showIdle();
    pushLiveInput("");
  }

  irrecv.resume();
}

void setupDisplay() {
  byte numDigits = 4;
  byte digitPins[] = {21, 23, 32, 33};              // D1, D2, D3, D4
  byte segmentPins[] = {4, 13, 14, 16, 17, 18, 19};  // A, B, C, D, E, F, G (decimal ishlatilmaydi)
  bool resistorsOnSegments = true;
  byte hardwareConfig = COMMON_CATHODE;
  bool updateWithDelays = false;
  bool leadingZeros = false;
  bool disableDecPoint = true;

  sevseg.begin(hardwareConfig, numDigits, digitPins, segmentPins,
               resistorsOnSegments, updateWithDelays, leadingZeros, disableDecPoint);
  sevseg.setBrightness(90);
}

void setup() {
  Serial.begin(115200);
  connectWiFi();

  configTime(5 * 3600, 0, "pool.ntp.org", "time.nist.gov"); // GMT+5 (Toshkent)
  Serial.println("Vaqt sinxronlanmoqda...");
  time_t now = time(nullptr);
  while (now < 100000) {
    delay(300);
    now = time(nullptr);
  }
  Serial.println("Vaqt sinxronlandi");

  setupFirebase();

  lockServo.attach(SERVO_PIN);
  lockServo.write(SERVO_LOCKED_POS);

  setupDisplay();
  showIdle();

  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(RED_LED_PIN, OUTPUT);
  pinMode(GREEN_LED_PIN, OUTPUT);
  setLockLeds(false); // boshida qulf yopiq

  irrecv.enableIRIn();
  Serial.println("Tizim tayyor. 4 xonali kodni pult orqali kiriting.");
}

void loop() {
  sevseg.refreshDisplay();
  handleIRInput();
  pushCurrentCodeIfChanged();
  checkRemoteCommand();
}
