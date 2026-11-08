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
static const int LED_PIN                 = 10;                   // red LED, active LOW

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
static bool heating = false;  // boiler state == "Water" (heating the hot water)

enum PowerState { ON_BATTERY, CHARGING, CHARGED };
static PowerState powerState = ON_BATTERY;

// USB present + AXP192 charge flag -> on battery / charging / fully charged.
static PowerState readPowerState() {
  auto &axp = M5.Power.Axp192;
  if (!axp.isVBUS() && !axp.isACIN()) return ON_BATTERY;
  return axp.isCharging() ? CHARGING : CHARGED;
}

// Red LED on while charging, off otherwise (the StickC Plus has no green LED).
static void updateLed() {
  digitalWrite(LED_PIN, powerState == CHARGING ? LOW : HIGH);
}

// Temperature colour: red at >=tHot, blue at <=tCold, sweeping the hue
// (red -> orange -> yellow -> green -> cyan -> blue) in between.
static uint16_t tempColour(float t) {
  if (isnan(t)) return TFT_WHITE;
  const float tHot = 42.0f, tCold = 10.0f;
  float f = (tHot - t) / (tHot - tCold);  // 0 at hot, 1 at cold
  f = constrain(f, 0.0f, 1.0f);
  float hue = f * 240.0f;                 // 0 = red, 180 = cyan, 240 = blue

  // HSV -> RGB with S = V = 1, for hue in [0, 240]
  float x = 1.0f - fabsf(fmodf(hue / 60.0f, 2.0f) - 1.0f);
  float r, g, b;
  if (hue < 60)       { r = 1; g = x; b = 0; }
  else if (hue < 120) { r = x; g = 1; b = 0; }
  else if (hue < 180) { r = 0; g = 1; b = x; }
  else                { r = 0; g = x; b = 1; }
  return M5.Display.color565((uint8_t)(r * 255), (uint8_t)(g * 255), (uint8_t)(b * 255));
}

// Battery charge 0-100 % (M5Unified estimates this from the AXP192 battery voltage).
static int batteryPercent() {
  return constrain((int)M5.Power.getBatteryLevel(), 0, 100);
}

// Battery icon in the top-right corner, filled in proportion to charge.
static void drawBattery() {
  const int x = 204, y = 4, w = 28, h = 14;  // body
  int pct = powerState == CHARGED ? 100 : batteryPercent();
  uint16_t colour = pct > 25 ? TFT_GREEN : (pct >= 10 ? TFT_YELLOW : TFT_RED);

  M5.Display.drawRect(x, y, w, h, TFT_WHITE);
  M5.Display.fillRect(x + w, y + 4, 3, h - 8, TFT_WHITE);  // terminal nub

  int innerW = w - 4;
  int fillW = (innerW * pct + 50) / 100;
  M5.Display.fillRect(x + 2, y + 2, innerW, h - 4, TFT_BLACK);
  if (fillW > 0) M5.Display.fillRect(x + 2, y + 2, fillW, h - 4, colour);

  // Lightning bolt while charging: white with a black outline so it shows on any fill
  if (powerState == CHARGING) {
    const int cx = x + w / 2, cy = y + h / 2;
    for (int pass = 0; pass < 2; pass++) {
      int o = pass == 0 ? 1 : 0;
      uint16_t c = pass == 0 ? TFT_BLACK : TFT_WHITE;
      for (int dx = -o; dx <= o; dx++) {
        M5.Display.fillTriangle(cx + 3 + dx, cy - 6, cx - 4 + dx, cy + 1, cx + 1 + dx, cy + 1, c);
        M5.Display.fillTriangle(cx - 1 + dx, cy - 1, cx + 4 + dx, cy - 1, cx - 3 + dx, cy + 6, c);
      }
    }
  }
}

// Status line + remaining awake time (redrawn every second).
// On USB power the sleep countdown is hidden (the device stays awake).
static void drawFooter() {
  char buf[48];
  if (powerState != ON_BATTERY) {
    snprintf(buf, sizeof(buf), "%s", lastStatus.c_str());
  } else {
    uint32_t elapsed = millis() - bootMs;
    uint32_t remain = elapsed < AWAKE_TIME_MS ? (AWAKE_TIME_MS - elapsed) / 1000 : 0;
    snprintf(buf, sizeof(buf), "%s  sleep %lu:%02lu", lastStatus.c_str(),
             (unsigned long)(remain / 60), (unsigned long)(remain % 60));
  }
  M5.Display.setTextDatum(BL_DATUM);  // bottom-left; bottom-right is the HEATING label
  M5.Display.setFont(&fonts::Font2);
  M5.Display.setTextColor(TFT_DARKGREY, TFT_BLACK);
  M5.Display.setTextPadding(165);  // clear old text without flicker
  M5.Display.drawString(buf, 2, 133);
  M5.Display.setTextPadding(0);
}

// "HEATING" in the bottom-right corner while the boiler is heating the hot water.
static void drawHeating() {
  if (!heating) return;
  M5.Display.setTextDatum(BR_DATUM);
  M5.Display.setFont(&fonts::Font2);
  M5.Display.setTextColor(TFT_RED, TFT_BLACK);
  M5.Display.drawString("HEATING", 238, 133);
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
  drawHeating();
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

  JsonVariant fields = doc["v65"]["messages"]["State"]["fields"];
  heating = (fields["state"]["value"] == "water");

  JsonVariant v = fields["temp"]["value"];
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
  digitalWrite(LED_PIN, HIGH);  // LED off

  M5.Display.setBrightness(0);

  // Wait for button release so we don't immediately wake
  while (digitalRead(WAKE_PIN) == LOW) delay(10);

  // Cut power to everything except the ESP32 (DCDC1) to minimise sleep current.
  // M5.begin() restores these on wake.
  M5.Imu.sleep();                     // MPU6886 IMU (~3 mA when active)
  M5.Display.sleep();                 // ST7789 into sleep before its supply is cut
  auto &axp = M5.Power.Axp192;
  axp.setLDO2(0);                     // LCD backlight
  axp.setLDO3(0);                     // LCD logic power
  axp.setLDO0(0);                     // microphone
  axp.setGPIO0(false);
  axp.setEXTEN(false);                // 5V boost for HAT / Grove
  axp.setAdcState(false);             // AXP192 measurement ADCs

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

  pinMode(LED_PIN, OUTPUT);
  powerState = readPowerState();
  updateLed();

  // If woken by button A, wait for release so it isn't seen as a "sleep" press
  while (digitalRead(WAKE_PIN) == LOW) delay(10);
  M5.BtnA.setHoldThresh(2000);  // long press (2 s) on button A = sleep
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

  // On USB power (charging or charged) the sleep timer is held at zero
  if (powerState != ON_BATTERY) {
    bootMs = now;
  } else if (now - bootMs >= AWAKE_TIME_MS) {
    goToSleep();
  }

  // Big button (A): hold 2 s to sleep, short press resets the sleep timer
  if (M5.BtnA.wasHold()) {
    goToSleep();
  } else if (M5.BtnA.wasClicked()) {
    bootMs = now;
    drawFooter();
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
    PowerState ps = readPowerState();
    if (ps != powerState) {
      powerState = ps;
      updateLed();
    }
    drawFooter();
    drawBattery();
  }

  delay(20);
}
