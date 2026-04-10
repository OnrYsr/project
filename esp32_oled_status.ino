#include <Wire.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <string.h>

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

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
WebServer server(80);

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

const float VREF = 3.3;          // ESP32 ADC reference voltage
const float ADC_MAX = 4095.0;    // 12-bit ADC
const float WATER_TEMP_C = 25.0; // Temperature compensation reference
const float EC_US_PER_PPM = 2.0; // Your meter data: EC(uS/cm) ~= 2 * PPM
const float PH_CAL_V4 = 3.300;   // Measured voltage in pH 4 buffer
const float PH_CAL_V7 = 2.528;   // Measured voltage in pH 7 buffer
const float PH_CAL_V10 = 2.012;  // Measured voltage in pH 10 buffer

unsigned long lastUpdateMs = 0;
unsigned long lastSampleMs = 0;
unsigned long lastButtonMs = 0;
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
const float RAW_POINTS[CAL_POINTS] = {45, 183, 236, 469, 911, 1019};
const float PPM_POINTS[CAL_POINTS] = {83, 218, 268, 487, 816, 992};

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
  if (rawValue <= RAW_POINTS[0]) {
    // Extrapolate below first point instead of clamping, so low readings can change
    float x1 = RAW_POINTS[0];
    float x2 = RAW_POINTS[1];
    float y1 = PPM_POINTS[0];
    float y2 = PPM_POINTS[1];
    float t = (rawValue - x1) / (x2 - x1);
    float ppm = y1 + t * (y2 - y1);
    return ppm < 0 ? 0 : ppm;
  }

  for (int i = 0; i < CAL_POINTS - 1; i++) {
    float x1 = RAW_POINTS[i];
    float x2 = RAW_POINTS[i + 1];
    float y1 = PPM_POINTS[i];
    float y2 = PPM_POINTS[i + 1];

    if (rawValue >= x1 && rawValue <= x2) {
      float t = (rawValue - x1) / (x2 - x1);
      return y1 + t * (y2 - y1);
    }
  }

  // Slight extrapolation above last point
  float x1 = RAW_POINTS[CAL_POINTS - 2];
  float x2 = RAW_POINTS[CAL_POINTS - 1];
  float y1 = PPM_POINTS[CAL_POINTS - 2];
  float y2 = PPM_POINTS[CAL_POINTS - 1];
  float t = (rawValue - x1) / (x2 - x1);
  float ppm = y1 + t * (y2 - y1);
  return ppm < 0 ? 0 : ppm;
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
  if (voltage >= PH_CAL_V7) {
    float m1 = (7.0 - 4.0) / (PH_CAL_V7 - PH_CAL_V4);
    float b1 = 7.0 - m1 * PH_CAL_V7;
    ph = m1 * voltage + b1;
  } else {
    float m2 = (10.0 - 7.0) / (PH_CAL_V10 - PH_CAL_V7);
    float b2 = 7.0 - m2 * PH_CAL_V7;
    ph = m2 * voltage + b2;
  }

  phAdcOut = avgAdc;
  phVoltageOut = voltage;

  if (ph < 0) return 0;
  if (ph > 14) return 14;
  return ph;
}

void handleRoot() {
  // Web gorunumu OLED'den bagimsiz: ?view=normal | ?view=debug
  String view = server.hasArg("view") ? server.arg("view") : "normal";
  view.toLowerCase();
  bool webDebug = (view == "debug");
  int refreshSec = webDebug ? 1 : 3;
  String viewNext = webDebug ? "debug" : "normal";

  String html;
  html.reserve(2200);
  html += F("<!DOCTYPE html><html><head><meta charset=\"utf-8\">");
  html += F("<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">");
  html += F("<meta http-equiv=\"refresh\" content=\"");
  html += String(refreshSec);
  html += F(";url=/?view=");
  html += viewNext;
  html += F("\">");
  html += F("<title>Hydro Monitor</title>");
  html += F("<style>");
  html += F("body{font-family:system-ui,sans-serif;margin:1rem;background:#0f172a;color:#e2e8f0;max-width:22rem;}");
  html += F(".nav{display:flex;gap:0.5rem;margin-bottom:1rem;}");
  html += F(".nav a{flex:1;text-align:center;padding:0.55rem 0.4rem;border-radius:0.5rem;text-decoration:none;font-weight:600;font-size:0.9rem;}");
  html += F(".nav a.on{background:#3b82f6;color:#fff;}");
  html += F(".nav a.off{background:#334155;color:#cbd5e1;}");
  html += F(".ok{color:#4ade80;font-size:0.8rem;margin-bottom:0.75rem;} .bad{color:#f87171;font-size:0.8rem;margin-bottom:0.75rem;}");
  html += F(".n-ph{font-size:1.85rem;font-weight:700;margin:0.35rem 0 0.15rem;} .n-ec{font-size:1.85rem;font-weight:700;margin:0.35rem 0;}");
  html += F(".n-lab{color:#94a3b8;font-size:0.95rem;}");
  html += F(".d-row{margin:0.35rem 0;font-size:1rem;line-height:1.35;} .d-lab{color:#94a3b8;display:inline-block;min-width:4.2rem;}");
  html += F(".hint{color:#64748b;font-size:0.78rem;margin-top:1rem;line-height:1.35;}");
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

  if (!webDebug) {
    html += F("<div class=\"n-lab\">pH</div>");
    html += F("<div class=\"n-ph\">"); html += String(g_ph, 2); html += F("</div>");
    html += F("<div class=\"n-lab\" style=\"margin-top:0.6rem;\">EC</div>");
    html += F("<div class=\"n-ec\">"); html += String((int)g_ecUsCm); html += F(" uS/cm</div>");
    html += F("<p class=\"hint\">Web: sadece bu sayfa (OLED'i degistirmez). Yenileme 3 sn.</p>");
    html += F("<p class=\"hint\">OLED cihaz: ");
    html += g_debugModeSnapshot ? F("DEBUG") : F("NORMAL");
    html += F(" (buton)</p>");
  } else {
    html += F("<div class=\"d-row\"><span class=\"d-lab\">RAW</span>"); html += String((int)g_tdsRaw); html += F("</div>");
    html += F("<div class=\"d-row\"><span class=\"d-lab\">TDS</span>"); html += String((int)g_tdsPpm); html += F(" ppm</div>");
    html += F("<div class=\"d-row\"><span class=\"d-lab\">EC</span>"); html += String((int)g_ecUsCm); html += F(" uS</div>");
    html += F("<div class=\"d-row\"><span class=\"d-lab\">pH</span>"); html += String(g_ph, 2);
    html += F(" <span style=\"color:#64748b;\">pA:</span> "); html += String((int)g_phAdc); html += F("</div>");
    html += F("<div class=\"d-row\"><span class=\"d-lab\">pV</span>"); html += String(g_phV, 3); html += F("</div>");
    html += F("<div class=\"d-row\" style=\"margin-top:0.5rem;color:#64748b;font-size:0.85rem;\">OLED cihaz: ");
    html += g_debugModeSnapshot ? F("DEBUG") : F("NORMAL");
    html += F(" (buton)</div>");
    html += F("<p class=\"hint\">Web: sadece bu sayfa. Yenileme 1 sn.</p>");
  }

  html += F("</body></html>");
  server.send(200, "text/html", html);
}

void setup() {
  Serial.begin(115200);

  // ESP32 default I2C pins set explicitly for clarity
  Wire.begin(21, 22);  // SDA, SCL
  analogReadResolution(12);
  analogSetPinAttenuation(TDS_PIN, ADC_11db);
  analogSetPinAttenuation(PH_PIN, ADC_11db);
  pinMode(TDS_PIN, INPUT);
  pinMode(PH_PIN, INPUT);
  pinMode(MODE_BTN_PIN, INPUT_PULLUP);

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

  if (strlen(WIFI_SSID) == 0) {
    g_wifiOk = false;
    g_webStarted = false;
    Serial.println(F("WiFi: wifi_secrets.h yok veya WIFI_SSID bos. Web kapali."));
  } else {
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    Serial.print(F("WiFi baglaniyor"));
    unsigned long wifiStart = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - wifiStart < 20000) {
      delay(500);
      Serial.print(F("."));
    }
    Serial.println();
    if (WiFi.status() == WL_CONNECTED) {
      g_wifiOk = true;
      server.on("/", HTTP_GET, handleRoot);
      server.begin();
      g_webStarted = true;
      Serial.print(F("Web: http://"));
      Serial.println(WiFi.localIP());
    } else {
      g_wifiOk = false;
      g_webStarted = false;
      Serial.println(F("WiFi baglanamadi, web kapali."));
    }
  }
}

void loop() {
  if (g_webStarted) {
    server.handleClient();
  }

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
    float ecUsCm = tdsCalPpm * EC_US_PER_PPM;
    float phAdc = 0.0;
    float phVoltage = 0.0;
    float phValue = readPhValue(phAdc, phVoltage);

    g_ph = phValue;
    g_ecUsCm = ecUsCm;
    g_tdsPpm = tdsCalPpm;
    g_tdsRaw = tdsRawPpm;
    g_tdsAdc = avgAdc;
    g_tdsV = voltage;
    g_phAdc = phAdc;
    g_phV = phVoltage;
    Serial.print("ADC:");
    Serial.print(avgAdc, 1);
    Serial.print("  V:");
    Serial.print(voltage, 3);
    Serial.print("  TDS_raw:");
    Serial.print(tdsRawPpm, 1);
    Serial.print("  PPM_cal:");
    Serial.print(tdsCalPpm, 1);
    Serial.print("  EC_uS:");
    Serial.print(ecUsCm, 1);
    Serial.print("  PH_ADC:");
    Serial.print(phAdc, 1);
    Serial.print("  PH_V:");
    Serial.print(phVoltage, 3);
    Serial.print("  pH:");
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

    if (!debugMode) {
      display.setTextSize(1);
      display.setCursor(0, 0);
      display.print("pH:");
      display.println(g_ph, 2);
      display.setCursor(0, 24);
      display.print("EC:");
      display.println((int)g_ecUsCm);
    } else {
      display.setTextSize(1);
      display.setCursor(0, 0);
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
