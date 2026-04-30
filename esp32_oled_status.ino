#include <Wire.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoOTA.h>
#include <ESPmDNS.h>
#include <Update.h>
#include <Preferences.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <string.h>
#include <time.h>

#if __has_include("wifi_secrets.h")
#include "wifi_secrets.h"
#else
// wifi_secrets.h yoksa asagidaki varsayilan ag kullanilir (tek dosya kopyala-yapistir icin).
// Farkli ag icin: wifi_secrets.h ekleyin veya burayi duzenleyin.
#ifndef WIFI_SSID
#define WIFI_SSID "Zyxel_3691"
#endif
#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD "3883D488Y7"
#endif
#endif

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
#define OLED_ADDRESS 0x3C
#define TDS_PIN 34
#define PH_PIN 35
#define MODE_BTN_PIN 27
#define RELAY_STRIP_PIN 32
#define RELAY_DROP_PIN 33
#define RELAY_AUX_PIN 26
// 1: pin LOW = role ON (yaygin 2 kanalli moduller). 0: pin HIGH = ON.
#define RELAY_ACTIVE_LOW 1
#define RELAY_SEQ_DELAY_MS 500

// OTA: Arduino IDE -> Port -> ag portu | Tarayici -> http://<IP>/ota
#ifndef OTA_HOSTNAME
#define OTA_HOSTNAME "hydro-esp32"
#endif
#ifndef OTA_PASSWORD
#define OTA_PASSWORD "hydro_ota_change_me"
#endif

// NTP (internet saati). Turkiye TRT = UTC+3, yaz saati yok.
#ifndef NTP_GMT_OFFSET_SEC
#define NTP_GMT_OFFSET_SEC (3 * 3600)
#endif

// OLED ust bilgi cubugu (WiFi + saat); asil icerik bundan asagi.
const int OLED_STATUS_H = 9;

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
WebServer server(80);
Preferences prefs;

// Latest readings for web UI (updated together with OLED)
float g_ph = 0;
float g_ecUsCm = 0;
float g_tdsPpm = 0;
float g_tdsRaw = 0;
float g_tdsAdc = 0;
float g_tdsV = 0;
float g_phAdc = 0;
float g_phV = 0;
bool g_debugModeSnapshot = false;
bool g_wifiOk = false;
bool g_webStarted = false;
bool g_otaStarted = false;
bool g_relayStrip = false;
bool g_relayDrop = false;
bool g_relayAux = false;
bool g_lastRelayStrip = false;
bool g_lastRelayDrop = false;
bool g_lastRelayAux = false;
bool g_auxIsolationActive = false;
bool g_auxPrevStrip = false;
bool g_auxPrevDrop = false;
bool g_relayStateDirty = false;
char g_clockStr[6] = "--:--";
const int MAX_EVENT_LOGS = 16;
int g_eventLogCount = 0;
String g_eventLogs[MAX_EVENT_LOGS];

const float VREF = 3.3;          // ESP32 ADC reference voltage
const float ADC_MAX = 4095.0;    // 12-bit ADC
const float WATER_TEMP_C = 25.0; // Temperature compensation reference
const float DEFAULT_EC_US_PER_PPM = 2.0;
const float DEFAULT_PH_CAL_V4 = 3.300;
const float DEFAULT_PH_CAL_V7 = 2.528;
const float DEFAULT_PH_CAL_V10 = 2.012;
// Saha düzeltmesi fabrika varsayilani (el referans: pH 6, EC 1756 uS; ham ESP ~8.6 / ~2382 uS)
const float DEFAULT_PH_FIELD_SCALE = 1.0f;
const float DEFAULT_PH_FIELD_OFFSET = -2.60f;
const float DEFAULT_EC_FIELD_SCALE = 1756.0f / 2382.0f;
const float DEFAULT_EC_FIELD_OFFSET = 0.0f;

unsigned long lastUpdateMs = 0;
unsigned long lastSampleMs = 0;
unsigned long lastButtonMs = 0;
unsigned long lastWifiRetryMs = 0;
unsigned long lastNtpRetryMs = 0;
unsigned long bootMs = 0;
unsigned long lastRelayPersistMs = 0;
static bool s_httpRoutesRegistered = false;
static bool s_networkStackStarted = false;
bool g_bootRelayRestoreDone = false;
bool debugMode = false;
bool lastButtonState = HIGH;
bool forceRefresh = true;
bool haveSample = false;
const unsigned long SAMPLE_INTERVAL_MS = 1000;
// OLED: debug ekrani 1 sn'de bir; normal ekran da sensörle ayni hizda (web ile uyumlu veri).
const unsigned long DEBUG_INTERVAL_MS = 1000;

// Multi-point calibration table: RAW -> reference PPM
// Note: Last provided point looked inconsistent (raw almost same, ppm much higher),
// so it is intentionally excluded to keep mapping stable.
const int CAL_POINTS = 6;
const float DEFAULT_RAW_POINTS[CAL_POINTS] = {45, 183, 236, 469, 911, 1019};
const float DEFAULT_PPM_POINTS[CAL_POINTS] = {83, 218, 268, 487, 816, 992};
const float DEFAULT_EC_POINTS[CAL_POINTS] = {166, 436, 536, 974, 1632, 1984};

float cfgEcUsPerPpm = DEFAULT_EC_US_PER_PPM;
float cfgPhCalV4 = DEFAULT_PH_CAL_V4;
float cfgPhCalV7 = DEFAULT_PH_CAL_V7;
float cfgPhCalV10 = DEFAULT_PH_CAL_V10;
float cfgRawPoints[CAL_POINTS];
float cfgPpmPoints[CAL_POINTS];
float cfgEcPoints[CAL_POINTS];
// Ana sayfa scheduler ayarlari
int cfgWinEnabled = 0;           // saate gore ac/kapa aktif mi
int cfgWinRelay = 2;             // 0: serit, 1: damla, 2: role3
int cfgWinAtMin = 15 * 60;
const int MAX_SCHED_RULES = 64;
int gWinRuleCount = 0;
int gWinRuleRelay[MAX_SCHED_RULES];
int gWinRuleEnabled[MAX_SCHED_RULES];
int gWinRuleAtMin[MAX_SCHED_RULES];
int cfgCycleRelay = 2;
int cfgCycleEnabled = 1;  // Aralik icindeki durum: 1=Aktif(ON), 0=Pasif(OFF)
int cfgCycleStartMin = 0;
int cfgCycleEndMin = 24 * 60;
int cfgCyclePeriodMin = 120;
int cfgCycleDurMin = 10;
int gCycleRuleCount = 0;
int gCycleRuleRelay[MAX_SCHED_RULES];
int gCycleRuleEnabled[MAX_SCHED_RULES];
int gCycleRuleStartMin[MAX_SCHED_RULES];
int gCycleRuleEndMin[MAX_SCHED_RULES];
int gCycleRulePeriodMin[MAX_SCHED_RULES];
int gCycleRuleDurMin[MAX_SCHED_RULES];
const unsigned long MANUAL_OVERRIDE_MS = 15UL * 60UL * 1000UL;  // 15 dk
unsigned long g_manualOverrideUntilMs[3] = {0, 0, 0};
// Saha düzeltmesi (besin/kabin suyu; buffer sonrasi ince ayar). ph' = ph * phs + pho, ec' = ec * ecs + eco
float cfgPhScale = DEFAULT_PH_FIELD_SCALE;
float cfgPhOffset = DEFAULT_PH_FIELD_OFFSET;
float cfgEcScale = DEFAULT_EC_FIELD_SCALE;
float cfgEcOffset = DEFAULT_EC_FIELD_OFFSET;

static void loadSettings() {
  prefs.begin("hydro", true);
  cfgEcUsPerPpm = prefs.getFloat("ecf", DEFAULT_EC_US_PER_PPM);
  cfgPhCalV4 = prefs.getFloat("ph4", DEFAULT_PH_CAL_V4);
  cfgPhCalV7 = prefs.getFloat("ph7", DEFAULT_PH_CAL_V7);
  cfgPhCalV10 = prefs.getFloat("ph10", DEFAULT_PH_CAL_V10);
  for (int i = 0; i < CAL_POINTS; i++) {
    String rk = "r" + String(i);
    String pk = "p" + String(i);
    String ek = "e" + String(i);
    cfgRawPoints[i] = prefs.getFloat(rk.c_str(), DEFAULT_RAW_POINTS[i]);
    cfgPpmPoints[i] = prefs.getFloat(pk.c_str(), DEFAULT_PPM_POINTS[i]);
    cfgEcPoints[i] = prefs.getFloat(ek.c_str(), DEFAULT_EC_POINTS[i]);
  }
  cfgWinEnabled = prefs.getInt("swe", 0);
  cfgWinRelay = prefs.getInt("swr", 2);
  cfgWinAtMin = prefs.getInt("sws", 15 * 60);
  cfgCycleRelay = prefs.getInt("scr", 2);
  cfgCycleEnabled = prefs.getInt("sca", 1);
  cfgCycleStartMin = prefs.getInt("scs", 0);
  cfgCycleEndMin = prefs.getInt("sce", 24 * 60);
  cfgCyclePeriodMin = prefs.getInt("scp", 120);
  cfgCycleDurMin = prefs.getInt("scd", 10);
  g_lastRelayStrip = prefs.getBool("lrs", false);
  g_lastRelayDrop = prefs.getBool("lrd", false);
  g_lastRelayAux = prefs.getBool("lra", false);
  gWinRuleCount = prefs.getInt("wcnt", 0);
  if (gWinRuleCount < 0) gWinRuleCount = 0;
  if (gWinRuleCount > MAX_SCHED_RULES) gWinRuleCount = MAX_SCHED_RULES;
  for (int i = 0; i < gWinRuleCount; i++) {
    String k;
    k = "wr" + String(i); gWinRuleRelay[i] = prefs.getInt(k.c_str(), 2);
    k = "we" + String(i); gWinRuleEnabled[i] = prefs.getInt(k.c_str(), 1);
    k = "ws" + String(i); gWinRuleAtMin[i] = prefs.getInt(k.c_str(), 0);
  }
  gCycleRuleCount = prefs.getInt("ccnt", 0);
  if (gCycleRuleCount < 0) gCycleRuleCount = 0;
  if (gCycleRuleCount > MAX_SCHED_RULES) gCycleRuleCount = MAX_SCHED_RULES;
  for (int i = 0; i < gCycleRuleCount; i++) {
    String k;
    k = "cr" + String(i); gCycleRuleRelay[i] = prefs.getInt(k.c_str(), 2);
    k = "ca" + String(i); gCycleRuleEnabled[i] = prefs.getInt(k.c_str(), 1);
    k = "cs" + String(i); gCycleRuleStartMin[i] = prefs.getInt(k.c_str(), 0);
    k = "ce" + String(i); gCycleRuleEndMin[i] = prefs.getInt(k.c_str(), 24 * 60);
    k = "cp" + String(i); gCycleRulePeriodMin[i] = prefs.getInt(k.c_str(), 120);
    k = "cd" + String(i); gCycleRuleDurMin[i] = prefs.getInt(k.c_str(), 10);
  }
  g_eventLogCount = prefs.getInt("elc", 0);
  if (g_eventLogCount < 0) g_eventLogCount = 0;
  if (g_eventLogCount > MAX_EVENT_LOGS) g_eventLogCount = MAX_EVENT_LOGS;
  for (int i = 0; i < g_eventLogCount; i++) {
    String k = "el" + String(i);
    g_eventLogs[i] = prefs.getString(k.c_str(), "");
  }
  cfgPhScale = prefs.getFloat("phs", DEFAULT_PH_FIELD_SCALE);
  cfgPhOffset = prefs.getFloat("pho", DEFAULT_PH_FIELD_OFFSET);
  cfgEcScale = prefs.getFloat("ecs", DEFAULT_EC_FIELD_SCALE);
  cfgEcOffset = prefs.getFloat("eco", DEFAULT_EC_FIELD_OFFSET);
  prefs.end();
}

static void saveSettings() {
  prefs.begin("hydro", false);
  prefs.putFloat("ecf", cfgEcUsPerPpm);
  prefs.putFloat("ph4", cfgPhCalV4);
  prefs.putFloat("ph7", cfgPhCalV7);
  prefs.putFloat("ph10", cfgPhCalV10);
  for (int i = 0; i < CAL_POINTS; i++) {
    String rk = "r" + String(i);
    String pk = "p" + String(i);
    String ek = "e" + String(i);
    prefs.putFloat(rk.c_str(), cfgRawPoints[i]);
    prefs.putFloat(pk.c_str(), cfgPpmPoints[i]);
    prefs.putFloat(ek.c_str(), cfgEcPoints[i]);
  }
  prefs.putInt("swe", cfgWinEnabled);
  prefs.putInt("swr", cfgWinRelay);
  prefs.putInt("sws", cfgWinAtMin);
  prefs.putInt("scr", cfgCycleRelay);
  prefs.putInt("sca", cfgCycleEnabled);
  prefs.putInt("scs", cfgCycleStartMin);
  prefs.putInt("sce", cfgCycleEndMin);
  prefs.putInt("scp", cfgCyclePeriodMin);
  prefs.putInt("scd", cfgCycleDurMin);
  prefs.putBool("lrs", g_lastRelayStrip);
  prefs.putBool("lrd", g_lastRelayDrop);
  prefs.putBool("lra", g_lastRelayAux);
  prefs.putInt("wcnt", gWinRuleCount);
  for (int i = 0; i < gWinRuleCount; i++) {
    String k;
    k = "wr" + String(i); prefs.putInt(k.c_str(), gWinRuleRelay[i]);
    k = "we" + String(i); prefs.putInt(k.c_str(), gWinRuleEnabled[i]);
    k = "ws" + String(i); prefs.putInt(k.c_str(), gWinRuleAtMin[i]);
  }
  prefs.putInt("ccnt", gCycleRuleCount);
  for (int i = 0; i < gCycleRuleCount; i++) {
    String k;
    k = "cr" + String(i); prefs.putInt(k.c_str(), gCycleRuleRelay[i]);
    k = "ca" + String(i); prefs.putInt(k.c_str(), gCycleRuleEnabled[i]);
    k = "cs" + String(i); prefs.putInt(k.c_str(), gCycleRuleStartMin[i]);
    k = "ce" + String(i); prefs.putInt(k.c_str(), gCycleRuleEndMin[i]);
    k = "cp" + String(i); prefs.putInt(k.c_str(), gCycleRulePeriodMin[i]);
    k = "cd" + String(i); prefs.putInt(k.c_str(), gCycleRuleDurMin[i]);
  }
  prefs.putFloat("phs", cfgPhScale);
  prefs.putFloat("pho", cfgPhOffset);
  prefs.putFloat("ecs", cfgEcScale);
  prefs.putFloat("eco", cfgEcOffset);
  prefs.end();
}

static void applyDefaultSettings() {
  cfgEcUsPerPpm = DEFAULT_EC_US_PER_PPM;
  cfgPhCalV4 = DEFAULT_PH_CAL_V4;
  cfgPhCalV7 = DEFAULT_PH_CAL_V7;
  cfgPhCalV10 = DEFAULT_PH_CAL_V10;
  for (int i = 0; i < CAL_POINTS; i++) {
    cfgRawPoints[i] = DEFAULT_RAW_POINTS[i];
    cfgPpmPoints[i] = DEFAULT_PPM_POINTS[i];
    cfgEcPoints[i] = DEFAULT_EC_POINTS[i];
  }
  cfgWinEnabled = 0;
  cfgWinRelay = 2;
  cfgWinAtMin = 15 * 60;
  cfgCycleRelay = 2;
  cfgCycleEnabled = 1;
  cfgCycleStartMin = 0;
  cfgCycleEndMin = 24 * 60;
  cfgCyclePeriodMin = 120;
  cfgCycleDurMin = 10;
  g_lastRelayStrip = false;
  g_lastRelayDrop = false;
  g_lastRelayAux = false;
  gWinRuleCount = 0;
  gCycleRuleCount = 0;
  g_eventLogCount = 0;
  cfgPhScale = DEFAULT_PH_FIELD_SCALE;
  cfgPhOffset = DEFAULT_PH_FIELD_OFFSET;
  cfgEcScale = DEFAULT_EC_FIELD_SCALE;
  cfgEcOffset = DEFAULT_EC_FIELD_OFFSET;
}

static inline uint8_t relayLevel(bool on) {
#if RELAY_ACTIVE_LOW
  return on ? LOW : HIGH;
#else
  return on ? HIGH : LOW;
#endif
}

static void saveRelayStates() {
  prefs.begin("hydro", false);
  prefs.putBool("lrs", g_lastRelayStrip);
  prefs.putBool("lrd", g_lastRelayDrop);
  prefs.putBool("lra", g_lastRelayAux);
  prefs.end();
  g_relayStateDirty = false;
  lastRelayPersistMs = millis();
}

static void saveEventLogsToNvs() {
  prefs.begin("hydro", false);
  prefs.putInt("elc", g_eventLogCount);
  for (int i = 0; i < g_eventLogCount; i++) {
    String k = "el" + String(i);
    prefs.putString(k.c_str(), g_eventLogs[i]);
  }
  for (int i = g_eventLogCount; i < MAX_EVENT_LOGS; i++) {
    String k = "el" + String(i);
    prefs.remove(k.c_str());
  }
  prefs.end();
}

static void addScenarioLog(const String &msg) {
  time_t now = time(nullptr);
  String line;
  if (now > 1700000000) {
    struct tm ti;
    localtime_r(&now, &ti);
    char tbuf[9];
    snprintf(tbuf, sizeof(tbuf), "%02d:%02d:%02d", ti.tm_hour, ti.tm_min, ti.tm_sec);
    line = "[" + String(tbuf) + "] " + msg;
  } else {
    line = "[" + String(millis() / 1000) + "s] " + msg;
  }

  if (g_eventLogCount < MAX_EVENT_LOGS) {
    g_eventLogs[g_eventLogCount++] = line;
  } else {
    for (int i = 1; i < MAX_EVENT_LOGS; i++) g_eventLogs[i - 1] = g_eventLogs[i];
    g_eventLogs[MAX_EVENT_LOGS - 1] = line;
  }
  saveEventLogsToNvs();
}

static void setRelayStrip(bool on) {
  if (g_relayStrip == on) return;
  digitalWrite(RELAY_STRIP_PIN, relayLevel(on));
  g_relayStrip = on;
  g_lastRelayStrip = on;
  g_relayStateDirty = true;
}

static void setRelayDrop(bool on) {
  if (g_relayDrop == on) return;
  digitalWrite(RELAY_DROP_PIN, relayLevel(on));
  g_relayDrop = on;
  g_lastRelayDrop = on;
  g_relayStateDirty = true;
}

static void setRelayAux(bool on) {
  if (g_relayAux == on) return;
  digitalWrite(RELAY_AUX_PIN, relayLevel(on));
  g_relayAux = on;
  g_lastRelayAux = on;
  g_relayStateDirty = true;
}

static bool relayStateById(int relayId) {
  if (relayId == 0) return g_relayStrip;
  if (relayId == 1) return g_relayDrop;
  return g_relayAux;
}

static void setRelayById(int relayId, bool on) {
  if (relayId == 0) setRelayStrip(on);
  else if (relayId == 1) setRelayDrop(on);
  else setRelayAux(on);
}

// Pompa (Role 3) acildiginda diger roleleri gecici kapat; pompa bitince eski durumlara don.
static void applyAuxIsolationRule(bool auxOn) {
  if (auxOn) {
    if (!g_auxIsolationActive) {
      g_auxPrevStrip = g_relayStrip;
      g_auxPrevDrop = g_relayDrop;
      g_auxIsolationActive = true;
      addScenarioLog(String("Senaryo calisti: Su Mot. ON, SunLig=") + (g_auxPrevStrip ? "ON" : "OFF") + ", GrowLig=" + (g_auxPrevDrop ? "ON" : "OFF"));
    }
    if (g_relayStrip) setRelayStrip(false);
    if (g_relayDrop) setRelayDrop(false);
    if (!g_relayAux) setRelayAux(true);
    return;
  }

  if (g_relayAux) setRelayAux(false);
  if (!g_auxIsolationActive) return;

  if (g_relayStrip != g_auxPrevStrip) setRelayStrip(g_auxPrevStrip);
  if (g_relayDrop != g_auxPrevDrop) setRelayDrop(g_auxPrevDrop);
  g_auxIsolationActive = false;
  addScenarioLog(String("Senaryo bitti: Su Mot. OFF, SunLig=") + (g_relayStrip ? "ON" : "OFF") + ", GrowLig=" + (g_relayDrop ? "ON" : "OFF"));
}

static bool isManualOverrideActive(int relayId) {
  if (relayId < 0 || relayId > 2) return false;
  unsigned long untilMs = g_manualOverrideUntilMs[relayId];
  if (untilMs == 0) return false;
  if ((long)(untilMs - millis()) > 0) return true;
  g_manualOverrideUntilMs[relayId] = 0;
  return false;
}

static void persistRelayStatesIfNeeded() {
  // NVS'ye her role degisiminde aninda yazmak bazen sistemi kasar.
  // Degisiklikleri gecikmeli toplu yazarak kilitlenme riskini azaltiriz.
  if (!g_relayStateDirty) return;
  if (millis() - lastRelayPersistMs < 1500) return;
  saveRelayStates();
}

// Sirali baslatma: iki LED ayni anda enerjilenince PSU dalgalanmasi / hata olmasin.
static void sequentialStartLeds() {
  if (!g_relayStrip) {
    setRelayStrip(true);
    Serial.println(F("LED: serit ON"));
    delay(RELAY_SEQ_DELAY_MS);
  }
  if (!g_relayDrop) {
    setRelayDrop(true);
    Serial.println(F("LED: damla ON"));
    delay(RELAY_SEQ_DELAY_MS);
  }
  if (!g_relayAux) {
    setRelayAux(true);
    Serial.println(F("LED: role3 ON"));
  }
}

static void allLedsOff() {
  setRelayAux(false);
  delay(50);
  setRelayDrop(false);
  delay(50);
  setRelayStrip(false);
  Serial.println(F("LED: hepsi OFF"));
}

static void restoreLastRelayStatesSequentially() {
  if (g_bootRelayRestoreDone) return;
  g_bootRelayRestoreDone = true;
  Serial.println(F("Boot restore: roleler son duruma sirali alinacak"));
  if (g_lastRelayStrip) {
    setRelayStrip(true);
    delay(RELAY_SEQ_DELAY_MS);
  }
  if (g_lastRelayDrop) {
    setRelayDrop(true);
    delay(RELAY_SEQ_DELAY_MS);
  }
  if (g_lastRelayAux) {
    setRelayAux(true);
  }
}

static int wifiSignalBars() {
  if (WiFi.status() != WL_CONNECTED) return 0;
  int r = WiFi.RSSI();
  if (r > -55) return 3;
  if (r > -67) return 2;
  if (r > -80) return 1;
  return 0;
}

static String relayNameById(int relayId) {
  if (relayId == 0) return "SunLig";
  if (relayId == 1) return "GrowLig";
  return "Su Mot.";
}

static bool isInWindowMinutes(int nowMin, int startMin, int endMin) {
  startMin %= (24 * 60);
  endMin %= (24 * 60);
  if (startMin == endMin) return true;
  if (startMin < endMin) return nowMin >= startMin && nowMin < endMin;
  return nowMin >= startMin || nowMin < endMin;
}

static void drawOledStatusBar() {
  const int baselineY = OLED_STATUS_H - 1;
  const uint8_t bw = 2;
  const uint8_t gap = 1;
  int lvl = wifiSignalBars();
  int x = 0;
  for (int i = 0; i < 3; i++) {
    uint8_t h = 2 + (uint8_t)i * 2;
    int y = baselineY - (int)h;
    if (i < lvl) {
      display.fillRect(x, y, bw, h, SSD1306_WHITE);
    } else {
      display.drawRect(x, y, bw, h, SSD1306_WHITE);
    }
    x += bw + gap;
  }

  display.setTextSize(1);
  int16_t x1, y1;
  uint16_t tw, th;
  display.getTextBounds(g_clockStr, 0, 0, &x1, &y1, &tw, &th);
  display.setCursor(SCREEN_WIDTH - (int)tw, 0);
  display.setTextColor(SSD1306_WHITE);
  display.print(g_clockStr);
}

float readTdsPpm(float &avgAdcOut, float &voltageOut) {
  const int sampleCount = 20;
  uint32_t total = 0;

  for (int i = 0; i < sampleCount; i++) {
    total += analogRead(TDS_PIN);
    delay(5);
  }

  float avgAdc = total / (float)sampleCount;
  float voltage = avgAdc * VREF / ADC_MAX;
  avgAdcOut = avgAdc;
  voltageOut = voltage;

  // Standard TDS compensation formula used by common analog TDS boards
  float compensationCoefficient = 1.0 + 0.02 * (WATER_TEMP_C - 25.0);
  float compensationVoltage = voltage / compensationCoefficient;
  float tds = (133.42 * compensationVoltage * compensationVoltage * compensationVoltage
               - 255.86 * compensationVoltage * compensationVoltage
               + 857.39 * compensationVoltage) * 0.5;

  return tds;
}

float rawToPpmCalibrated(float rawValue) {
  if (rawValue <= cfgRawPoints[0]) {
    // Extrapolate below first point instead of clamping, so low readings can change
    float x1 = cfgRawPoints[0];
    float x2 = cfgRawPoints[1];
    float y1 = cfgPpmPoints[0];
    float y2 = cfgPpmPoints[1];
    float t = (rawValue - x1) / (x2 - x1);
    float ppm = y1 + t * (y2 - y1);
    return ppm < 0 ? 0 : ppm;
  }

  for (int i = 0; i < CAL_POINTS - 1; i++) {
    float x1 = cfgRawPoints[i];
    float x2 = cfgRawPoints[i + 1];
    float y1 = cfgPpmPoints[i];
    float y2 = cfgPpmPoints[i + 1];

    if (rawValue >= x1 && rawValue <= x2) {
      float t = (rawValue - x1) / (x2 - x1);
      return y1 + t * (y2 - y1);
    }
  }

  // Slight extrapolation above last point
  float x1 = cfgRawPoints[CAL_POINTS - 2];
  float x2 = cfgRawPoints[CAL_POINTS - 1];
  float y1 = cfgPpmPoints[CAL_POINTS - 2];
  float y2 = cfgPpmPoints[CAL_POINTS - 1];
  float t = (rawValue - x1) / (x2 - x1);
  float ppm = y1 + t * (y2 - y1);
  return ppm < 0 ? 0 : ppm;
}

float rawToEcCalibrated(float rawValue) {
  if (rawValue <= cfgRawPoints[0]) {
    float x1 = cfgRawPoints[0];
    float x2 = cfgRawPoints[1];
    float y1 = cfgEcPoints[0];
    float y2 = cfgEcPoints[1];
    float t = (rawValue - x1) / (x2 - x1);
    float ec = y1 + t * (y2 - y1);
    return ec < 0 ? 0 : ec;
  }

  for (int i = 0; i < CAL_POINTS - 1; i++) {
    float x1 = cfgRawPoints[i];
    float x2 = cfgRawPoints[i + 1];
    float y1 = cfgEcPoints[i];
    float y2 = cfgEcPoints[i + 1];

    if (rawValue >= x1 && rawValue <= x2) {
      float t = (rawValue - x1) / (x2 - x1);
      return y1 + t * (y2 - y1);
    }
  }

  float x1 = cfgRawPoints[CAL_POINTS - 2];
  float x2 = cfgRawPoints[CAL_POINTS - 1];
  float y1 = cfgEcPoints[CAL_POINTS - 2];
  float y2 = cfgEcPoints[CAL_POINTS - 1];
  float t = (rawValue - x1) / (x2 - x1);
  float ec = y1 + t * (y2 - y1);
  return ec < 0 ? 0 : ec;
}

float readPhValue(float &phAdcOut, float &phVoltageOut) {
  const int sampleCount = 20;
  uint32_t total = 0;

  for (int i = 0; i < sampleCount; i++) {
    total += analogRead(PH_PIN);
    delay(5);
  }

  float avgAdc = total / (float)sampleCount;
  float voltage = avgAdc * VREF / ADC_MAX;
  // Piecewise linear calibration:
  // segment-1: pH 4..7 (V4 to V7), segment-2: pH 7..10 (V7 to V10)
  float ph;
  if (voltage >= cfgPhCalV7) {
    float m1 = (7.0 - 4.0) / (cfgPhCalV7 - cfgPhCalV4);
    float b1 = 7.0 - m1 * cfgPhCalV7;
    ph = m1 * voltage + b1;
  } else {
    float m2 = (10.0 - 7.0) / (cfgPhCalV10 - cfgPhCalV7);
    float b2 = 7.0 - m2 * cfgPhCalV7;
    ph = m2 * voltage + b2;
  }

  phAdcOut = avgAdc;
  phVoltageOut = voltage;

  if (ph < 0) return 0;
  if (ph > 14) return 14;
  return ph;
}

static int clampMinuteOfDay(int m) {
  if (m < 0) return 0;
  if (m > 24 * 60) return 24 * 60;
  return m;
}

static int parseTimeArgToMinutes(const String &s, int fallback) {
  int colon = s.indexOf(':');
  if (colon <= 0) return fallback;
  int hh = s.substring(0, colon).toInt();
  int mm = s.substring(colon + 1).toInt();
  if (hh < 0 || hh > 24 || mm < 0 || mm > 59) return fallback;
  if (hh == 24 && mm != 0) return fallback;
  return hh * 60 + mm;
}

static String formatMinutesToTime(int totalMin) {
  totalMin = clampMinuteOfDay(totalMin);
  int hh = totalMin / 60;
  int mm = totalMin % 60;
  if (hh == 24) hh = 0;
  char buf[6];
  snprintf(buf, sizeof(buf), "%02d:%02d", hh, mm);
  return String(buf);
}

static void applyAuxSchedulerIfNeeded() {
  time_t now = time(nullptr);
  if (now <= 1700000000) return;  // saat senkron degilse roleye dokunma
  struct tm ti;
  localtime_r(&now, &ti);
  int nowMin = ti.tm_hour * 60 + ti.tm_min;
  for (int i = 0; i < gWinRuleCount; i++) {
    int relay = constrain(gWinRuleRelay[i], 0, 2);
    if (nowMin != gWinRuleAtMin[i]) continue;
    if (isManualOverrideActive(relay)) continue;
    bool desired = gWinRuleEnabled[i] != 0;  // Aktif=ON, Pasif=OFF
    if (relay == 2) {
      applyAuxIsolationRule(desired);
    } else if (relayStateById(relay) != desired) {
      setRelayById(relay, desired);
      addScenarioLog(String("Saate Gore: ") + relayNameById(relay) + " -> " + (desired ? "ON" : "OFF"));
    }
  }

  for (int i = 0; i < gCycleRuleCount; i++) {
    int relay = constrain(gCycleRuleRelay[i], 0, 2);
    if (isManualOverrideActive(relay)) continue;
    int elapsed = nowMin;  // Araliga gore senaryo: gun basindan itibaren periyot
    int per = gCycleRulePeriodMin[i] < 1 ? 1 : gCycleRulePeriodMin[i];
    int dur = gCycleRuleDurMin[i] < 0 ? 0 : gCycleRuleDurMin[i];
    if (dur > per) dur = per;
    bool inPulse = (elapsed % per) < dur;
    bool activeState = (gCycleRuleEnabled[i] != 0);  // Aktif=ON, Pasif=OFF
    bool desired = inPulse ? activeState : !activeState;
    if (relay == 2) {
      applyAuxIsolationRule(desired);
    } else if (relayStateById(relay) != desired) {
      setRelayById(relay, desired);
      addScenarioLog(String("Araliga Gore: ") + relayNameById(relay) + " -> " + (desired ? "ON" : "OFF"));
    }
  }
}

void handleRoot() {
  // Web gorunumu OLED'den bagimsiz: ?view=normal | ?view=debug
  String view = server.hasArg("view") ? server.arg("view") : "normal";
  view.toLowerCase();
  bool webDebug = (view == "debug");
  int refreshSec = webDebug ? 1 : 0;
  String viewNext = webDebug ? "debug" : "normal";

  String html;
  html.reserve(2200);
  html += F("<!DOCTYPE html><html><head><meta charset=\"utf-8\">");
  html += F("<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">");
  if (refreshSec > 0) {
    html += F("<meta http-equiv=\"refresh\" content=\"");
    html += String(refreshSec);
    html += F(";url=/?view=");
    html += viewNext;
    html += F("\">");
  }
  html += F("<title>Hydro Monitor</title>");
  html += F("<style>");
  html += F("body{font-family:system-ui,sans-serif;margin:1rem auto;padding:0 0.5rem;background:#0f172a;color:#e2e8f0;max-width:62rem;}");
  html += F(".nav{display:flex;justify-content:flex-end;gap:0.35rem;margin-bottom:0.55rem;}");
  html += F(".nav a{text-align:center;padding:0.32rem 0.5rem;border-radius:0.45rem;text-decoration:none;font-weight:600;font-size:0.72rem;min-width:4.4rem;}");
  html += F(".nav a.on{background:#3b82f6;color:#fff;}");
  html += F(".nav a.off{background:#334155;color:#cbd5e1;}");
  html += F(".ok{color:#4ade80;font-size:0.8rem;margin-bottom:0.75rem;} .bad{color:#f87171;font-size:0.8rem;margin-bottom:0.75rem;}");
  html += F(".n-ph{font-size:1.85rem;font-weight:700;margin:0.35rem 0 0.15rem;} .n-ec{font-size:1.85rem;font-weight:700;margin:0.35rem 0;}");
  html += F(".n-lab{color:#94a3b8;font-size:0.95rem;}");
  html += F(".n-grid{display:grid;grid-template-columns:1fr 1fr;gap:0.55rem;margin-top:0.25rem;}");
  html += F(".n-item{background:#1e293b;border-radius:0.5rem;padding:0.55rem 0.6rem;}");
  html += F(".dbg-grid{display:grid;grid-template-columns:repeat(3,minmax(0,1fr));gap:0.55rem;margin-top:0.25rem;}");
  html += F(".dbg-card{background:#1e293b;border-radius:0.5rem;padding:0.55rem 0.6rem;}");
  html += F(".dbg-title{font-size:0.78rem;color:#94a3b8;margin-bottom:0.35rem;}");
  html += F(".dbg-val{font-size:1.18rem;font-weight:700;line-height:1.2;margin:0.08rem 0;}");
  html += F(".dbg-sub{font-size:0.82rem;color:#cbd5e1;line-height:1.3;margin:0.08rem 0;}");
  html += F(".dbg-log{margin-top:0.55rem;background:#1e293b;border-radius:0.5rem;padding:0.55rem 0.6rem;}");
  html += F(".dbg-log h4{margin:0 0 0.35rem;font-size:0.83rem;color:#cbd5e1;}");
  html += F(".dbg-log-item{font-size:0.78rem;color:#cbd5e1;line-height:1.35;padding:0.14rem 0;border-bottom:1px solid #243246;}");
  html += F(".dbg-log-item:last-child{border-bottom:none;}");
  html += F(".d-row{margin:0.35rem 0;font-size:1rem;line-height:1.35;} .d-lab{color:#94a3b8;display:inline-block;min-width:4.2rem;}");
  html += F(".hint{color:#64748b;font-size:0.78rem;margin-top:1rem;line-height:1.35;}");
  html += F(".led{margin-top:1rem;padding:0.7rem;background:#1e293b;border-radius:0.55rem;}");
  html += F(".led h3{margin:0;font-size:0.95rem;color:#cbd5e1;}");
  html += F(".led-head{display:flex;align-items:center;justify-content:space-between;margin-bottom:0.55rem;}");
  html += F(".led-head-actions{display:flex;gap:0.35rem;}");
  html += F(".led-grid{display:grid;grid-template-columns:repeat(3,minmax(0,1fr));gap:0.7rem;align-items:start;}");
  html += F(".led-card{width:100%;max-width:10.2rem;aspect-ratio:1/1;background:#0f172a;border:1px solid #334155;border-radius:0.5rem;padding:0.45rem;display:flex;flex-direction:column;justify-content:space-between;}");
  html += F(".led-card:nth-child(1){justify-self:start;} .led-card:nth-child(2){justify-self:center;} .led-card:nth-child(3){justify-self:end;}");
  html += F(".led-name{font-size:0.82rem;color:#cbd5e1;margin-bottom:0.3rem;display:flex;align-items:center;}");
  html += F(".led-actions{display:flex;gap:0.35rem;width:100%;}");
  html += F(".led-row{display:flex;align-items:center;justify-content:space-between;margin:0.3rem 0;font-size:0.9rem;}");
  html += F(".dot{display:inline-block;width:0.65rem;height:0.65rem;border-radius:50%;margin-right:0.4rem;vertical-align:middle;}");
  html += F(".dot-on{background:#22c55e;box-shadow:0 0 4px #22c55e;} .dot-off{background:#475569;}");
  html += F(".led-btn{display:inline-block;min-width:4.2rem;text-align:center;padding:0.35rem 0.7rem;border-radius:0.4rem;text-decoration:none;font-size:0.82rem;font-weight:600;}");
  html += F(".led-actions .led-btn{flex:1;min-width:0;padding:0.32rem 0.25rem;}");
  html += F(".b-on{background:#16a34a;color:#fff;} .b-off{background:#64748b;color:#fff;} .b-seq{background:#2563eb;color:#fff;}");
  html += F(".led-all{display:flex;gap:0.5rem;margin-top:0.55rem;} .led-all a{flex:1;text-align:center;}");
  html += F(".sched{margin-top:1rem;padding:0.7rem;background:#1e293b;border-radius:0.55rem;}");
  html += F(".sched h3{margin:0 0 0.5rem;font-size:0.95rem;color:#cbd5e1;}");
  html += F(".sched form{margin-top:0.4rem;}");
  html += F(".sched .g{display:flex;flex-wrap:nowrap;justify-content:space-between;gap:0.45rem;align-items:flex-end;width:100%;}");
  html += F(".sched .g>div{display:flex;flex-direction:column;}");
  html += F(".sched label{font-size:0.78rem;color:#94a3b8;display:block;}");
  html += F(".sched select,.sched input{width:100%;max-width:10rem;padding:0.33rem;border-radius:0.4rem;border:1px solid #334155;background:#111827;color:#e2e8f0;font-size:0.82rem;}");
  html += F(".sched input.hhmm{background:#111827;color:#e2e8f0;border:1px solid #334155;border-radius:0.4rem;min-height:2.05rem;}");
  html += F(".sched button{padding:0.45rem 0.7rem;border:none;border-radius:0.4rem;background:#2563eb;color:#fff;font-weight:600;}");
  html += F(".sum{margin-top:0.7rem;padding:0.7rem;background:#1e293b;border-radius:0.55rem;}");
  html += F(".sum h3{margin:0 0 0.45rem;font-size:0.95rem;color:#cbd5e1;}");
  html += F(".sum .r{font-size:0.82rem;color:#cbd5e1;line-height:1.45;margin:0.15rem 0;}");
  html += F(".rule-line,.rule-line-cycle,.cycle-create,.cycle-create-mid{display:flex;flex-wrap:nowrap;justify-content:space-between;align-items:center;gap:0.45rem;width:100%;}");
  html += F(".rule-line{margin:0.25rem 0 0.55rem;} .rule-line-cycle{margin:0.25rem 0 0.55rem;} .cycle-create{margin:0.35rem 0 0.6rem;}");
  html += F(".rule-line select,.rule-line input,.rule-line-cycle select,.rule-line-cycle input,.cycle-create select,.cycle-create input,.sched .g select,.sched .g input{width:5.1rem;max-width:5.1rem;height:2.05rem;min-height:2.05rem;padding:0 0.45rem;border-radius:0.4rem;border:1px solid #334155;background:#111827;color:#e2e8f0;font-size:0.82rem;box-sizing:border-box;}");
  html += F(".rule-line button,.rule-line-cycle button,.cycle-create button,.sched .g button{width:5.1rem;max-width:5.1rem;height:2.05rem;min-height:2.05rem;padding:0 0.45rem;border:none;border-radius:0.4rem;color:#fff;font-size:0.8rem;box-sizing:border-box;}");
  html += F(".rule-line .w-time,.rule-line-cycle .w-time,.cycle-create .w-time,.rule-line .w-num,.rule-line-cycle .w-num,.cycle-create .w-num{text-align:center;}");
  html += F(".rule-line .w-relay,.rule-line-cycle .w-relay,.cycle-create .w-relay,.rule-line .w-time,.rule-line-cycle .w-time,.cycle-create .w-time,.rule-line .w-num,.rule-line-cycle .w-num,.cycle-create .w-num{min-width:0;}");
  html += F(".btn-save{background:#2563eb;} .btn-del{background:#b91c1c;}");
  html += F("</style></head><body>");

  html += F("<div class=\"nav\">");
  if (webDebug) {
    html += F("<a class=\"off\" href=\"/?view=normal\">Normal</a>");
    html += F("<a class=\"on\" href=\"/?view=debug\">Debug</a>");
  } else {
    html += F("<a class=\"on\" href=\"/?view=normal\">Normal</a>");
    html += F("<a class=\"off\" href=\"/?view=debug\">Debug</a>");
  }
  html += F("</div>");

  if (g_wifiOk) {
    html += F("<div class=\"ok\">WiFi bagli</div>");
  } else {
    html += F("<div class=\"bad\">WiFi yok</div>");
  }
  html += F("<div style=\"color:#94a3b8;font-size:0.85rem;margin:0.15rem 0 0.5rem;\">Saat: ");
  html += g_clockStr;
  html += F(" (NTP, TRT)</div>");

  if (!webDebug) {
    html += F("<div class=\"n-grid\">");
    html += F("<div class=\"n-item\"><div class=\"n-lab\">pH</div><div class=\"n-ph\">");
    html += String(g_ph, 2);
    html += F("</div></div>");
    html += F("<div class=\"n-item\"><div class=\"n-lab\">EC</div><div class=\"n-ec\">");
    html += String((int)g_ecUsCm);
    html += F(" uS/cm</div></div></div>");
    html += F("<p class=\"hint\">Web: sadece bu sayfa (OLED'i degistirmez). Yenileme 3 sn.</p>");
    html += F("<p class=\"hint\">OLED cihaz: ");
    html += g_debugModeSnapshot ? F("DEBUG") : F("NORMAL");
    html += F(" (buton)</p>");
  } else {
    uint32_t heapTotalKb = ESP.getHeapSize() / 1024;
    uint32_t heapFreeKb = ESP.getFreeHeap() / 1024;
    uint32_t heapUsedKb = heapTotalKb > heapFreeKb ? (heapTotalKb - heapFreeKb) : 0;
    html += F("<div class=\"dbg-grid\">");
    html += F("<div class=\"dbg-card\"><div class=\"dbg-title\">pH / pV</div>");
    html += F("<div class=\"dbg-val\">"); html += String(g_ph, 2); html += F("</div>");
    html += F("<div class=\"dbg-sub\">pV: "); html += String(g_phV, 3);
    html += F(" | pA: "); html += String((int)g_phAdc); html += F("</div></div>");
    html += F("<div class=\"dbg-card\"><div class=\"dbg-title\">EC / TDS</div>");
    html += F("<div class=\"dbg-val\">"); html += String((int)g_ecUsCm); html += F(" uS</div>");
    html += F("<div class=\"dbg-sub\">TDS: "); html += String((int)g_tdsPpm); html += F(" ppm</div></div>");
    html += F("<div class=\"dbg-card\"><div class=\"dbg-title\">RAW</div>");
    html += F("<div class=\"dbg-val\">"); html += String((int)g_tdsRaw); html += F("</div>");
    html += F("<div class=\"dbg-sub\">Heap: ");
    html += String(heapTotalKb);
    html += F("KB / ");
    html += String(heapUsedKb);
    html += F("KB (mevcut/kullanilan)</div></div></div>");
    html += F("<div class=\"dbg-log\"><h4>Senaryo Log</h4>");
    if (g_eventLogCount == 0) {
      html += F("<div class=\"dbg-log-item\">- kayit yok</div>");
    } else {
      for (int i = g_eventLogCount - 1; i >= 0; i--) {
        html += F("<div class=\"dbg-log-item\">");
        html += g_eventLogs[i];
        html += F("</div>");
      }
    }
    html += F("</div>");
    html += F("<div class=\"d-row\" style=\"margin-top:0.5rem;color:#64748b;font-size:0.85rem;\">OLED cihaz: ");
    html += g_debugModeSnapshot ? F("DEBUG") : F("NORMAL");
    html += F(" (buton)</div>");
    html += F("<p class=\"hint\">Web: sadece bu sayfa. Yenileme 1 sn.</p>");
  }

  if (!webDebug) {
  html += F("<div class=\"led\">");
  html += F("<div class=\"led-head\"><h3>LED Kontrol</h3><div class=\"led-head-actions\">");
  html += F("<a class=\"led-btn b-seq\" href=\"/led?all=on\">On</a>");
  html += F("<a class=\"led-btn b-off\" href=\"/led?all=off\">Off</a>");
  html += F("</div></div>");
  html += F("<div class=\"led-grid\">");

  html += F("<div class=\"led-card\"><div class=\"led-name\"><span class=\"dot ");
  html += g_relayStrip ? F("dot-on") : F("dot-off");
  html += F("\"></span>SunLig</div><div class=\"led-actions\">");
  html += F("<a class=\"led-btn b-on\" href=\"/led?t=strip&s=on\">On</a>");
  html += F("<a class=\"led-btn b-off\" href=\"/led?t=strip&s=off\">Off</a>");
  html += F("</div></div>");

  html += F("<div class=\"led-card\"><div class=\"led-name\"><span class=\"dot ");
  html += g_relayDrop ? F("dot-on") : F("dot-off");
  html += F("\"></span>GrowLig</div><div class=\"led-actions\">");
  html += F("<a class=\"led-btn b-on\" href=\"/led?t=drop&s=on\">On</a>");
  html += F("<a class=\"led-btn b-off\" href=\"/led?t=drop&s=off\">Off</a>");
  html += F("</div></div>");

  html += F("<div class=\"led-card\"><div class=\"led-name\"><span class=\"dot ");
  html += g_relayAux ? F("dot-on") : F("dot-off");
  html += F("\"></span>Su Mot.</div><div class=\"led-actions\">");
  html += F("<a class=\"led-btn b-on\" href=\"/led?t=aux&s=on\">On</a>");
  html += F("<a class=\"led-btn b-off\" href=\"/led?t=aux&s=off\">Off</a>");
  html += F("</div></div>");
  html += F("</div></div>");

  html += F("<div class=\"sched\"><h3>Saate Gore</h3><form method=\"POST\" action=\"/sched/save\">");
  html += F("<input type=\"hidden\" name=\"type\" value=\"win\">");
  html += F("<div class=\"g\">");
  html += F("<div><label>Role</label><select name=\"relay\">");
  html += F("<option value=\"0\"");
  if (cfgWinRelay == 0) html += F(" selected");
  html += F(">SunLig</option><option value=\"1\"");
  if (cfgWinRelay == 1) html += F(" selected");
  html += F(">GrowLig</option><option value=\"2\"");
  if (cfgWinRelay == 2) html += F(" selected");
  html += F(">Su Mot.</option></select></div>");
  html += F("<div><label>Durum</label><select name=\"enabled\"><option value=\"0\"");
  if (!cfgWinEnabled) html += F(" selected");
  html += F(">Pasif</option><option value=\"1\"");
  if (cfgWinEnabled) html += F(" selected");
  html += F(">Aktif</option></select></div>");
  html += F("<div><label>Saat</label><input type=\"text\" class=\"hhmm\" name=\"start\" maxlength=\"5\" placeholder=\"HH:MM\" value=\"");
  html += formatMinutesToTime(cfgWinAtMin);
  html += F("\"></div>");
  html += F("<div><label>&nbsp;</label><button type=\"submit\">Kaydet</button></div>");
  html += F("</div></form></div>");

  html += F("<div class=\"sched\"><h3>Araliga Gore</h3><form method=\"POST\" action=\"/sched/save\">");
  html += F("<input type=\"hidden\" name=\"type\" value=\"cycle\">");
  html += F("<div class=\"cycle-create\">");
  html += F("<div><label>Role</label><select name=\"relay\" class=\"w-relay\">");
  html += F("<option value=\"0\"");
  if (cfgCycleRelay == 0) html += F(" selected");
  html += F(">SunLig</option><option value=\"1\"");
  if (cfgCycleRelay == 1) html += F(" selected");
  html += F(">GrowLig</option><option value=\"2\"");
  if (cfgCycleRelay == 2) html += F(" selected");
  html += F(">Su Mot.</option></select></div>");
  html += F("<div class=\"cycle-create-mid\">");
  html += F("<div><label>Durum</label><select name=\"enabled\" class=\"w-num\">");
  html += F("<option value=\"1\"");
  if (cfgCycleEnabled) html += F(" selected");
  html += F(">Aktif</option><option value=\"0\"");
  if (!cfgCycleEnabled) html += F(" selected");
  html += F(">Pasif</option></select></div>");
  html += F("<div><label>Periyot Dk</label><input type=\"number\" class=\"w-num\" name=\"period\" min=\"1\" step=\"1\" value=\"");
  html += String(cfgCyclePeriodMin);
  html += F("\" placeholder=\"Periyot Dk\"></div>");
  html += F("<div><label>Durum Dk</label><input type=\"number\" class=\"w-num\" name=\"dur\" min=\"0\" step=\"1\" value=\"");
  html += String(cfgCycleDurMin);
  html += F("\" placeholder=\"Durum Dk\"></div>");
  html += F("</div>");
  html += F("<div><label>&nbsp;</label><button type=\"submit\">Kaydet</button></div></div></form></div>");

  html += F("<div class=\"sum\"><h3>Kayitli Senaryolar</h3>");
  if (gWinRuleCount == 0) {
    html += F("<div class=\"r\">- yok</div>");
  } else {
    for (int i = 0; i < gWinRuleCount; i++) {
      html += F("<form method=\"POST\" action=\"/sched/rule\" class=\"rule-line\" id=\"rule-");
      html += String(i);
      html += F("\">");
      html += F("<input type=\"hidden\" name=\"type\" value=\"win\">");
      html += F("<input type=\"hidden\" name=\"idx\" value=\"");
      html += String(i);
      html += F("\">");
      html += F("<select name=\"relay\" class=\"w-relay\" disabled>");
      html += F("<option value=\"0\"");
      if (gWinRuleRelay[i] == 0) html += F(" selected");
      html += F(">SunLig</option><option value=\"1\"");
      if (gWinRuleRelay[i] == 1) html += F(" selected");
      html += F(">GrowLig</option><option value=\"2\"");
      if (gWinRuleRelay[i] == 2) html += F(" selected");
      html += F(">Su Mot.</option></select>");
      html += F("<select name=\"enabled\" disabled>");
      html += F("<option value=\"0\"");
      if (!gWinRuleEnabled[i]) html += F(" selected");
      html += F(">Pasif</option><option value=\"1\"");
      if (gWinRuleEnabled[i]) html += F(" selected");
      html += F(">Aktif</option></select>");
      html += F("<input type=\"text\" class=\"hhmm\" name=\"at\" maxlength=\"5\" placeholder=\"HH:MM\" value=\"");
      html += formatMinutesToTime(gWinRuleAtMin[i]);
      html += F("\" disabled>");
      html += F("<button type=\"button\" class=\"btn-save\" data-editing=\"0\" onclick=\"toggleRuleEdit(");
      html += String(i);
      html += F(")\">Guncelle</button>");
      html += F("<button type=\"submit\" name=\"delete\" value=\"1\" class=\"btn-del\">Sil</button>");
      html += F("</form>");
    }
  }
  html += F("<div class=\"r\" style=\"color:#94a3b8;margin-top:0.45rem;\">Araliga Gore:</div>");
  if (gCycleRuleCount == 0) {
    html += F("<div class=\"r\">- yok</div>");
  } else {
    for (int i = 0; i < gCycleRuleCount; i++) {
      html += F("<form method=\"POST\" action=\"/sched/rule\" class=\"rule-line-cycle\" id=\"rulec-");
      html += String(i);
      html += F("\">");
      html += F("<input type=\"hidden\" name=\"type\" value=\"cycle\"><input type=\"hidden\" name=\"idx\" value=\"");
      html += String(i);
      html += F("\">");
      html += F("<select name=\"relay\" disabled>");
      html += F("<option value=\"0\"");
      if (gCycleRuleRelay[i] == 0) html += F(" selected");
      html += F(">SunLig</option><option value=\"1\"");
      if (gCycleRuleRelay[i] == 1) html += F(" selected");
      html += F(">GrowLig</option><option value=\"2\"");
      if (gCycleRuleRelay[i] == 2) html += F(" selected");
      html += F(">Su Mot.</option></select>");
      html += F("<select name=\"enabled\" class=\"w-num\" disabled>");
      html += F("<option value=\"1\"");
      if (gCycleRuleEnabled[i]) html += F(" selected");
      html += F(">Aktif</option><option value=\"0\"");
      if (!gCycleRuleEnabled[i]) html += F(" selected");
      html += F(">Pasif</option></select>");
      html += F("<input type=\"number\" class=\"w-num\" name=\"period\" value=\"");
      html += String(gCycleRulePeriodMin[i]);
      html += F("\" disabled>");
      html += F("<input type=\"number\" class=\"w-num\" name=\"dur\" value=\"");
      html += String(gCycleRuleDurMin[i]);
      html += F("\" disabled>");
      html += F("<button type=\"button\" class=\"btn-save\" data-editing=\"0\" onclick=\"toggleRuleCycleEdit(");
      html += String(i);
      html += F(")\">Guncelle</button>");
      html += F("<button type=\"submit\" name=\"delete\" value=\"1\" class=\"btn-del\">Sil</button></form>");
    }
  }
  html += F("</div>");
  html += F("<script>function toggleRuleEdit(i){const f=document.getElementById('rule-'+i);if(!f)return;const b=f.querySelector('.btn-save');if(!b)return;const fields=f.querySelectorAll('select,input.hhmm');const editing=b.getAttribute('data-editing')==='1';if(!editing){fields.forEach(el=>el.disabled=false);b.setAttribute('data-editing','1');b.textContent='Kaydet';return;}f.submit();}function toggleRuleCycleEdit(i){const f=document.getElementById('rulec-'+i);if(!f)return;const b=f.querySelector('.btn-save');if(!b)return;const fields=f.querySelectorAll('select,input');const editing=b.getAttribute('data-editing')==='1';if(!editing){fields.forEach(el=>{if(el.name!=='type'&&el.name!=='idx'&&el.name!=='delete')el.disabled=false;});b.setAttribute('data-editing','1');b.textContent='Kaydet';return;}f.submit();}</script>");
  }

  if (webDebug) {
    html += F("<p class=\"hint\"><a href=\"/settings\" style=\"color:#60a5fa;\">Ayarlar/Kalibrasyon</a> | <a href=\"/ota\" style=\"color:#60a5fa;\">Firmware OTA</a></p>");
  }
  html += F("</body></html>");
  server.send(200, "text/html", html);
}

void handleSettingsPage() {
  String html;
  html.reserve(5600);
  html += F("<!DOCTYPE html><html><head><meta charset=\"utf-8\">");
  html += F("<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">");
  html += F("<title>Ayarlar</title>");
  html += F("<style>body{font-family:system-ui,sans-serif;margin:1rem;background:#0f172a;color:#e2e8f0;max-width:32rem;}");
  html += F("label{display:block;margin-top:0.7rem;color:#94a3b8;}input{width:100%;padding:0.5rem;border-radius:0.45rem;border:1px solid #334155;background:#111827;color:#e2e8f0;}");
  html += F(".grid{display:grid;grid-template-columns:1fr 1fr;gap:0.6rem;}button{margin-top:1rem;padding:0.6rem 1rem;border:none;border-radius:0.5rem;background:#3b82f6;color:#fff;font-weight:600;}");
  html += F(".grid3{display:grid;grid-template-columns:1fr 1fr 1fr;gap:0.5rem;}");
  html += F(".card{background:#111827;border:1px solid #1f2937;border-radius:0.55rem;padding:0.65rem;margin:0.7rem 0;}");
  html += F(".card h3{margin:0 0 0.45rem 0;font-size:0.92rem;color:#cbd5e1;}");
  html += F(".d{font-size:0.85rem;line-height:1.45;color:#cbd5e1;}");
  html += F(".btn-danger{background:#b91c1c;} .row{display:flex;gap:0.6rem;flex-wrap:wrap;}");
  html += F("a{color:#60a5fa;} .hint{font-size:0.85rem;color:#94a3b8;line-height:1.35;}</style></head><body>");
  html += F("<h1 style=\"font-size:1.1rem;\">Kalici Ayarlar (NVS)</h1>");
  html += F("<div class=\"card\"><h3>Canli Debug Ozeti</h3>");
  html += F("<div class=\"d\">RAW: <span id=\"dbg-raw\">0</span> | PPM: <span id=\"dbg-ppm\">0</span> | EC: <span id=\"dbg-ec\">0</span> uS</div>");
  html += F("<div class=\"d\">pH: <span id=\"dbg-ph\">0</span> | pV: <span id=\"dbg-pv\">0</span> | pA: <span id=\"dbg-pa\">0</span></div>");
  html += F("<script>function updDbg(){fetch('/dbg').then(r=>r.json()).then(j=>{document.getElementById('dbg-raw').textContent=j.raw;document.getElementById('dbg-ppm').textContent=j.ppm;document.getElementById('dbg-ec').textContent=j.ec;document.getElementById('dbg-ph').textContent=j.ph.toFixed(2);document.getElementById('dbg-pv').textContent=j.pv.toFixed(3);document.getElementById('dbg-pa').textContent=j.pa;}).catch(()=>{});}setInterval(updDbg,1000);updDbg();</script>");
  html += F("</div>");
  html += F("<form method=\"POST\" action=\"/settings/save\">");
  html += F("<div class=\"grid\">");
  html += F("<div><label>pH4 voltaj</label><input name=\"ph4\" type=\"number\" step=\"0.0001\" value=\"");
  html += String(cfgPhCalV4, 4);
  html += F("\"></div>");
  html += F("<div><label>pH7 voltaj</label><input name=\"ph7\" type=\"number\" step=\"0.0001\" value=\"");
  html += String(cfgPhCalV7, 4);
  html += F("\"></div>");
  html += F("<div><label>pH10 voltaj</label><input name=\"ph10\" type=\"number\" step=\"0.0001\" value=\"");
  html += String(cfgPhCalV10, 4);
  html += F("\"></div></div>");
  html += F("<p class=\"hint\">pH kalibrasyon girisi: probu pH4/pH7/pH10 sivisina daldir, debug ekranda gorunen pV (voltaj) degerini ilgili kutuya yaz.</p>");
  html += F("<h2 style=\"font-size:1rem;margin-top:1rem;\">Saha duzeltmesi (besin/kabin suyu)</h2>");
  html += F("<p class=\"hint\">Buffer sonrasi besin/kabin suyunda sapma icin: pH = pH*olcek+ofset; EC(uS)=EC*olcek+ofset. Ornek: olcek=1, ofset= el_cihaz-ESP_pH.</p>");
  html += F("<div class=\"grid\">");
  html += F("<div><label>pH olcek</label><input name=\"ph_scale\" type=\"number\" step=\"0.0001\" value=\"");
  html += String(cfgPhScale, 4);
  html += F("\"></div>");
  html += F("<div><label>pH ofset</label><input name=\"ph_off\" type=\"number\" step=\"0.01\" value=\"");
  html += String(cfgPhOffset, 2);
  html += F("\"></div>");
  html += F("<div><label>EC olcek</label><input name=\"ec_scale\" type=\"number\" step=\"0.0001\" value=\"");
  html += String(cfgEcScale, 4);
  html += F("\"></div>");
  html += F("<div><label>EC ofset (uS)</label><input name=\"ec_off\" type=\"number\" step=\"1\" value=\"");
  html += String(cfgEcOffset, 0);
  html += F("\"></div></div>");
  html += F("<h2 style=\"font-size:1rem;margin-top:1rem;\">Kalibrasyon noktalar (RAW + cihaz PPM + cihaz EC)</h2>");
  html += F("<p class=\"hint\">Her noktada ayni anda 3 deger gir: ESP RAW, el cihazinin PPM'i ve el cihazinin EC(uS) degeri.</p>");
  for (int i = 0; i < CAL_POINTS; i++) {
    html += F("<div class=\"grid3\"><div><label>RAW ");
    html += String(i + 1);
    html += F("</label><input name=\"r");
    html += String(i);
    html += F("\" type=\"number\" step=\"0.01\" value=\"");
    html += String(cfgRawPoints[i], 2);
    html += F("\"></div><div><label>PPM ");
    html += String(i + 1);
    html += F("</label><input name=\"p");
    html += String(i);
    html += F("\" type=\"number\" step=\"0.01\" value=\"");
    html += String(cfgPpmPoints[i], 2);
    html += F("\"></div><div><label>Cihaz EC ");
    html += String(i + 1);
    html += F(" (uS)</label><input name=\"e");
    html += String(i);
    html += F("\" type=\"number\" step=\"0.01\" value=\"");
    html += String(cfgEcPoints[i], 2);
    html += F("\"></div></div>");
  }
  html += F("<div class=\"row\"><button type=\"submit\">Kaydet</button></form>");
  html += F("<form method=\"POST\" action=\"/settings/reset\" onsubmit=\"return confirm('Fabrika ayarlarina donulsun mu?');\">");
  html += F("<button class=\"btn-danger\" type=\"submit\">Fabrika ayarlarina don</button></form></div>");
  html += F("<p class=\"hint\">Kaydedilen ayarlar yeniden baslatmadan aktif olur ve enerji kesilse de kalir.</p>");
  html += F("<p><a href=\"/\">Ana sayfa</a></p>");
  html += F("</body></html>");
  server.send(200, "text/html", html);
}

void handleSettingsSave() {
  if (server.hasArg("ph4")) cfgPhCalV4 = server.arg("ph4").toFloat();
  if (server.hasArg("ph7")) cfgPhCalV7 = server.arg("ph7").toFloat();
  if (server.hasArg("ph10")) cfgPhCalV10 = server.arg("ph10").toFloat();

  for (int i = 0; i < CAL_POINTS; i++) {
    String rk = "r" + String(i);
    String pk = "p" + String(i);
    String ek = "e" + String(i);
    if (server.hasArg(rk)) cfgRawPoints[i] = server.arg(rk).toFloat();
    if (server.hasArg(pk)) cfgPpmPoints[i] = server.arg(pk).toFloat();
    if (server.hasArg(ek)) cfgEcPoints[i] = server.arg(ek).toFloat();
  }

  if (server.hasArg("ph_scale")) cfgPhScale = server.arg("ph_scale").toFloat();
  if (server.hasArg("ph_off")) cfgPhOffset = server.arg("ph_off").toFloat();
  if (server.hasArg("ec_scale")) cfgEcScale = server.arg("ec_scale").toFloat();
  if (server.hasArg("ec_off")) cfgEcOffset = server.arg("ec_off").toFloat();

  saveSettings();
  forceRefresh = true;
  server.sendHeader("Location", "/settings");
  server.send(303, "text/plain", "");
}

void handleSettingsReset() {
  applyDefaultSettings();
  saveSettings();
  forceRefresh = true;
  server.sendHeader("Location", "/settings");
  server.send(303, "text/plain", "");
}

void handleDebugJson() {
  // Settings sayfasindaki canli debug karti icin JSON endpoint
  String json;
  json.reserve(220);
  json += F("{");
  json += F("\"raw\":");
  json += String((int)g_tdsRaw);
  json += F(",\"ppm\":");
  json += String((int)g_tdsPpm);
  json += F(",\"ec\":");
  json += String((int)g_ecUsCm);
  json += F(",\"ph\":");
  json += String(g_ph, 2);
  json += F(",\"pv\":");
  json += String(g_phV, 3);
  json += F(",\"pa\":");
  json += String((int)g_phAdc);
  json += F(",\"heap\":");
  json += String((int)(ESP.getFreeHeap() / 1024));
  json += F("}");
  server.send(200, "application/json", json);
}

void handleScheduleSave() {
  String type = server.hasArg("type") ? server.arg("type") : "";
  type.toLowerCase();

  if (type == "win") {
    if (server.hasArg("relay")) cfgWinRelay = constrain(server.arg("relay").toInt(), 0, 2);
    if (server.hasArg("enabled")) cfgWinEnabled = constrain(server.arg("enabled").toInt(), 0, 1);
    if (server.hasArg("start")) cfgWinAtMin = parseTimeArgToMinutes(server.arg("start"), cfgWinAtMin);
    if (server.hasArg("clear") && server.arg("clear") == "1") {
      gWinRuleCount = 0;
    } else if (gWinRuleCount < MAX_SCHED_RULES) {
      gWinRuleRelay[gWinRuleCount] = cfgWinRelay;
      gWinRuleEnabled[gWinRuleCount] = cfgWinEnabled;
      gWinRuleAtMin[gWinRuleCount] = cfgWinAtMin;
      gWinRuleCount++;
    }
    g_manualOverrideUntilMs[cfgWinRelay] = 0;  // Yeni kural kaydinda scheduler hemen devreye girsin
  } else if (type == "cycle") {
    if (server.hasArg("relay")) cfgCycleRelay = constrain(server.arg("relay").toInt(), 0, 2);
    if (server.hasArg("enabled")) cfgCycleEnabled = constrain(server.arg("enabled").toInt(), 0, 1);
    if (server.hasArg("period")) {
      long v = server.arg("period").toInt();
      cfgCyclePeriodMin = (v < 1) ? 1 : (int)v;
    }
    if (server.hasArg("dur")) {
      long v = server.arg("dur").toInt();
      cfgCycleDurMin = (v < 0) ? 0 : (int)v;
    }
    if (gCycleRuleCount < MAX_SCHED_RULES) {
      gCycleRuleRelay[gCycleRuleCount] = cfgCycleRelay;
      gCycleRuleEnabled[gCycleRuleCount] = cfgCycleEnabled;
      gCycleRulePeriodMin[gCycleRuleCount] = cfgCyclePeriodMin;
      gCycleRuleDurMin[gCycleRuleCount] = cfgCycleDurMin;
      gCycleRuleCount++;
    }
    g_manualOverrideUntilMs[cfgCycleRelay] = 0;
  }

  saveSettings();
  applyAuxSchedulerIfNeeded();  // Aktif kural varsa kayit sonrasi hemen uygula
  forceRefresh = true;
  server.sendHeader("Location", "/");
  server.send(303, "text/plain", "");
}

void handleScheduleRuleAction() {
  String type = server.hasArg("type") ? server.arg("type") : "";
  type.toLowerCase();
  int idx = server.hasArg("idx") ? server.arg("idx").toInt() : -1;
  if (type == "win" && idx >= 0 && idx < gWinRuleCount) {
    if (server.hasArg("delete") && server.arg("delete") == "1") {
      for (int i = idx + 1; i < gWinRuleCount; i++) {
        gWinRuleRelay[i - 1] = gWinRuleRelay[i];
        gWinRuleEnabled[i - 1] = gWinRuleEnabled[i];
        gWinRuleAtMin[i - 1] = gWinRuleAtMin[i];
      }
      gWinRuleCount--;
    } else {
      if (server.hasArg("relay")) gWinRuleRelay[idx] = constrain(server.arg("relay").toInt(), 0, 2);
      if (server.hasArg("enabled")) {
        gWinRuleEnabled[idx] = constrain(server.arg("enabled").toInt(), 0, 1);
      }
      if (server.hasArg("at")) {
        gWinRuleAtMin[idx] = parseTimeArgToMinutes(server.arg("at"), gWinRuleAtMin[idx]);
      }
      g_manualOverrideUntilMs[constrain(gWinRuleRelay[idx], 0, 2)] = 0;
    }
  } else if (type == "cycle" && idx >= 0 && idx < gCycleRuleCount) {
    if (server.hasArg("delete") && server.arg("delete") == "1") {
      for (int i = idx + 1; i < gCycleRuleCount; i++) {
        gCycleRuleRelay[i - 1] = gCycleRuleRelay[i];
        gCycleRuleEnabled[i - 1] = gCycleRuleEnabled[i];
        gCycleRuleStartMin[i - 1] = gCycleRuleStartMin[i];
        gCycleRuleEndMin[i - 1] = gCycleRuleEndMin[i];
        gCycleRulePeriodMin[i - 1] = gCycleRulePeriodMin[i];
        gCycleRuleDurMin[i - 1] = gCycleRuleDurMin[i];
      }
      gCycleRuleCount--;
    } else {
      if (server.hasArg("relay")) gCycleRuleRelay[idx] = constrain(server.arg("relay").toInt(), 0, 2);
      if (server.hasArg("enabled")) gCycleRuleEnabled[idx] = constrain(server.arg("enabled").toInt(), 0, 1);
      if (server.hasArg("period")) {
        long v = server.arg("period").toInt();
        gCycleRulePeriodMin[idx] = (v < 1) ? 1 : (int)v;
      }
      if (server.hasArg("dur")) {
        long v = server.arg("dur").toInt();
        gCycleRuleDurMin[idx] = (v < 0) ? 0 : (int)v;
      }
      g_manualOverrideUntilMs[constrain(gCycleRuleRelay[idx], 0, 2)] = 0;
    }
  }
  saveSettings();
  applyAuxSchedulerIfNeeded();
  forceRefresh = true;
  server.sendHeader("Location", "/");
  server.send(303, "text/plain", "");
}

void handleLed() {
  String t = server.hasArg("t") ? server.arg("t") : "";
  String s = server.hasArg("s") ? server.arg("s") : "";
  String all = server.hasArg("all") ? server.arg("all") : "";
  t.toLowerCase();
  s.toLowerCase();
  all.toLowerCase();

  if (all == "on") {
    sequentialStartLeds();
    g_manualOverrideUntilMs[0] = millis() + MANUAL_OVERRIDE_MS;
    g_manualOverrideUntilMs[1] = millis() + MANUAL_OVERRIDE_MS;
    g_manualOverrideUntilMs[2] = millis() + MANUAL_OVERRIDE_MS;
  } else if (all == "off") {
    allLedsOff();
    g_manualOverrideUntilMs[0] = millis() + MANUAL_OVERRIDE_MS;
    g_manualOverrideUntilMs[1] = millis() + MANUAL_OVERRIDE_MS;
    g_manualOverrideUntilMs[2] = millis() + MANUAL_OVERRIDE_MS;
  } else if (t == "strip") {
    bool target = (s == "toggle") ? !g_relayStrip : (s == "on");
    setRelayStrip(target);
    g_manualOverrideUntilMs[0] = millis() + MANUAL_OVERRIDE_MS;
  } else if (t == "drop") {
    bool target = (s == "toggle") ? !g_relayDrop : (s == "on");
    setRelayDrop(target);
    g_manualOverrideUntilMs[1] = millis() + MANUAL_OVERRIDE_MS;
  } else if (t == "aux") {
    bool target = (s == "toggle") ? !g_relayAux : (s == "on");
    applyAuxIsolationRule(target);
    g_manualOverrideUntilMs[2] = millis() + MANUAL_OVERRIDE_MS;
  }

  forceRefresh = true;
  server.sendHeader("Location", "/");
  server.send(303, "text/plain", "");
}

void handleOtaPage() {
  String html;
  html.reserve(1200);
  html += F("<!DOCTYPE html><html><head><meta charset=\"utf-8\">");
  html += F("<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">");
  html += F("<title>OTA</title>");
  html += F("<style>body{font-family:system-ui,sans-serif;margin:1rem;background:#0f172a;color:#e2e8f0;max-width:24rem;}");
  html += F("a{color:#60a5fa;} input[type=file]{margin:0.75rem 0;width:100%;}");
  html += F("button{padding:0.55rem 1rem;border-radius:0.5rem;border:none;background:#3b82f6;color:#fff;font-weight:600;}");
  html += F(".warn{color:#fbbf24;font-size:0.85rem;margin-top:1rem;line-height:1.4;}</style></head><body>");
  html += F("<h1 style=\"font-size:1.1rem;\">Firmware guncelle</h1>");
  html += F("<p style=\"color:#94a3b8;font-size:0.9rem;\">Arduino IDE: Sketch -> Export derlenmis ikili dosya (.bin). Asagidan secip yukleyin.</p>");
  html += F("<form method=\"POST\" action=\"/update\" enctype=\"multipart/form-data\">");
  html += F("<input type=\"file\" name=\"firmware\" accept=\".bin\" required>");
  html += F("<br><button type=\"submit\">Yukle ve yeniden baslat</button></form>");
  html += F("<p><a href=\"/\">Ana sayfa</a></p>");
  html += F("<p class=\"warn\">Uyari: Yukleme sirasinda guc kesmeyin. Sadece guvenilir agda kullanin. Sifreyi degistirin: OTA_PASSWORD</p>");
  html += F("</body></html>");
  server.send(200, "text/html", html);
}

void handleFirmwareUpload() {
  HTTPUpload &upload = server.upload();
  if (upload.status == UPLOAD_FILE_START) {
    Serial.printf("OTA dosya: %s\n", upload.filename.c_str());
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
      Update.printError(Serial);
    }
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
      Update.printError(Serial);
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (Update.end(true)) {
      Serial.printf("OTA bitti: %u bayt\n", upload.totalSize);
    } else {
      Update.printError(Serial);
    }
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    Update.abort();
    Serial.println(F("OTA iptal"));
  }
}

void handleFirmwareUploadDone() {
  server.sendHeader("Connection", "close");
  if (Update.hasError()) {
    server.send(500, "text/plain", "Guncelleme basarisiz");
    Update.printError(Serial);
    Update.abort();
  } else {
    server.send(200, "text/plain", "Tamam. Cihaz yeniden basliyor...");
    delay(300);
    ESP.restart();
  }
}

static const unsigned long WIFI_RETRY_INTERVAL_MS = 8000;
static const unsigned long WIFI_AUTO_RECOVERY_MS = 90000;  // 90 sn wifi yoksa reboot
unsigned long wifiDownSinceMs = 0;
bool g_hadWifiConnection = false;

static void registerHttpRoutesIfNeeded() {
  if (s_httpRoutesRegistered) return;
  s_httpRoutesRegistered = true;
  server.on("/", HTTP_GET, handleRoot);
  server.on("/settings", HTTP_GET, handleSettingsPage);
  server.on("/settings/save", HTTP_POST, handleSettingsSave);
  server.on("/settings/reset", HTTP_POST, handleSettingsReset);
  server.on("/dbg", HTTP_GET, handleDebugJson);
  server.on("/sched/save", HTTP_POST, handleScheduleSave);
  server.on("/sched/rule", HTTP_POST, handleScheduleRuleAction);
  server.on("/ota", HTTP_GET, handleOtaPage);
  server.on("/update", HTTP_POST, handleFirmwareUploadDone, handleFirmwareUpload);
  server.on("/led", HTTP_GET, handleLed);
  server.on("/led", HTTP_POST, handleLed);
}

static void startNetworkStack() {
  if (s_networkStackStarted) return;
  s_networkStackStarted = true;

  registerHttpRoutesIfNeeded();
  server.begin();
  g_webStarted = true;

  if (MDNS.begin(OTA_HOSTNAME)) {
    MDNS.addService("http", "tcp", 80);
    Serial.print(F("mDNS: http://"));
    Serial.print(OTA_HOSTNAME);
    Serial.println(F(".local"));
  }

  ArduinoOTA.setHostname(OTA_HOSTNAME);
  if (strlen(OTA_PASSWORD) > 0) {
    ArduinoOTA.setPassword(OTA_PASSWORD);
  }
  ArduinoOTA
      .onStart([]() {
        Serial.println(F("[OTA] Basladi"));
      })
      .onEnd([]() {
        Serial.println(F("[OTA] Bitti"));
      })
      .onError([](ota_error_t err) {
        Serial.printf("[OTA] Hata: %u\n", err);
      });
  ArduinoOTA.begin();
  g_otaStarted = true;
  Serial.print(F("ArduinoOTA sifre: "));
  Serial.println(strlen(OTA_PASSWORD) > 0 ? F("(ayarli)") : F("(yok)"));

  configTime(NTP_GMT_OFFSET_SEC, 0, "pool.ntp.org", "time.google.com");
  Serial.print(F("NTP (UTC+"));
  Serial.print(NTP_GMT_OFFSET_SEC / 3600);
  Serial.println(F("h) baslatildi"));

  Serial.print(F("Web: http://"));
  Serial.println(WiFi.localIP());
}

static void maintainWifi() {
  if (strlen(WIFI_SSID) == 0) {
    g_wifiOk = false;
    return;
  }

  g_wifiOk = (WiFi.status() == WL_CONNECTED);

  if (g_wifiOk) {
    g_hadWifiConnection = true;
    wifiDownSinceMs = 0;
    if (!s_networkStackStarted) {
      Serial.println(F("WiFi baglandi, ag servisleri basliyor"));
      startNetworkStack();
    }
    return;
  }

  if (wifiDownSinceMs == 0) {
    wifiDownSinceMs = millis();
  } else if (g_hadWifiConnection && (millis() - wifiDownSinceMs >= WIFI_AUTO_RECOVERY_MS)) {
    Serial.println(F("WiFi uzun sure yok, otomatik yeniden baslatma..."));
    delay(150);
    ESP.restart();
  }

  if (millis() - lastWifiRetryMs >= WIFI_RETRY_INTERVAL_MS) {
    lastWifiRetryMs = millis();
    Serial.println(F("WiFi yok, yeniden deneniyor..."));
    WiFi.disconnect(false);
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  }
}

void setup() {
  Serial.begin(115200);
  bootMs = millis();
  lastRelayPersistMs = millis();
  g_bootRelayRestoreDone = false;
  loadSettings();

  // ESP32 default I2C pins set explicitly for clarity
  Wire.begin(21, 22);  // SDA, SCL
  analogReadResolution(12);
  analogSetPinAttenuation(TDS_PIN, ADC_11db);
  analogSetPinAttenuation(PH_PIN, ADC_11db);
  pinMode(TDS_PIN, INPUT);
  pinMode(PH_PIN, INPUT);
  pinMode(MODE_BTN_PIN, INPUT_PULLUP);

  // Roleler: once OFF seviyesini yaz, sonra OUTPUT'a al. Boot sirasinda
  // pinin kisa floating/LOW anini bu sekilde maskeliyoruz (active-LOW rolelerde
  // aksi halde aciliyor anlik).
  digitalWrite(RELAY_STRIP_PIN, relayLevel(false));
  digitalWrite(RELAY_DROP_PIN, relayLevel(false));
  digitalWrite(RELAY_AUX_PIN, relayLevel(false));
  pinMode(RELAY_STRIP_PIN, OUTPUT);
  pinMode(RELAY_DROP_PIN, OUTPUT);
  pinMode(RELAY_AUX_PIN, OUTPUT);
  digitalWrite(RELAY_STRIP_PIN, relayLevel(false));
  digitalWrite(RELAY_DROP_PIN, relayLevel(false));
  digitalWrite(RELAY_AUX_PIN, relayLevel(false));
  g_relayStrip = false;
  g_relayDrop = false;
  g_relayAux = false;

  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDRESS)) {
    Serial.println("OLED baslatilamadi");
    while (true) {
      delay(1000);
    }
  }

  display.clearDisplay();
  display.setRotation(2);  // Rotate screen 180 degrees
  display.setTextSize(2);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 20);
  display.println("Sistem");
  display.println("calisiyor");
  display.display();

  Serial.println("OLED mesaj yazdirildi");

  Serial.println(F("LED'ler boot'ta kapali baslatildi (brownout korumasi)."));

  if (strlen(WIFI_SSID) == 0) {
    g_wifiOk = false;
    g_webStarted = false;
    Serial.println(F("WiFi: wifi_secrets.h yok veya WIFI_SSID bos. Web kapali."));
  } else {
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    lastWifiRetryMs = millis();
    Serial.println(F("WiFi baslatildi; baglanti arka planda denenir (OLED/sensor calisir)."));
  }
}

void loop() {
  maintainWifi();

  if (g_wifiOk && time(nullptr) <= 1700000000 && millis() - lastNtpRetryMs > 25000) {
    lastNtpRetryMs = millis();
    configTime(NTP_GMT_OFFSET_SEC, 0, "pool.ntp.org", "time.google.com");
    Serial.println(F("NTP tekrar deneniyor"));
  }

  if (g_otaStarted && g_wifiOk) {
    ArduinoOTA.handle();
  }
  if (g_webStarted && g_wifiOk) {
    server.handleClient();
  }

  if (!g_bootRelayRestoreDone && millis() - bootMs > 3000) {
    restoreLastRelayStatesSequentially();
  }

  applyAuxSchedulerIfNeeded();
  persistRelayStatesIfNeeded();

  bool buttonState = digitalRead(MODE_BTN_PIN);
  if (buttonState == LOW && lastButtonState == HIGH && millis() - lastButtonMs > 250) {
    debugMode = !debugMode;
    forceRefresh = true;
    lastUpdateMs = 0;
    lastButtonMs = millis();
  }
  lastButtonState = buttonState;
  g_debugModeSnapshot = debugMode;

  // Always read sensors every second (independent from display mode/platform).
  if (!haveSample || (millis() - lastSampleMs >= SAMPLE_INTERVAL_MS)) {
    lastSampleMs = millis();
    haveSample = true;
    float avgAdc = 0.0;
    float voltage = 0.0;
    float tdsRawPpm = readTdsPpm(avgAdc, voltage);
    float tdsCalPpm = rawToPpmCalibrated(tdsRawPpm);
    float ecBuf = rawToEcCalibrated(tdsRawPpm);
    float ecUsCm = ecBuf * cfgEcScale + cfgEcOffset;
    if (ecUsCm < 0.0f) ecUsCm = 0.0f;

    float phAdc = 0.0;
    float phVoltage = 0.0;
    float phBuf = readPhValue(phAdc, phVoltage);
    float phValue = phBuf * cfgPhScale + cfgPhOffset;
    if (phValue < 0.0f) phValue = 0.0f;
    if (phValue > 14.0f) phValue = 14.0f;

    g_ph = phValue;
    g_ecUsCm = ecUsCm;
    g_tdsPpm = tdsCalPpm;
    g_tdsRaw = tdsRawPpm;
    g_tdsAdc = avgAdc;
    g_tdsV = voltage;
    g_phAdc = phAdc;
    g_phV = phVoltage;

    time_t now = time(nullptr);
    if (now > 1700000000) {
      struct tm ti;
      localtime_r(&now, &ti);
      snprintf(g_clockStr, sizeof(g_clockStr), "%02d:%02d", ti.tm_hour, ti.tm_min);
    } else {
      snprintf(g_clockStr, sizeof(g_clockStr), "%s", "--:--");
    }

    Serial.print("ADC:");
    Serial.print(avgAdc, 1);
    Serial.print("  V:");
    Serial.print(voltage, 3);
    Serial.print("  TDS_raw:");
    Serial.print(tdsRawPpm, 1);
    Serial.print("  PPM_cal:");
    Serial.print(tdsCalPpm, 1);
    Serial.print("  EC_kal:");
    Serial.print(ecBuf, 1);
    Serial.print("  EC_saha:");
    Serial.print(ecUsCm, 1);
    Serial.print("  PH_ADC:");
    Serial.print(phAdc, 1);
    Serial.print("  PH_V:");
    Serial.print(phVoltage, 3);
    Serial.print("  pH_kal:");
    Serial.print(phBuf, 2);
    Serial.print("  pH_saha:");
    Serial.println(phValue, 2);
  }

  // OLED refresh period is mode-dependent.
  unsigned long displayIntervalMs = debugMode ? DEBUG_INTERVAL_MS : SAMPLE_INTERVAL_MS;
  if (haveSample && (forceRefresh || (millis() - lastUpdateMs >= displayIntervalMs))) {
    lastUpdateMs = millis();
    forceRefresh = false;

    display.clearDisplay();
    display.setRotation(2);  // Keep 180-degree orientation
    display.setTextColor(SSD1306_WHITE);

    drawOledStatusBar();

    if (!debugMode) {
      display.setTextSize(1);
      display.setCursor(0, OLED_STATUS_H);
      display.print("pH:");
      display.println(g_ph, 2);
      display.setCursor(0, OLED_STATUS_H + 24);
      display.print("EC:");
      display.println((int)g_ecUsCm);
      display.setCursor(0, SCREEN_HEIGHT - 8);
      display.print("LED S:");
      display.print(g_relayStrip ? "ON " : "OFF");
      display.print(" D:");
      display.print(g_relayDrop ? "ON " : "OFF");
      display.print(" R3:");
      display.print(g_relayAux ? "ON" : "OFF");
    } else {
      display.setTextSize(1);
      display.setCursor(0, OLED_STATUS_H);
      display.print("RAW:");
      display.println((int)g_tdsRaw);
      display.print("TDS:");
      display.print((int)g_tdsPpm);
      display.println(" ppm");
      display.print("EC:");
      display.print((int)g_ecUsCm);
      display.println(" uS");
      display.print("pH:");
      display.print(g_ph, 2);
      display.print(" pA:");
      display.println((int)g_phAdc);
      display.print("pV:");
      display.println(g_phV, 3);
    }
    display.display();
  }
}
