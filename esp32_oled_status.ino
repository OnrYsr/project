#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
#define OLED_ADDRESS 0x3C
#define TDS_PIN 34
#define PH_PIN 35
#define MODE_BTN_PIN 27

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

const float VREF = 3.3;          // ESP32 ADC reference voltage
const float ADC_MAX = 4095.0;    // 12-bit ADC
const float WATER_TEMP_C = 25.0; // Temperature compensation reference
const float EC_US_PER_PPM = 2.0; // Your meter data: EC(uS/cm) ~= 2 * PPM
const float PH_CAL_V4 = 3.300;   // Measured voltage in pH 4 buffer
const float PH_CAL_V7 = 2.528;   // Measured voltage in pH 7 buffer
const float PH_CAL_V10 = 2.012;  // Measured voltage in pH 10 buffer

unsigned long lastUpdateMs = 0;
unsigned long lastButtonMs = 0;
bool debugMode = false;
bool lastButtonState = HIGH;
const unsigned long NORMAL_INTERVAL_MS = 30000;
const unsigned long DEBUG_INTERVAL_MS = 3000;

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
}

void loop() {
  bool buttonState = digitalRead(MODE_BTN_PIN);
  if (buttonState == LOW && lastButtonState == HIGH && millis() - lastButtonMs > 250) {
    debugMode = !debugMode;
    lastButtonMs = millis();
    lastUpdateMs = 0; // Force immediate refresh after mode change
  }
  lastButtonState = buttonState;

  unsigned long updateIntervalMs = debugMode ? DEBUG_INTERVAL_MS : NORMAL_INTERVAL_MS;
  if (millis() - lastUpdateMs >= updateIntervalMs) {
    lastUpdateMs = millis();

    float avgAdc = 0.0;
    float voltage = 0.0;
    float tdsRawPpm = readTdsPpm(avgAdc, voltage);
    float tdsCalPpm = rawToPpmCalibrated(tdsRawPpm);
    float ecUsCm = tdsCalPpm * EC_US_PER_PPM;
    float phAdc = 0.0;
    float phVoltage = 0.0;
    float phValue = readPhValue(phAdc, phVoltage);

    display.clearDisplay();
    display.setRotation(2);  // Keep 180-degree orientation
    display.setTextColor(SSD1306_WHITE);

    if (!debugMode) {
      display.setTextSize(1);
      display.setCursor(0, 0);
      display.print("pH:");
      display.println(phValue, 1);
      display.setCursor(0, 24);
      display.print("EC:");
      display.println((int)ecUsCm);
    } else {
      display.setTextSize(1);
      display.setCursor(0, 0);
      display.print("RAW:");
      display.println((int)tdsRawPpm);
      display.print("TDS:");
      display.print((int)tdsCalPpm);
      display.println(" ppm");
      display.print("EC:");
      display.print((int)ecUsCm);
      display.println(" uS");
      display.print("pH:");
      display.print(phValue, 2);
      display.print(" pA:");
      display.println((int)phAdc);
      display.print("pV:");
      display.println(phVoltage, 3);
    }
    display.display();

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
}
