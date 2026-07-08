# Cockpit Clock + BME280

Alternate firmware for the Plane Radar hardware: an aviation-style round clock
for an ESP32-C3 Super Mini, a 1.28 inch GC9A01 round display, and a BME280
temperature/humidity/pressure sensor.

The display is inspired by a transport-category cockpit clock: black face,
white tick marks, seven-segment time, cabin temperature, outside air
temperature, and relative humidity.

## Features

- Local time from NTP with a configurable POSIX timezone string
- First-time Wi-Fi setup with WiFiManager
- BME280 cabin temperature and relative humidity
- Outside air temperature from Open-Meteo by latitude/longitude
- Full-screen sprite drawing to avoid the visible second-by-second flash
- No buttons required

## Hardware

| Part | Notes |
| --- | --- |
| ESP32-C3 Super Mini | Built with PlatformIO board `esp32-c3-devkitm-1` |
| 1.28 inch round GC9A01 display | 240 x 240 SPI display |
| BME280 sensor breakout | I2C, works at address `0x77` or `0x76` |
| Jumper wires / headers | Soldered headers are fine |

## Wiring

### Display

| Display pin | ESP32-C3 pin |
| --- | --- |
| VCC | 3V3 |
| GND | GND |
| RST | GPIO0 |
| CS | GPIO1 |
| DC | GPIO10 |
| SDA / MOSI | GPIO3 |
| SCL / SCLK | GPIO4 |

### BME280

| BME280 pin | ESP32-C3 pin |
| --- | --- |
| VCC | 3V3 |
| GND | GND |
| SDA | GPIO6 |
| SCL | GPIO7 |

## Build And Flash

Install PlatformIO, then build from this folder:

```powershell
cd cockpit-clock-bme280
pio run -e supermini
```

Flash the ESP32-C3:

```powershell
pio run -e supermini -t upload
```

Open the serial monitor at 115200 baud:

```powershell
pio device monitor -b 115200
```

## First-Time Setup

If no saved Wi-Fi credentials exist, the device starts a setup access point:

- SSID: `CockpitClock-Setup`
- Setup URL: `http://192.168.44.1`

Typical phone setup flow:

1. Flash the ESP32-C3.
2. On your phone, join Wi-Fi network `CockpitClock-Setup`.
3. Open `http://192.168.44.1` if the captive portal does not open itself.
4. Choose **Configure WiFi** and save your home Wi-Fi.
5. Reconnect your phone to that same home Wi-Fi.
6. Open `http://cockpit-clock.local/`.

The portal also includes **Find This Clock**, which explains this flow on the
temporary setup AP and shows the clock's LAN IP after it joins Wi-Fi.

The separate **Setup** page lets you configure clock/weather fields:

| Field | Example | Purpose |
| --- | --- | --- |
| Timezone | `US Eastern` | Picker for common timezones |
| Timezone string | `EST5EDT,M3.2.0/2,M11.1.0/2` | Advanced POSIX timezone override |
| Weather latitude | `33.6407` | Location for outside air temperature |
| Weather longitude | `-84.4277` | Location for outside air temperature |

The default weather coordinates are a generic Atlanta airport-area location.
Change them in the setup portal for your own weather feed before using OAT.

After saving Wi-Fi credentials, the device restarts and reconnects
automatically.

## Reconfigure After Wi-Fi Is Saved

When the clock is connected to Wi-Fi, the config page stays available on the
device's LAN address:

- `http://cockpit-clock.local/`
- `http://<device-ip>/`

The serial log prints the current IP address at boot. If `.local` does not
resolve on your computer or phone, use the IP address from your router's client
list or the serial monitor. The portal home page also has **Find This Clock**,
which shows the current LAN address without putting the IP on the clock face.
Choose **Setup** on the portal home page to change timezone and weather
location.

## Timezones

The setup page includes a picker for common timezones. Under the picker, the
firmware still stores a POSIX timezone string because that is what the ESP32
Arduino time library accepts.

Common examples:

| Region | Timezone string |
| --- | --- |
| US Eastern | `EST5EDT,M3.2.0/2,M11.1.0/2` |
| US Central | `CST6CDT,M3.2.0/2,M11.1.0/2` |
| US Mountain | `MST7MDT,M3.2.0/2,M11.1.0/2` |
| US Pacific | `PST8PDT,M3.2.0/2,M11.1.0/2` |
| UTC | `UTC0` |

## Weather

Outside air temperature is fetched from Open-Meteo roughly every 15 minutes.
No API key is required. Cabin temperature and relative humidity come from the
BME280 and update locally every few seconds.

## Display Layout

- Timezone abbreviation above the time
- Local time in seven-segment numerals with leading zeros
- CABIN and OAT temperatures centered between time and humidity
- RH value at the lower center of the clock face
- Pressure is still logged on serial, but not shown on the face

## Notes

- If the setup AP disappears after saving, the ESP32 is probably rebooting and
  trying the saved Wi-Fi credentials.
- If the display is blank, check the SPI pins first: DC, CS, MOSI, SCLK, and
  RST are all required.
- If BME280 data is missing, confirm 3.3 V power and try an I2C scanner. This
  firmware probes both `0x77` and `0x76`.
