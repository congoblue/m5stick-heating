// M5StickC Plus 1.1 - heating temperature display
// Polls a JSON endpoint every 10 s, shows the temperature,
// stays awake for 10 minutes then deep-sleeps until button A (big M5 button) is pressed.

#include <M5Unified.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "credentials.h"

static const char *DATA_URL  = "http://192.168.1.240:8080/data";

static const uint32_t UPDATE_INTERVAL_MS = 10UL * 1000UL;       // 10 seconds
static const uint32_t AWAKE_TIME_MS      = 10UL * 60UL * 1000UL; // 10 minutes
static const gpio_num_t WAKE_PIN         = GPIO_NUM_37;          // Button A (big front button)

// Backlight brightness levels (0-255) cycled with button B (small side button).
static const uint8_t BRIGHTNESS_LEVELS[] = {32, 80, 128, 192, 255};
static const int NUM_BRIGHTNESS = sizeof(BRIGHTNESS_LEVELS) / sizeof(BRIGHTNESS_LEVELS[0]);
RTC_DATA_ATTR static int brightnessIdx = 2;  // kept across deep sleep

static uint32_t bootMs = 0;
static uint32_t lastUpdateMs = 0;
static uint32_t lastSecondMs = 0;
static String lastTemp = "--";
static float lastTempValue = NAN;  // numeric value for colouring (NAN if unknown)
static String lastStatus = "";

// Temperature colour: red at >=45C, cyan at <=20C, sweeping the hue
// (red -> orange -> yellow -> green -> cyan) in between.
static uint16_t tempColour(float t) {
  if (isnan(t)) return TFT_WHITE;
  const float tHot = 45.0f, tCold = 20.0f;
  float f = (tHot - t) / (tHot - tCold);  // 0 at hot, 1 at cold
  f = constrain(f, 0.0f, 1.0f);
  float hue = f * 180.0f;                 // 0 = red, 180 = cyan

  // HSV -> RGB with S = V = 1, for hue in [0, 180]
  float x = 1.0f - fabsf(fmodf(hue / 60.0f, 2.0f) - 1.0f);
  float r, g, b;
  if (hue < 60)       { r = 1; g = x; b = 0; }
  else if (hue < 120) { r = x; g = 1; b = 0; }
  else                { r = 0; g = 1; b = x; }
  return M5.Display.color565((uint8_t)(r * 255), (uint8_t)(g * 255), (uint8_t)(b * 255));
}

// Battery charge 0-100 % (M5Unified estimates this from the AXP192 battery voltage).
static int batteryPercent() {
  return constrain((int)M5.Power.getBatteryLevel(), 0, 100);
}

// Battery icon in the top-right corner, filled in proportion to charge.
static void drawBattery() {
  const int x = 204, y = 4, w = 28, h = 14;  // body
  int pct = batteryPercent();
  uint16_t colour = pct > 25 ? TFT_GREEN : (pct >= 10 ? TFT_YELLOW : TFT_RED);

  M5.Display.drawRect(x, y, w, h, TFT_WHITE);
  M5.Display.fillRect(x + w, y + 4, 3, h - 8, TFT_WHITE);  // terminal nub

  int innerW = w - 4;
  int fillW = (innerW * pct + 50) / 100;
  M5.Display.fillRect(x + 2, y + 2, innerW, h - 4, TFT_BLACK);
  if (fillW > 0) M5.Display.fillRect(x + 2, y + 2, fillW, h - 4, colour);
}

// Status line + remaining awake time (redrawn every second).
static void drawFooter() {
  uint32_t elapsed = millis() - bootMs;
  uint32_t remain = elapsed < AWAKE_TIME_MS ? (AWAKE_TIME_MS - elapsed) / 1000 : 0;
  char buf[48];
  snprintf(buf, sizeof(buf), "%s  sleep %lu:%02lu", lastStatus.c_str(),
           (unsigned long)(remain / 60), (unsigned long)(remain % 60));
  M5.Display.setTextDatum(BC_DATUM);
  M5.Display.setFont(&fonts::Font2);
  M5.Display.setTextColor(TFT_DARKGREY, TFT_BLACK);
  M5.Display.setTextPadding(240);  // clear old text without flicker
  M5.Display.drawString(buf, 120, 133);
  M5.Display.setTextPadding(0);
}

static void drawScreen() {
  M5.Display.startWrite();
  M5.Display.fillScreen(TFT_BLACK);

  M5.Display.setTextDatum(TC_DATUM);
  M5.Display.setFont(&fonts::Font2);
  M5.Display.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  M5.Display.drawString("Hot Water Temperature", 120, 2);

  // 7-segment font at double size (96 px tall); fall back to normal size if too wide
  M5.Display.setFont(&fonts::Font7);
  M5.Display.setTextColor(tempColour(lastTempValue), TFT_BLACK);
  M5.Display.setTextDatum(MC_DATUM);
  M5.Display.setTextSize(2);
  if (M5.Display.textWidth(lastTemp) > 236) M5.Display.setTextSize(1);
  M5.Display.drawString(lastTemp, 120, 68);
  M5.Display.setTextSize(1);

  drawBattery();
  drawFooter();
  M5.Display.endWrite();
}

static bool ensureWifi() {
  if (WiFi.status() == WL_CONNECTED) return true;

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    delay(250);
  }
  return WiFi.status() == WL_CONNECTED;
}

static void fetchTemperature() {
  if (!ensureWifi()) {
    lastStatus = "No WiFi";
    return;
  }

  HTTPClient http;
  http.setTimeout(5000);
  http.begin(DATA_URL);
  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    lastStatus = "HTTP " + String(code);
    http.end();
    return;
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, http.getStream());
  http.end();
  if (err) {
    lastStatus = "JSON err";
    Serial.printf("JSON error: %s\n", err.c_str());
    return;
  }

  JsonVariant v = doc["v65"]["messages"]["State"]["fields"]["temp"]["value"];
  if (v.isNull()) {
    lastStatus = "No temp";
    return;
  }

  if (v.is<float>()) {
    lastTempValue = v.as<float>();
    lastTemp = String(lastTempValue, 1);
  } else {
    lastTemp = v.as<String>();
    lastTempValue = lastTemp.toFloat();  // handles numeric strings like "38.5"
    if (lastTempValue == 0.0f && !lastTemp.startsWith("0")) lastTempValue = NAN;
  }
  lastStatus = "OK";
  Serial.printf("temp = %s\n", lastTemp.c_str());
}

static void goToSleep() {
  M5.Display.fillScreen(TFT_BLACK);
  M5.Display.setTextDatum(MC_DATUM);
  M5.Display.setFont(&fonts::Font4);
  M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
  M5.Display.drawString("Sleeping...", 120, 67);
  delay(1000);

  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);

  M5.Display.setBrightness(0);

  // Wait for button release so we don't immediately wake
  while (digitalRead(WAKE_PIN) == LOW) delay(10);

  // Wake on button A. M5Unified's own wake pin is disabled (touch_wakeup = false)
  // because the ESP32 has only one ext0 source; deepSleep() also puts the LCD to sleep.
  esp_sleep_enable_ext0_wakeup(WAKE_PIN, 0);
  M5.Power.deepSleep(M5.Power.sleep_no_timer, false);
}

void setup() {
  auto cfg = M5.config();
  cfg.serial_baudrate = 115200;
  M5.begin(cfg);
  M5.Display.setRotation(3);  // landscape, button on the left
  M5.Display.setBrightness(BRIGHTNESS_LEVELS[brightnessIdx]);

  // If woken by button A, wait for release so it isn't seen as a "sleep" press
  while (digitalRead(WAKE_PIN) == LOW) delay(10);
  M5.update();

  bootMs = millis();

  M5.Display.fillScreen(TFT_BLACK);
  M5.Display.setTextDatum(MC_DATUM);
  M5.Display.setFont(&fonts::Font4);
  M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
  M5.Display.drawString("Connecting...", 120, 67);

  fetchTemperature();
  drawScreen();
  lastUpdateMs = millis();
}

void loop() {
  M5.update();
  uint32_t now = millis();

  if (now - bootMs >= AWAKE_TIME_MS) {
    goToSleep();
  }

  // Big button (A): go to sleep now
  if (M5.BtnA.wasPressed()) {
    goToSleep();
  }

  // Small side button (B): step through backlight brightness
  if (M5.BtnB.wasPressed()) {
    brightnessIdx = (brightnessIdx + 1) % NUM_BRIGHTNESS;
    M5.Display.setBrightness(BRIGHTNESS_LEVELS[brightnessIdx]);
  }

  if (now - lastUpdateMs >= UPDATE_INTERVAL_MS) {
    lastUpdateMs = now;
    fetchTemperature();
    drawScreen();
    lastSecondMs = now;
  } else if (now - lastSecondMs >= 1000) {
    lastSecondMs = now;
    drawFooter();
    drawBattery();
  }

  delay(20);
}
