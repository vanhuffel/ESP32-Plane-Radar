#include <Arduino.h>
#include <Adafruit_BME280.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <LovyanGFX.hpp>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <Wire.h>
#include <time.h>

namespace pins {
constexpr int DisplayRst = 0;
constexpr int DisplayCs = 1;
constexpr int DisplayDc = 10;
constexpr int DisplayMosi = 3;
constexpr int DisplaySclk = 4;
constexpr int I2cSda = 6;
constexpr int I2cScl = 7;
}  // namespace pins

namespace clockface {
constexpr int W = 240;
constexpr int H = 240;
constexpr int Cx = 120;
constexpr int Cy = 120;
constexpr char WifiApName[] = "CockpitClock-Setup";
const IPAddress PortalIp(192, 168, 44, 1);
const IPAddress PortalGateway(192, 168, 44, 1);
const IPAddress PortalSubnet(255, 255, 255, 0);
constexpr char DefaultTimezone[] = "EST5EDT,M3.2.0/2,M11.1.0/2";
constexpr char DefaultWeatherLat[] = "33.6407";
constexpr char DefaultWeatherLon[] = "-84.4277";
constexpr unsigned long SensorIntervalMs = 5000;
constexpr unsigned long WeatherIntervalMs = 15UL * 60UL * 1000UL;
constexpr unsigned long FrameIntervalMs = 1000;
constexpr unsigned long WifiAttemptMs = 15000;
constexpr uint16_t White = TFT_WHITE;
constexpr uint16_t Gray = 0x7BEF;
constexpr uint16_t Dim = 0x39E7;
constexpr uint16_t Amber = 0xFBE0;
constexpr uint16_t Green = 0x07E0;
constexpr uint16_t Black = TFT_BLACK;
}  // namespace clockface

class CockpitDisplay : public lgfx::LGFX_Device {
  lgfx::Bus_SPI bus_;
  lgfx::Panel_GC9A01 panel_;

public:
  CockpitDisplay() {
    {
      auto cfg = bus_.config();
      cfg.spi_host = SPI2_HOST;
      cfg.freq_write = 40000000;
      cfg.pin_sclk = pins::DisplaySclk;
      cfg.pin_mosi = pins::DisplayMosi;
      cfg.pin_miso = -1;
      cfg.pin_dc = pins::DisplayDc;
      bus_.config(cfg);
      panel_.setBus(&bus_);
    }
    {
      auto cfg = panel_.config();
      cfg.pin_cs = pins::DisplayCs;
      cfg.pin_rst = pins::DisplayRst;
      cfg.invert = true;
      cfg.rgb_order = true;
      panel_.config(cfg);
    }
    setPanel(&panel_);
  }
};

struct SensorReadings {
  float tempF = NAN;
  float humidity = NAN;
  float pressureIn = NAN;
  float oatF = NAN;
};

CockpitDisplay display;
LGFX_Sprite frame(&display);
Adafruit_BME280 bme;
WiFiManager wifiManager;
SensorReadings readings;
Preferences prefs;

uint8_t bmeAddress = 0;
bool wifiOk = false;
bool timeOk = false;
bool restartAfterWifiSave = false;
unsigned long lastFrameMs = 0;
unsigned long lastSensorMs = 0;
unsigned long lastWeatherMs = 0;
unsigned long restartAtMs = 0;
char timezoneValue[64] = "EST5EDT,M3.2.0/2,M11.1.0/2";
WiFiManagerParameter timezoneParam("timezone", "Timezone", timezoneValue,
                                   sizeof(timezoneValue));
char weatherLatValue[16] = "33.6407";
char weatherLonValue[16] = "-84.4277";
WiFiManagerParameter weatherLatParam("weather_lat", "Weather latitude",
                                     weatherLatValue, sizeof(weatherLatValue),
                                     " type=\"number\" step=\"0.000001\"");
WiFiManagerParameter weatherLonParam("weather_lon", "Weather longitude",
                                     weatherLonValue, sizeof(weatherLonValue),
                                     " type=\"number\" step=\"0.000001\"");

bool beginBme() {
  if (bme.begin(0x77, &Wire)) {
    bmeAddress = 0x77;
    return true;
  }
  if (bme.begin(0x76, &Wire)) {
    bmeAddress = 0x76;
    return true;
  }
  bmeAddress = 0;
  return false;
}

void readSensor() {
  if (!bmeAddress && !beginBme()) {
    readings = {};
    return;
  }
  readings.tempF = bme.readTemperature() * 9.0f / 5.0f + 32.0f;
  readings.humidity = bme.readHumidity();
  readings.pressureIn = bme.readPressure() / 3386.389f;
}

void fetchOutdoorWeather() {
  if (WiFi.status() != WL_CONNECTED) {
    return;
  }

  char url[192];
  snprintf(url, sizeof(url),
           "http://api.open-meteo.com/v1/forecast?latitude=%.6f&longitude=%.6f"
           "&current=temperature_2m&temperature_unit=fahrenheit&forecast_days=1",
           atof(weatherLatValue), atof(weatherLonValue));

  HTTPClient http;
  http.setTimeout(7000);
  if (!http.begin(url)) {
    Serial.println("Weather HTTP begin failed");
    return;
  }

  const int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("Weather HTTP failed: %d\n", code);
    http.end();
    return;
  }

  const String payload = http.getString();
  http.end();

  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, payload);
  if (error) {
    Serial.printf("Weather JSON failed: %s\n", error.c_str());
    Serial.println(payload.substring(0, 120));
    return;
  }

  if (doc["current"]["temperature_2m"].is<float>()) {
    readings.oatF = doc["current"]["temperature_2m"].as<float>();
    Serial.printf("OAT: %.1f F\n", readings.oatF);
  }
}

void loadConfig() {
  strlcpy(timezoneValue, clockface::DefaultTimezone, sizeof(timezoneValue));
  strlcpy(weatherLatValue, clockface::DefaultWeatherLat, sizeof(weatherLatValue));
  strlcpy(weatherLonValue, clockface::DefaultWeatherLon, sizeof(weatherLonValue));
  if (prefs.begin("clock", true)) {
    const String saved = prefs.getString("tz", clockface::DefaultTimezone);
    const String savedLat = prefs.getString("wx_lat", clockface::DefaultWeatherLat);
    const String savedLon = prefs.getString("wx_lon", clockface::DefaultWeatherLon);
    prefs.end();
    if (saved.length() > 0 && saved.length() < sizeof(timezoneValue)) {
      strlcpy(timezoneValue, saved.c_str(), sizeof(timezoneValue));
    }
    if (savedLat.length() > 0 && savedLat.length() < sizeof(weatherLatValue)) {
      strlcpy(weatherLatValue, savedLat.c_str(), sizeof(weatherLatValue));
    }
    if (savedLon.length() > 0 && savedLon.length() < sizeof(weatherLonValue)) {
      strlcpy(weatherLonValue, savedLon.c_str(), sizeof(weatherLonValue));
    }
  }
  timezoneParam.setValue(timezoneValue, sizeof(timezoneValue));
  weatherLatParam.setValue(weatherLatValue, sizeof(weatherLatValue));
  weatherLonParam.setValue(weatherLonValue, sizeof(weatherLonValue));
}

void saveConfigFromPortal() {
  const char* timezoneCandidate = timezoneParam.getValue();
  if (timezoneCandidate && strlen(timezoneCandidate) > 0 &&
      strlen(timezoneCandidate) < sizeof(timezoneValue)) {
    strlcpy(timezoneValue, timezoneCandidate, sizeof(timezoneValue));
  }

  const char* latCandidate = weatherLatParam.getValue();
  const char* lonCandidate = weatherLonParam.getValue();
  if (latCandidate && strlen(latCandidate) > 0 &&
      strlen(latCandidate) < sizeof(weatherLatValue)) {
    strlcpy(weatherLatValue, latCandidate, sizeof(weatherLatValue));
  }
  if (lonCandidate && strlen(lonCandidate) > 0 &&
      strlen(lonCandidate) < sizeof(weatherLonValue)) {
    strlcpy(weatherLonValue, lonCandidate, sizeof(weatherLonValue));
  }

  if (prefs.begin("clock", false)) {
    prefs.putString("tz", timezoneValue);
    prefs.putString("wx_lat", weatherLatValue);
    prefs.putString("wx_lon", weatherLonValue);
    prefs.end();
  }
  Serial.printf("Config saved: TZ=%s weather=%s,%s\n", timezoneValue,
                weatherLatValue, weatherLonValue);
}

bool currentTime(tm& localTm) {
  time_t now = time(nullptr);
  if (now < 1700000000) {
    return false;
  }
  localtime_r(&now, &localTm);
  return true;
}

void drawText(LGFX_Sprite& g, const char* text, int x, int y, int size, uint16_t color,
              textdatum_t datum = middle_center) {
  g.setFont(nullptr);
  g.setTextSize(size);
  g.setTextColor(color, clockface::Black);
  g.setTextDatum(datum);
  g.drawString(text, x, y);
}

void drawSevenText(LGFX_Sprite& g, const char* text, int x, int y, uint16_t color,
                   float scale = 1.0f) {
  g.setFont(&fonts::Font7);
  g.setTextSize(scale);
  g.setTextColor(color, clockface::Black);
  g.setTextDatum(middle_center);
  g.drawString(text, x, y);
  g.setFont(nullptr);
  g.setTextSize(1);
}

void drawPercentMark(LGFX_Sprite& g, int x, int y, uint16_t color) {
  g.drawCircle(x - 5, y - 6, 2, color);
  g.drawCircle(x + 5, y + 6, 2, color);
  g.drawLine(x + 7, y - 9, x - 7, y + 9, color);
  g.drawLine(x + 8, y - 9, x - 6, y + 9, color);
}

const char* timezoneAbbreviation(const tm& localTm) {
  if (localTm.tm_isdst > 0 && tzname[1] && tzname[1][0]) {
    return tzname[1];
  }
  if (tzname[0] && tzname[0][0]) {
    return tzname[0];
  }
  return "LOCAL";
}

void drawFace(LGFX_Sprite& g, const tm* localTm) {
  g.fillScreen(clockface::Black);
  g.drawCircle(clockface::Cx, clockface::Cy, 118, clockface::Dim);
  g.drawCircle(clockface::Cx, clockface::Cy, 112, clockface::Gray);

  for (int i = 0; i < 60; ++i) {
    const float angle = (i * 6.0f - 90.0f) * DEG_TO_RAD;
    const int outer = 105;
    const int inner = (i % 5 == 0) ? 90 : 98;
    const uint16_t color = (i % 5 == 0) ? clockface::White : clockface::Gray;
    const int x1 = clockface::Cx + cosf(angle) * inner;
    const int y1 = clockface::Cy + sinf(angle) * inner;
    const int x2 = clockface::Cx + cosf(angle) * outer;
    const int y2 = clockface::Cy + sinf(angle) * outer;
    g.drawLine(x1, y1, x2, y2, color);
  }

  if (localTm) {
    const float angle = (localTm->tm_sec * 6.0f - 90.0f) * DEG_TO_RAD;
    const int x = clockface::Cx + cosf(angle) * 82;
    const int y = clockface::Cy + sinf(angle) * 82;
    g.fillCircle(x, y, 3, clockface::Green);
  }
}

void drawStatusDots(LGFX_Sprite& g) {
}

void drawClockFrame() {
  tm localTm = {};
  const bool haveTime = currentTime(localTm);
  timeOk = haveTime;

  drawFace(frame, haveTime ? &localTm : nullptr);
  drawStatusDots(frame);

  char line[32];
  drawText(frame, haveTime ? timezoneAbbreviation(localTm) : "LOCAL", 120, 53, 1,
           clockface::White);

  if (haveTime) {
    snprintf(line, sizeof(line), "%02d:%02d", localTm.tm_hour, localTm.tm_min);
    const int timeX = (localTm.tm_hour >= 10 && localTm.tm_hour <= 19) ? 115 : 120;
    drawSevenText(frame, line, timeX, 88, clockface::White);
  } else {
    drawSevenText(frame, "--:--", 120, 88, clockface::Amber);
    drawText(frame, "SYNC", 120, 134, 2, clockface::Amber);
  }

  if (bmeAddress && !isnan(readings.tempF)) {
    snprintf(line, sizeof(line), "%.1fF", readings.tempF);
    drawSevenText(frame, line, 84, 142, clockface::White, 0.5f);
    drawText(frame, "CABIN", 84, 163, 1, clockface::White);

    if (!isnan(readings.oatF)) {
      snprintf(line, sizeof(line), "%.1fF", readings.oatF);
      drawSevenText(frame, line, 156, 142, clockface::White, 0.5f);
    } else {
      drawSevenText(frame, "--.-F", 156, 142, clockface::Amber, 0.5f);
    }
    drawText(frame, "OAT", 156, 163, 1, clockface::White);

    drawText(frame, "RH", 90, 184, 1, clockface::White);
    snprintf(line, sizeof(line), "%.0f", readings.humidity);
    drawSevenText(frame, line, 120, 184, clockface::White, 0.5f);
    drawPercentMark(frame, 150, 184, clockface::White);
  } else {
    drawText(frame, "NO BME", 120, 218, 1, clockface::Amber);
  }

  if (WiFi.status() != WL_CONNECTED) {
    drawText(frame, "SETUP 192.168.44.1", 120, 42, 1, clockface::Amber);
  }

  frame.pushSprite(0, 0);
}

void showBootMessage(const char* msg) {
  frame.fillScreen(clockface::Black);
  frame.drawCircle(clockface::Cx, clockface::Cy, 112, clockface::Gray);
  drawText(frame, "COCKPIT", 120, 86, 2, clockface::White);
  drawText(frame, "CLOCK", 120, 112, 2, clockface::White);
  drawText(frame, msg, 120, 148, 1, clockface::Amber);
  frame.pushSprite(0, 0);
}

void setupTime() {
  setenv("TZ", timezoneValue, 1);
  tzset();
  configTzTime(timezoneValue, "pool.ntp.org", "time.nist.gov", "time.google.com");
  Serial.printf("Timezone active: %s\n", timezoneValue);
}

bool wifiLinkUp() {
  return WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0);
}

bool waitForWifi(unsigned long timeoutMs) {
  const unsigned long deadline = millis() + timeoutMs;
  while (millis() < deadline) {
    if (wifiLinkUp()) {
      return true;
    }
    delay(100);
  }
  return wifiLinkUp();
}

bool connectSavedWifi() {
  Serial.println("Connecting to saved WiFi credentials.");
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  WiFi.setTxPower(WIFI_POWER_8_5dBm);

  for (int attempt = 1; attempt <= 3; ++attempt) {
    if (attempt > 1) {
      Serial.printf("WiFi retry %d/3\n", attempt);
      WiFi.disconnect(false, false);
      delay(400);
    }
    WiFi.begin();
    if (waitForWifi(clockface::WifiAttemptMs)) {
      Serial.printf("WiFi connected: %s  IP %s\n", WiFi.SSID().c_str(),
                    WiFi.localIP().toString().c_str());
      return true;
    }
  }

  Serial.println("Saved WiFi could not connect.");
  return false;
}

void startSetupPortal() {
  WiFi.disconnect(false, false);
  WiFi.mode(WIFI_AP_STA);
  delay(50);
  wifiManager.setConfigPortalBlocking(false);
  wifiManager.startConfigPortal(clockface::WifiApName);
  Serial.println("WiFi not connected; setup portal is available as CockpitClock-Setup.");
}

void setupWifi() {
  showBootMessage("WIFI");

  wifiManager.setConnectTimeout(15);
  wifiManager.setConnectRetries(1);
  wifiManager.setConfigPortalBlocking(false);
  wifiManager.setWiFiAutoReconnect(true);
  wifiManager.setSaveConnect(false);
  wifiManager.setSaveConfigCallback([]() {
    saveConfigFromPortal();
    restartAfterWifiSave = true;
    restartAtMs = millis() + 3000;
    Serial.println("WiFi credentials saved; restarting.");
  });
  wifiManager.setSaveParamsCallback([]() {
    saveConfigFromPortal();
    setenv("TZ", timezoneValue, 1);
    tzset();
  });
  wifiManager.addParameter(&timezoneParam);
  wifiManager.addParameter(&weatherLatParam);
  wifiManager.addParameter(&weatherLonParam);
  wifiManager.setAPStaticIPConfig(clockface::PortalIp, clockface::PortalGateway,
                                  clockface::PortalSubnet);

  wifiOk = connectSavedWifi();
  if (!wifiOk) {
    startSetupPortal();
  }
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.println("Cockpit Clock + BME280");
  loadConfig();

  display.init();
  display.setRotation(0);
  frame.setColorDepth(8);
  if (!frame.createSprite(clockface::W, clockface::H)) {
    display.fillScreen(TFT_BLACK);
    display.setTextColor(TFT_RED, TFT_BLACK);
    display.drawString("SPRITE FAIL", 60, 110);
    Serial.println("Failed to allocate display sprite");
    delay(3000);
  }

  Wire.begin(pins::I2cSda, pins::I2cScl);
  Wire.setClock(100000);
  if (beginBme()) {
    Serial.printf("BME280 found at 0x%02X\n", bmeAddress);
    readSensor();
  } else {
    Serial.println("BME280 not found");
  }

  setupWifi();
  setupTime();
  fetchOutdoorWeather();
  showBootMessage("SYNC");
  delay(500);
}

void loop() {
  const unsigned long nowMs = millis();
  wifiManager.process();
  wifiOk = WiFi.status() == WL_CONNECTED;

  if (restartAfterWifiSave && millis() >= restartAtMs) {
    showBootMessage("REBOOT");
    delay(250);
    ESP.restart();
  }

  if (nowMs - lastSensorMs >= clockface::SensorIntervalMs || lastSensorMs == 0) {
    lastSensorMs = nowMs;
    readSensor();
    if (bmeAddress) {
      Serial.printf("BME280 0x%02X: %.2f F, %.1f %%RH, %.2f IN\n",
                    bmeAddress, readings.tempF, readings.humidity, readings.pressureIn);
    }
  }

  if (wifiOk &&
      (nowMs - lastWeatherMs >= clockface::WeatherIntervalMs || lastWeatherMs == 0)) {
    lastWeatherMs = nowMs;
    fetchOutdoorWeather();
  }

  if (nowMs - lastFrameMs >= clockface::FrameIntervalMs || lastFrameMs == 0) {
    lastFrameMs = nowMs;
    drawClockFrame();
  }
}
