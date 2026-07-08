#include <Arduino.h>
#include <Adafruit_BME280.h>
#include <ArduinoJson.h>
#include <ESPmDNS.h>
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
constexpr char Hostname[] = "cockpit-clock";
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

struct TimezoneOption {
  const char* label;
  const char* value;
};

const TimezoneOption TimezoneOptions[] = {
    {"UTC", "UTC0"},
    {"US Eastern", "EST5EDT,M3.2.0/2,M11.1.0/2"},
    {"US Central", "CST6CDT,M3.2.0/2,M11.1.0/2"},
    {"US Mountain", "MST7MDT,M3.2.0/2,M11.1.0/2"},
    {"US Arizona", "MST7"},
    {"US Pacific", "PST8PDT,M3.2.0/2,M11.1.0/2"},
    {"Alaska", "AKST9AKDT,M3.2.0/2,M11.1.0/2"},
    {"Hawaii", "HST10"},
    {"UK", "GMT0BST,M3.5.0/1,M10.5.0/2"},
    {"Central Europe", "CET-1CEST,M3.5.0/2,M10.5.0/3"},
    {"Japan", "JST-9"},
    {"Australia Eastern", "AEST-10AEDT,M10.1.0/2,M4.1.0/3"},
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
constexpr char FindClockMenuHtml[] =
    "<form action='/find' method='get'><button>Find This Clock</button></form><br/>\n";

String htmlEscape(const char* text) {
  String out;
  if (!text) {
    return out;
  }
  while (*text) {
    switch (*text) {
      case '&':
        out += F("&amp;");
        break;
      case '<':
        out += F("&lt;");
        break;
      case '>':
        out += F("&gt;");
        break;
      case '"':
        out += F("&quot;");
        break;
      case '\'':
        out += F("&#39;");
        break;
      default:
        out += *text;
        break;
    }
    ++text;
  }
  return out;
}

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

void drawRadialLine(LGFX_Sprite& g, float angle, int inner, int outer, uint16_t color) {
  const int x1 = clockface::Cx + lroundf(cosf(angle) * inner);
  const int y1 = clockface::Cy + lroundf(sinf(angle) * inner);
  const int x2 = clockface::Cx + lroundf(cosf(angle) * outer);
  const int y2 = clockface::Cy + lroundf(sinf(angle) * outer);
  g.drawLine(x1, y1, x2, y2, color);
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

void drawFace(LGFX_Sprite& g) {
  g.fillScreen(clockface::Black);

  for (int i = 0; i < 60; ++i) {
    const float angle = (i * 6.0f - 90.0f) * DEG_TO_RAD;
    const bool fiveSecondTick = (i % 5 == 0);
    const int innerRadius = 92;
    const int tickLength = fiveSecondTick ? 16 : 8;
    const uint16_t color = fiveSecondTick ? clockface::White : clockface::Gray;
    drawRadialLine(g, angle, innerRadius, innerRadius + tickLength, color);
  }
}

void drawSecondSweep(LGFX_Sprite& g, const tm& localTm) {
  const float angle = (localTm.tm_sec * 6.0f - 90.0f) * DEG_TO_RAD;
  drawRadialLine(g, angle - 0.020f, 82, 116, clockface::White);
  drawRadialLine(g, angle - 0.010f, 82, 116, clockface::White);
  drawRadialLine(g, angle, 82, 116, clockface::White);
  drawRadialLine(g, angle + 0.010f, 82, 116, clockface::White);
  drawRadialLine(g, angle + 0.020f, 82, 116, clockface::White);
}

void drawStatusDots(LGFX_Sprite& g) {
}

void drawClockFrame() {
  tm localTm = {};
  const bool haveTime = currentTime(localTm);
  timeOk = haveTime;

  drawFace(frame);
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

  if (haveTime) {
    drawSecondSweep(frame, localTm);
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
  WiFi.setHostname(clockface::Hostname);
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

void appendFindClockPanel(String& page) {
  page += F("<div class='panel'><h2>Find This Clock</h2>");
  if (wifiLinkUp()) {
    page += F("<p>Connected to Wi-Fi as <strong>");
    page += htmlEscape(clockface::Hostname);
    page += F(".local</strong>.</p><p><a href='http://");
    page += htmlEscape(clockface::Hostname);
    page += F(".local/'>http://");
    page += htmlEscape(clockface::Hostname);
    page += F(".local/</a></p><p>Current IP: <strong>");
    page += WiFi.localIP().toString();
    page += F("</strong></p>");
  } else {
    page += F("<p>You are on the temporary setup access point. After saving Wi-Fi, "
              "this AP will disappear and the clock will join your Wi-Fi network.</p>");
  }
  page += F("<ol><li>Save Wi-Fi from this portal.</li>"
            "<li>Reconnect your phone or computer to that same Wi-Fi network.</li>"
            "<li>Open <strong>http://cockpit-clock.local/</strong>.</li>"
            "<li>If that name does not open, find <strong>cockpit-clock</strong> "
            "in your router's client list and use its IP address.</li></ol>"
            "<p class='hint'>The clock face intentionally keeps the IP address off-screen.</p>"
            "</div>");
}

void handleFindClockPage() {
  String page;
  page.reserve(3200);
  page += F("<!DOCTYPE html><html lang='en'><head><meta charset='UTF-8'>"
            "<meta name='viewport' content='width=device-width,initial-scale=1'>"
            "<title>Find Cockpit Clock</title><style>"
            "body{text-align:center;font-family:verdana;margin:0;padding:24px;background:#f7f7f7;color:#111}"
            ".wrap{text-align:left;display:inline-block;min-width:260px;max-width:520px;width:100%}"
            "h1{font-size:1.6rem;margin:.2rem 0 1rem}h2{font-size:1.1rem;margin:.2rem 0 .7rem}"
            "button{box-sizing:border-box;width:100%;padding:10px;margin:6px 0;border-radius:.3rem;"
            "cursor:pointer;border:0;background:#1fa3ec;color:#fff;line-height:2rem;font-size:1.1rem}"
            ".panel{background:#fff;border:1px solid #ddd;border-left:5px solid #1fa3ec;"
            "padding:14px 16px;margin:14px 0;border-radius:.3rem}"
            ".hint{font-size:.86rem;color:#555}a{color:#111;font-weight:700;text-decoration:none}"
            "ol{padding-left:1.4rem}</style></head><body><div class='wrap'>"
            "<h1>Cockpit Clock</h1>");
  appendFindClockPanel(page);
  page += F("<form action='/' method='get'><button type='submit'>Back</button></form>"
            "</div></body></html>");

  wifiManager.server->send(200, F("text/html"), page);
}

void handleClockSetupPage() {
  String page;
  page.reserve(6400);
  page += F("<!DOCTYPE html><html lang='en'><head><meta charset='UTF-8'>"
            "<meta name='viewport' content='width=device-width,initial-scale=1'>"
            "<title>Cockpit Clock Setup</title><style>"
            "body{text-align:center;font-family:verdana;margin:0;padding:24px;background:#f7f7f7;color:#111}"
            ".wrap{text-align:left;display:inline-block;min-width:260px;max-width:520px;width:100%}"
            "h1{font-size:1.6rem;margin:.2rem 0 1rem}h2{font-size:1.1rem;margin:.2rem 0 .7rem}"
            "label{display:block;font-weight:700;margin-top:14px}"
            "input,select,button{box-sizing:border-box;width:100%;padding:10px;margin:6px 0;font-size:1rem;border-radius:.3rem}"
            "input,select{border:1px solid #bbb;background:white}"
            "button{cursor:pointer;border:0;background:#1fa3ec;color:#fff;line-height:2rem;font-size:1.1rem}"
            ".hint{font-size:.86rem;color:#555;margin:0 0 8px}"
            ".panel{background:#fff;border:1px solid #ddd;border-left:5px solid #1fa3ec;"
            "padding:14px 16px;margin:14px 0;border-radius:.3rem}"
            "ol{padding-left:1.4rem}"
            "a{color:#111;font-weight:700;text-decoration:none}"
            "</style></head><body><div class='wrap'>"
            "<h1>Cockpit Clock Setup</h1>");
  appendFindClockPanel(page);
  page += F("<form method='POST' action='/paramsave'>"
            "<label for='timezone_picker'>Timezone</label>"
            "<select id='timezone_picker' onchange=\"if(this.value)document.getElementById('timezone').value=this.value\">");

  bool knownTimezone = false;
  for (const TimezoneOption& option : TimezoneOptions) {
    if (strcmp(timezoneValue, option.value) == 0) {
      knownTimezone = true;
      break;
    }
  }

  page += F("<option value=''");
  if (!knownTimezone) {
    page += F(" selected");
  }
  page += F(">Custom / advanced</option>");

  for (const TimezoneOption& option : TimezoneOptions) {
    page += F("<option value='");
    page += htmlEscape(option.value);
    page += F("'");
    if (strcmp(timezoneValue, option.value) == 0) {
      page += F(" selected");
    }
    page += F(">");
    page += htmlEscape(option.label);
    page += F("</option>");
  }

  page += F("</select>"
            "<label for='timezone'>Timezone string</label>"
            "<input id='timezone' name='timezone' maxlength='63' value='");
  page += htmlEscape(timezoneValue);
  page += F("' autocorrect='off' autocapitalize='none'>"
            "<p class='hint'>Use the picker for common zones, or edit the POSIX string directly.</p>"
            "<label for='weather_lat'>Weather latitude</label>"
            "<input id='weather_lat' name='weather_lat' type='number' step='0.000001' value='");
  page += htmlEscape(weatherLatValue);
  page += F("'>"
            "<label for='weather_lon'>Weather longitude</label>"
            "<input id='weather_lon' name='weather_lon' type='number' step='0.000001' value='");
  page += htmlEscape(weatherLonValue);
  page += F("'>"
            "<button type='submit'>Save</button>"
            "</form><br><form action='/' method='get'><button type='submit'>Back</button></form>"
            "</div></body></html>");

  wifiManager.server->send(200, F("text/html"), page);
}

void registerClockSetupRoutes() {
  if (!wifiManager.server) {
    return;
  }
  wifiManager.server->on(F("/find"), HTTP_GET, handleFindClockPage);
  wifiManager.server->on(F("/param"), HTTP_GET, handleClockSetupPage);
}

void startLanWebPortal() {
  wifiManager.startWebPortal();
  Serial.printf("Config portal: http://%s/\n", WiFi.localIP().toString().c_str());
  if (MDNS.begin(clockface::Hostname)) {
    MDNS.addService("http", "tcp", 80);
    Serial.printf("mDNS config portal: http://%s.local/\n", clockface::Hostname);
  } else {
    Serial.println("mDNS failed; use the printed IP address for config.");
  }
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
  wifiManager.setWebServerCallback(registerClockSetupRoutes);
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
  wifiManager.setTitle("Cockpit Clock");
  wifiManager.setCustomMenuHTML(FindClockMenuHtml);
  const char* menu[] = {"wifi", "param", "custom", "info", "restart", "exit"};
  wifiManager.setMenu(menu, 6);
  wifiManager.setAPStaticIPConfig(clockface::PortalIp, clockface::PortalGateway,
                                  clockface::PortalSubnet);

  wifiOk = connectSavedWifi();
  if (wifiOk) {
    startLanWebPortal();
  } else {
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
