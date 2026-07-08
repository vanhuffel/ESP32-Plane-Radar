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

The portal lets you choose Wi-Fi and configure these custom fields:

| Field | Example | Purpose |
| --- | --- | --- |
| Timezone | `EST5EDT,M3.2.0/2,M11.1.0/2` | POSIX timezone used for local time |
| Weather latitude | `33.6407` | Location for outside air temperature |
| Weather longitude | `-84.4277` | Location for outside air temperature |

The default weather coordinates are a generic Atlanta airport-area location.
Change them in the setup portal for your own weather feed before using OAT.

After saving Wi-Fi credentials, the device restarts and reconnects
automatically.

## Timezones

The firmware uses POSIX timezone strings because that is what the ESP32 Arduino
time library accepts.

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
