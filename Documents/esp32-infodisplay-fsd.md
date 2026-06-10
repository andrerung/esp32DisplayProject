# ESP32 InfoDisplay — Functional Specification Document (FSD)

**Version**: 0.1
**Date**: 2026-06-08
**Status**: Draft

---

## 1. System Overview

### Purpose

The ESP32 InfoDisplay is a compact, always-on information panel based on an ESP32-C3
microcontroller driving a 2.8" TFT LCD. It shows live data — time, weather, and
configurable cryptocurrency prices — on a home network-connected device managed
entirely over WiFi via a built-in HTTP configuration portal.

### Problem Statement

Home users often want a standalone "at-a-glance" dashboard for daily information
without maintaining a full PC or tablet. This device provides a self-contained,
low-power display that fetches and presents live data automatically after minimal
one-time setup.

### Users / Stakeholders

- **Owner/User**: The home user who mounts and views the device.
- **Administrator**: Same person; configures WiFi, API keys, and display preferences
  via the web portal.

### Goals

- Display time, date, weather, and cryptocurrency prices on a 2.8" TFT.
- Be configurable from any browser on the home network.
- Support OTA firmware updates for maintenance-free operation.
- Provide structured logging (serial + UDP) for debugging.

### Non-Goals

- Cloud sync or remote access from outside the home network (Phases 1–3 scope).
- Touch interface (display is view-only in initial phases).
- Battery / low-power operation (mains-powered assumed).

### High-Level System Flow

```
Boot → NVS load config → Display splash → WiFi connect → SNTP sync
     → First data fetch → Render UI → HTTP server start
     → Periodic fetch loop (weather + crypto) + clock tick
```

---

## 2. System Architecture

### 2.1 Logical Architecture

```
┌───────────────────────────────────────────────────────────┐
│                    ESP32-C3 InfoDisplay                    │
│                                                            │
│  ┌──────────┐  ┌────────────┐  ┌──────────┐  ┌────────┐  │
│  │ Display  │  │   Data     │  │  HTTP    │  │  OTA   │  │
│  │ Manager  │  │  Fetcher   │  │  Server  │  │Handler │  │
│  └────┬─────┘  └─────┬──────┘  └────┬─────┘  └───┬────┘  │
│       │               │              │             │       │
│  ┌────▼───────────────▼──────────────▼─────────────▼────┐ │
│  │                  WiFi / TCP-IP Stack                  │ │
│  └───────────────────────────────────────────────────────┘ │
│  ┌─────────────────────┐  ┌──────────────────────────────┐ │
│  │   NVS (config)      │  │  Logging (serial + UDP)      │ │
│  └─────────────────────┘  └──────────────────────────────┘ │
└───────────────────────────────────────────────────────────┘
          │ SPI                          │ WiFi
    ┌─────▼──────┐               ┌───────▼──────────┐
    │ ILI9341    │               │  Home Network    │
    │ 2.8" TFT  │               │  (Router / AP)   │
    └────────────┘               └──────┬───────────┘
                                        │
                            ┌───────────┼────────────┐
                      ┌─────▼────┐ ┌────▼────┐ ┌─────▼─────┐
                      │ Weather  │ │ Crypto  │ │   SNTP    │
                      │   API    │ │   API   │ │  Server   │
                      └──────────┘ └─────────┘ └───────────┘
```

Data flows:
1. **SNTP** → time sync on boot and every 6 h.
2. **Weather API** → HTTP GET polled every 10 min (configurable).
3. **Crypto API** → HTTP GET polled every 60 s (configurable).
4. **Display Manager** renders time / weather / crypto onto TFT each second.
5. **HTTP Server** serves config portal and processes config POSTs.
6. **OTA Handler** downloads new firmware on `POST /ota` trigger and reboots.

### 2.2 Hardware / Platform Architecture

| Component | Part | Interface | Notes |
|-----------|------|-----------|-------|
| MCU | ESP32-C3 | — | RISC-V, 160 MHz, 4 MB flash, WiFi 802.11 b/g/n |
| Display | GMT028 TFT 2.8" (ILI9341) | SPI | 320×240 px, 262K colors |
| Power | USB 5 V → 3.3 V LDO | — | Mains-powered (assumed) |

**ESP32-C3 → ILI9341 SPI pin mapping (assumed — confirm in Phase 1):**

| ESP32-C3 GPIO | ILI9341 Pin | Function |
|---------------|-------------|----------|
| GPIO 6 | MOSI / SDI | SPI data out |
| GPIO 4 | SCK | SPI clock |
| GPIO 7 | CS | Chip select |
| GPIO 5 | DC | Data / Command select |
| GPIO 3 | RST | Hardware reset |
| GPIO 8 | LED / BL | Backlight enable |

### 2.3 Software Architecture

**FreeRTOS Task Map:**

| Task | Stack | Priority | Responsibility |
|------|-------|----------|----------------|
| `wifi_manager_task` | 4 KB | 5 | STA connect / auto-reconnect |
| `sntp_task` | 2 KB | 3 | Time sync on connect |
| `data_fetch_task` | 6 KB | 3 | Weather + crypto API polling |
| `display_task` | 8 KB | 4 | Render UI tiles to TFT |
| `http_server_task` | 6 KB | 3 | Config portal |
| `ota_task` | 8 KB | 2 | OTA download + flash |
| `log_task` | 2 KB | 1 | UDP log forwarding |

**Boot sequence:**

1. NVS init → load config (or apply defaults on first / corrupted boot).
2. Display init → show "Connecting…" splash screen.
3. WiFi STA connect → attempt with stored credentials.
4. SNTP sync → update system clock.
5. First data fetch → weather + crypto pull.
6. Display main UI.
7. HTTP server start.
8. Enter periodic loop.

**NVS Configuration Keys:**

| Key | Type | Default |
|-----|------|---------|
| `wifi_ssid` | string | — |
| `wifi_pass` | string (encrypted) | — |
| `weather_api_key` | string (encrypted) | — |
| `weather_city` | string | "London" (assumed) |
| `crypto_coins` | string | "bitcoin,ethereum,litecoin" |
| `crypto_interval_s` | uint32 | 60 |
| `weather_interval_s` | uint32 | 600 |
| `log_udp_host` | string | — |
| `log_udp_port` | uint16 | 5555 |
| `log_level` | uint8 | INFO |

**OTA model:** Dual-partition A/B (`factory` + `ota_0`) using `esp_ota_ops`.
Triggered via `POST /ota`. Boot-count watchdog calls
`esp_ota_mark_app_valid_cancel_rollback`; failure triggers automatic rollback.

---

## 3. Implementation Phases

### 3.1 Phase 1 — Circuit Design & Hardware Validation

**Scope:** Design and validate the ESP32-C3 + ILI9341 circuit. No production
firmware features.

**Deliverables:**
- Schematic with ESP32-C3, ILI9341 TFT, 3.3 V power rail, decoupling capacitors.
- Confirmed pin assignment table (see Section 2.2).
- Breadboard prototype with working SPI display.
- Proof-of-concept firmware: ILI9341 initialises, renders static text.

**Exit Criteria:**
- TFT displays static text over SPI without corruption or flicker.
- 3.3 V rail stable (< ±50 mV ripple under load).
- All GPIO lines confirmed against schematic.

**Dependencies:** None.

---

### 3.2 Phase 2 — Core Infrastructure

**Scope:** Full ESP32-C3 firmware: WiFi, SNTP, NVS, OTA, serial/UDP logging,
ILI9341 display driver, weather + crypto data fetching, time/date display.

**Deliverables:**
- WiFi STA connect / auto-reconnect with exponential back-off.
- SNTP time sync; time and date shown on TFT, updated every second.
- Weather API integration (temperature, condition label) for configured city.
- Crypto price API integration (BTC, ETH, LTC by default).
- Display layout rendering all data fields.
- OTA update flow (triggerable via serial command for Phase 2 testing).
- Serial + UDP structured logging.
- NVS config store with defaults and corruption recovery.

**Exit Criteria:**
- Device boots, connects to WiFi, and displays live time / weather / prices on TFT.
- Successful OTA update and rollback demonstrated (TC-OTA-100, TC-OTA-101).
- All LOG-001–026 requirements verified via serial monitor.
- NVS config persists across power cycle (TC-NVS-100).

**Dependencies:** Phase 1 complete (hardware validated and GPIO confirmed).

---

### 3.3 Phase 3 — HTTP Configuration Portal

**Scope:** In-browser configuration portal served from the ESP32 on the home LAN.

**Deliverables:**
- HTTP server on port 80 with responsive web UI (HTML/CSS, no external CDN).
- All configurable parameters exposed: WiFi credentials, API keys, city, coin list,
  refresh intervals, UDP log host/port, log level.
- `GET /status` JSON endpoint.
- `POST /config` endpoint persisting to NVS.
- `POST /ota` endpoint triggering OTA.
- `POST /reset` endpoint triggering factory reset.
- Graceful 400 responses on malformed requests.

**Exit Criteria:**
- All config fields readable and writable from a browser on the home LAN.
- Changed config persists after reboot (TC-NVS-100).
- OTA triggered via `POST /ota` completes successfully.
- Factory reset via `POST /reset` clears NVS and reboots to defaults.
- HTTP server responds within 2 s on home LAN (NFR-4.1).

**Dependencies:** Phase 2 complete.

---

### 3.4 Phase 4 (Future) — Calendar Integration

**Scope:** Fetch and display upcoming calendar events (e.g., Google Calendar iCal
feed). Out of scope for the initial release.

**Deliverables (TBD):** Calendar fetch task, display slot on TFT, calendar URL
and credentials configurable via portal.

---

## 4. Functional Requirements

### 4.1 Functional Requirements (FR)

**Display**

- **FR-1.1** [Must]: The device shall display current time (HH:MM:SS) and date
  (YYYY-MM-DD) on the TFT, updated every second.
- **FR-1.2** [Must]: The device shall display current weather information
  (temperature in °C, conditions text, daily low and high in °C) for a
  configured city.
- **FR-1.3** [Must]: The device shall display current prices for a configurable
  list of cryptocurrencies (default: Bitcoin, Ethereum, Litecoin).
- **FR-1.4** [Should]: The device shall indicate data staleness on the TFT
  when an API has not responded within 2× its configured refresh interval.
- **FR-1.5** [Should]: The device shall show a WiFi-disconnected indicator on
  the TFT when the network connection is lost.
- **FR-1.6** [Should]: The device shall display a trend arrow (↑ green, ↓ red)
  next to each cryptocurrency price, indicating whether the most recently fetched
  price is higher or lower than the previous fetched value. No arrow is shown on
  the first fetch (no prior reference).
- **FR-1.7** [May]: The device shall display upcoming calendar events
  (Phase 4 only).

**WiFi / Connectivity**

- **FR-2.1** [Must]: The device shall connect to a configured home WiFi network
  in STA mode.
- **FR-2.2** [Must]: WiFi credentials shall be stored encrypted in NVS.
- **FR-2.3** [Must]: The device shall automatically reconnect after WiFi
  disconnect using exponential back-off (max 60 s interval).
- **FR-2.4** [Must]: The device shall synchronise system time via SNTP
  after WiFi connects.
- **FR-2.5** [Should]: The device shall re-synchronise SNTP every 6 hours.

**Data Fetching**

- **FR-3.1** [Must]: The device shall fetch weather data from a configurable
  external HTTP API using a stored API key.
- **FR-3.2** [Must]: The device shall fetch cryptocurrency prices from a
  public HTTP API (no authentication required by default).
- **FR-3.3** [Must]: The list of cryptocurrencies fetched and displayed shall
  be user-configurable.
- **FR-3.4** [Should]: The device shall cache the last successfully fetched
  values and display them when the API is unreachable.
- **FR-3.5** [Should]: Weather and crypto refresh intervals shall be
  individually configurable via the portal.

**HTTP Configuration Server**

- **FR-4.1** [Must]: The device shall run an HTTP server accessible on the
  home network.
- **FR-4.2** [Must]: The server shall serve a browser-accessible
  configuration UI.
- **FR-4.3** [Must]: The configuration UI shall allow setting WiFi
  credentials, weather API key and city, crypto coin list, and refresh
  intervals.
- **FR-4.4** [Must]: Configuration changes shall be persisted to NVS and
  survive reboots.
- **FR-4.5** [Should]: The server shall expose `GET /status` returning
  firmware version, uptime, WiFi RSSI, IP, and last fetch timestamps as JSON.
- **FR-4.6** [Should]: The server shall expose `POST /ota` accepting a
  firmware URL and triggering an OTA update.
- **FR-4.7** [Should]: The server shall expose `POST /reset` triggering a
  factory reset.
- **FR-4.8** [May]: The configuration portal shall be password-protected.

**OTA Updates**

- **FR-5.1** [Must]: The device shall support OTA firmware updates via HTTP
  download.
- **FR-5.2** [Must]: The device shall use a dual-partition A/B scheme.
- **FR-5.3** [Must]: The device shall verify firmware integrity before
  applying.
- **FR-5.4** [Must]: The device shall automatically roll back to the previous
  firmware if the new firmware fails to boot.
- **FR-5.5** [Should]: The device shall report the current firmware version
  via `GET /status`.
- **FR-5.6** [Should]: The device shall log OTA progress (download %,
  verify, apply, reboot).

**Logging**

- **FR-6.1** [Must]: The device shall output structured log messages via
  USB serial.
- **FR-6.2** [Should]: The device shall forward log messages via UDP to a
  configurable host and port.
- **FR-6.3** [Must]: The device shall log the boot sequence, including
  firmware version.
- **FR-6.4** [Must]: The device shall log all WiFi state transitions and
  all OTA events.
- **FR-6.5** [Should]: The device shall support configurable log levels
  (ERROR, WARN, INFO, DEBUG).

**NVS / Persistence**

- **FR-7.1** [Must]: All configurable parameters shall be stored in NVS
  and persist across reboots and power cycles.
- **FR-7.2** [Must]: The device shall use hardcoded defaults when NVS is
  empty or corrupt.
- **FR-7.3** [Must]: The device shall support factory reset (erase all NVS
  config).
- **FR-7.4** [Must]: API keys and WiFi credentials shall never appear in
  log output in plaintext.

---

### 4.2 Non-Functional Requirements (NFR)

- **NFR-1.1** [Must]: Time display shall update within 1 second of
  real-world time.
- **NFR-1.2** [Should]: Crypto price refresh interval shall default to 60 s
  and be configurable in the range 10–3600 s.
- **NFR-1.3** [Should]: Weather refresh interval shall default to 600 s and
  be configurable in the range 60–3600 s.
- **NFR-2.1** [Must]: WiFi shall reconnect within 60 seconds of a disconnect
  event.
- **NFR-3.1** [Must]: API keys and WiFi credentials shall be stored encrypted
  and never logged in plaintext.
- **NFR-4.1** [Should]: HTTP config server shall respond within 2 seconds on
  the local network.
- **NFR-5.1** [Must]: OTA A/B scheme shall prevent the device from being
  bricked by a failed firmware update.
- **NFR-6.1** [Should]: Free heap shall remain above 20% of total heap during
  normal operation.
- **NFR-7.1** [Should]: Full-screen TFT SPI refresh shall complete within
  200 ms.

---

### 4.3 Constraints

- **C-1**: Firmware shall use ESP-IDF (not Arduino) and esptool for flashing.
- **C-2**: Target chip is ESP32-C3; no ESP32-S3 or Xtensa-specific APIs.
- **C-3**: Device operates on a private home network; public internet exposure
  is out of scope.
- **C-4**: Flash is 4 MB; partition table must accommodate bootloader, two app
  partitions (A/B), NVS, and phy_init.
- **C-5**: ESP-IDF version 6.x (master branch at `~/esp/esp-idf`).

---

## 5. Risks, Assumptions & Dependencies

| # | Item | Type | Likelihood | Impact | Mitigation |
|---|------|------|-----------|--------|------------|
| R-1 | OpenWeatherMap free tier: 60 calls/min rate limit | Risk | Low | Low | Default 10 min interval stays well within limits |
| R-2 | CoinGecko free API: ~10–30 calls/min limit | Risk | Medium | Medium | Default 60 s interval; cache last values on HTTP 429 |
| R-3 | ILI9341 SPI refresh competing with WiFi DMA on ESP32-C3 | Risk | Medium | Medium | Separate FreeRTOS tasks; yield during WiFi bursts |
| R-4 | 4 MB flash — dual OTA partitions may require tight size budget | Risk | Low | High | Profile app size early in Phase 2; strip unused components |
| R-5 | ESP32-C3 400 KB SRAM — display buffer + lwIP may be tight | Risk | Medium | High | Use tile/partial rendering; avoid full 320×240 framebuffer |
| A-1 | Device powered via USB 5 V mains supply (assumed) | Assumption | — | — | Low-power mode not required |
| A-2 | Home WiFi is WPA2/WPA3 on 2.4 GHz (assumed) | Assumption | — | — | |
| A-3 | Default weather city is "London" at build time (assumed) | Assumption | — | — | Configurable via portal |
| A-4 | SPI pin assignment per Section 2.2 (assumed, not yet PCB-confirmed) | Assumption | — | — | Verify against physical circuit in Phase 1 |
| D-1 | Requires a valid OpenWeatherMap API key (free tier) | External | — | — | User registers and enters key via portal |
| D-2 | CoinGecko API public availability | External | — | — | Cache last values on failure |
| D-3 | Home network DHCP assigns a stable IP | External | — | — | Document IP from router DHCP table; mDNS optional improvement |

---

## 6. Interface Specifications

### 6.1 External Interfaces

**WiFi (STA)**
- 802.11 b/g/n, 2.4 GHz, WPA2/WPA3.
- Credentials loaded from NVS.

**SNTP**
- Server: `pool.ntp.org` (configurable).
- Timezone: configurable via NVS (default UTC, assumed).

**Weather API (HTTP GET)**
- Provider: OpenWeatherMap (assumed) or compatible REST API.
- Endpoint: `https://api.openweathermap.org/data/2.5/weather?q={city}&appid={key}&units=metric`
- Extracts: `main.temp` (°C), `weather[0].description`.

**Crypto Price API (HTTP GET)**
- Provider: CoinGecko (free, no auth, assumed).
- Endpoint: `https://api.coingecko.com/api/v3/simple/price?ids={coin_ids}&vs_currencies=usd`
- Extracts: price per coin ID in USD.

### 6.2 HTTP Server Endpoints

| Method | Path | Description | Request Body | Response |
|--------|------|-------------|-------------|----------|
| GET | `/` | Config web UI | — | HTML page |
| GET | `/status` | Device status JSON | — | JSON (see below) |
| POST | `/config` | Apply configuration | JSON / form | `{"ok":true}` or error |
| POST | `/ota` | Trigger OTA update | `{"url":"..."}` | `{"ok":true}` |
| POST | `/reset` | Factory reset | — | — (device reboots) |

**`GET /status` response:**

```json
{
  "firmware_version": "1.0.0",
  "uptime_s": 3612,
  "wifi_rssi": -58,
  "ip": "192.168.1.42",
  "last_weather_fetch": "2026-06-08T12:00:00Z",
  "last_crypto_fetch": "2026-06-08T12:01:00Z",
  "coins": ["bitcoin", "ethereum", "litecoin"]
}
```

**`POST /config` body:**

```json
{
  "wifi_ssid": "MyNetwork",
  "wifi_pass": "...",
  "weather_api_key": "...",
  "weather_city": "London",
  "crypto_coins": "bitcoin,ethereum,litecoin",
  "crypto_interval_s": 60,
  "weather_interval_s": 600,
  "log_udp_host": "192.168.1.10",
  "log_udp_port": 5555
}
```

### 6.3 SPI Display Interface

| Signal | GPIO | ILI9341 Pin |
|--------|------|-------------|
| MOSI | 6 | SDI / MOSI |
| SCK | 4 | SCK |
| CS | 7 | CS |
| DC | 5 | D/C |
| RST | 3 | RESET |
| BL | 8 | LED (backlight) |

- SPI clock: 40 MHz (write), ILI9341 max 80 MHz.
- SPI Mode 0 (CPOL=0, CPHA=0).

### 6.4 Display Layout (assumed)

```
┌──────────────────────────────────────┐  ← 320 px
│  Mon, 08 Jun 2026        12:34:56    │  Time / Date bar
├──────────────────────────────────────┤
│  London   ⛅  22°C   Partly Cloudy   │  Weather row
├──────────────────────────────────────┤
│  BTC   $67,234    ▲ +2.1%            │
│  ETH    $3,412    ▼ -0.4%            │  Crypto rows
│  LTC      $82    ─  0.0%            │
├──────────────────────────────────────┤
│  Updated: 12:34:01                   │  Status / staleness bar
└──────────────────────────────────────┘  ← 240 px
```

---

## 7. Operational Procedures

### 7.1 Initial Flashing

```bash
RISCV_BIN=~/.espressif/tools/riscv32-esp-elf/esp-15.2.0_20251204/riscv32-esp-elf/bin
PATH="$RISCV_BIN:$PATH" \
IDF_PATH=~/esp/esp-idf \
IDF_PYTHON_ENV_PATH=~/.espressif/python_env/idf6.2_py3.14_env \
ESP_IDF_VERSION=6.2 \
~/.espressif/python_env/idf6.2_py3.14_env/bin/python3 \
  ~/esp/esp-idf/tools/idf.py -p /dev/ttyACM0 flash
```

WSL2 prerequisite: `usbipd bind --busid <id>` (admin PowerShell) then
`usbipd.exe attach --wsl --busid <id>`.

### 7.2 First-Time Configuration

1. Flash firmware; device boots and shows splash ("Connecting…" or "No WiFi").
2. Open a browser on the home network to `http://<device-ip>/`.
3. Enter WiFi credentials, OpenWeatherMap API key, city, coin list.
4. Submit — device saves to NVS and reboots.
5. After reboot, device connects to WiFi and displays live data.

### 7.3 OTA Firmware Update

```bash
# Host firmware.bin on any HTTP server, then:
curl -X POST http://<device-ip>/ota \
  -H 'Content-Type: application/json' \
  -d '{"url": "http://<server>/firmware.bin"}'
```

Device downloads, verifies, writes inactive partition, and reboots into new
firmware.

### 7.4 Factory Reset

```bash
curl -X POST http://<device-ip>/reset
```

Device erases NVS, reboots, and reverts to defaults.

### 7.5 Recovery (Unresponsive / No WiFi)

1. Connect USB serial; observe boot log for error codes.
2. If HTTP is reachable, use `POST /reset`.
3. Otherwise, reflash via USB using the flash command in Section 7.1.

---

## 8. Verification & Validation

### 8.1 Phase 1 Verification

| Test ID | Feature | Procedure | Success Criteria |
|---------|---------|-----------|-----------------|
| TC-HW-001 | Power supply | Measure 3.3 V rail under load | < ±50 mV ripple |
| TC-HW-002 | SPI connectivity | Logic analyser on SPI lines during init | Correct clock and data waveforms |
| TC-HW-003 | TFT solid fill | Fill screen red, green, blue | No stuck pixels, colours correct |
| TC-HW-004 | TFT text render | Render "Hello World" | Correct font, no corruption |
| TC-HW-005 | Pin assignment | Verify each GPIO against schematic | All signals on correct pins |

---

### 8.2 Phase 2 Verification

**WiFi Tests:**

| Test ID | Feature | Procedure | Success Criteria |
|---------|---------|-----------|-----------------|
| WIFI-001 | STA connect | Boot with stored credentials | Connects within 15 s |
| WIFI-003 | Auto-reconnect | Disable AP 30 s, re-enable | Reconnects automatically |
| EC-100 | Network disconnect | Disconnect during operation | Display continues, device reconnects |
| EC-110 | Weak signal | Move device to edge of WiFi range | No crash, graceful degradation |
| EC-115 | DHCP renewal | Set DHCP lease to 60 s | IP renewed, no disconnect |

**OTA Tests:**

| Test ID | Feature | Procedure | Success Criteria |
|---------|---------|-----------|-----------------|
| TC-OTA-100 | Successful OTA | Upload v2, trigger via serial cmd | v2 running after reboot |
| TC-OTA-101 | OTA rollback | Flash intentionally broken firmware | Auto-rollback to previous version |
| TC-OTA-102 | Version reporting | `GET /status` | Correct firmware version returned |
| EC-OTA-200 | Network loss mid-download | Cut WiFi at ~50% | No brick, previous firmware intact, retry succeeds |
| EC-OTA-202 | Invalid firmware rejection | Upload random binary | Rejected, device unaffected |

**Logging Tests:**

| Test ID | Feature | Procedure | Success Criteria |
|---------|---------|-----------|-----------------|
| TC-LOG-100 | Serial boot log | Monitor serial on boot | Version, WiFi connect, all events logged with timestamps |
| TC-LOG-101 | UDP log delivery | Configure UDP target, reboot | Boot messages received at target |
| TC-LOG-102 | Log level filtering | Set level to ERROR, trigger WARN | WARN not logged, ERROR logged |
| EC-LOG-200 | UDP target unreachable | Set non-existent UDP host | Device operates normally |

**NVS Tests:**

| Test ID | Feature | Procedure | Success Criteria |
|---------|---------|-----------|-----------------|
| TC-NVS-100 | Config persistence | Write config, reboot, read back | Values identical |
| TC-NVS-101 | First-boot defaults | Erase NVS, boot | Defaults applied, no crash |
| EC-NVS-200 | NVS corruption | Corrupt NVS partition, boot | Falls back to defaults, no crash |
| EC-NVS-203 | Credential security | Monitor all serial during boot | No plaintext WiFi password or API key |

**Display & Data Tests:**

| Test ID | Feature | Procedure | Success Criteria |
|---------|---------|-----------|-----------------|
| TC-DISP-001 | Time display | Boot, observe TFT | Correct time shown, increments every 1 s |
| TC-DISP-002 | SNTP sync | Boot after WiFi connect | Time updates from NTP within 10 s |
| TC-DISP-003 | Weather display | Configure valid API key, city | Temperature, condition, and Lo/Hi range shown on TFT |
| TC-DISP-004 | Crypto display | Default coin list | BTC, ETH, LTC prices shown |
| TC-DISP-005 | Staleness indicator | Block API for 2× refresh interval | Stale marker shown on TFT |
| TC-DISP-006 | WiFi disconnect indicator | Disconnect WiFi | Indicator shown on TFT |
| TC-DISP-007 | Crypto trend arrow | Wait for two fetch cycles; verify price goes up then down | ↑ green shown after price rise; ↓ red shown after price drop; no arrow on first fetch |
| TC-DATA-001 | Weather API fetch | Valid API key, known city | Correct temperature returned |
| TC-DATA-002 | Crypto API fetch | Default coins | Prices returned and displayed |
| TC-DATA-003 | Cache on API failure | Disable internet access | Last known values remain on display |
| TC-DATA-004 | Configurable coin list | Set coins to "bitcoin,dogecoin" | Only those coins fetched and displayed |

---

### 8.3 Phase 3 Verification

| Test ID | Feature | Procedure | Success Criteria |
|---------|---------|-----------|-----------------|
| TC-HTTP-001 | Portal accessible | Browse to `http://<ip>/` | Config page loads within 2 s |
| TC-HTTP-002 | Config save | Update weather city via portal | City updated on TFT, persists after reboot |
| TC-HTTP-003 | Status endpoint | `GET /status` | Valid JSON: version, uptime, RSSI |
| TC-HTTP-004 | OTA via portal | `POST /ota` with valid firmware URL | OTA completes, new version running |
| TC-HTTP-005 | Factory reset | `POST /reset` | NVS cleared, device reboots to defaults |
| TC-HTTP-006 | Malformed request | POST invalid JSON to `/config` | HTTP 400 response, device unaffected |
| TC-HTTP-007 | Config persists reboot | Update config via portal, reboot | Config values survive reboot |
| TC-NVS-102 | Factory reset clears all | `POST /reset`, then check NVS | All configurable keys erased, defaults used |

---

### 8.4 Traceability Matrix

| Requirement | Priority | Test Case(s) | Status |
|-------------|----------|-------------|--------|
| FR-1.1 (time/date display) | Must | TC-DISP-001, TC-DISP-002 | Covered |
| FR-1.2 (weather display) | Must | TC-DISP-003, TC-DATA-001 | Covered |
| FR-1.3 (crypto display) | Must | TC-DISP-004, TC-DATA-002 | Covered |
| FR-1.4 (staleness indicator) | Should | TC-DISP-005 | Covered |
| FR-1.5 (WiFi disconnect indicator) | Should | TC-DISP-006 | Covered |
| FR-1.6 (crypto trend arrow) | Should | TC-DISP-007 | Covered |
| FR-2.1 (WiFi STA connect) | Must | WIFI-001 | Covered |
| FR-2.2 (credentials encrypted in NVS) | Must | TC-NVS-100, EC-NVS-203 | Covered |
| FR-2.3 (auto-reconnect) | Must | WIFI-003, EC-100 | Covered |
| FR-2.4 (SNTP on connect) | Must | TC-DISP-002 | Covered |
| FR-2.5 (periodic SNTP resync) | Should | — | GAP |
| FR-3.1 (weather API fetch) | Must | TC-DATA-001 | Covered |
| FR-3.2 (crypto API fetch) | Must | TC-DATA-002 | Covered |
| FR-3.3 (configurable coin list) | Must | TC-DATA-004 | Covered |
| FR-3.4 (cache on failure) | Should | TC-DATA-003 | Covered |
| FR-3.5 (configurable intervals) | Should | TC-HTTP-002 | Covered |
| FR-4.1 (HTTP server runs) | Must | TC-HTTP-001 | Covered |
| FR-4.2 (config web UI) | Must | TC-HTTP-001 | Covered |
| FR-4.3 (all config fields in UI) | Must | TC-HTTP-002 | Covered |
| FR-4.4 (config persists) | Must | TC-HTTP-007, TC-NVS-100 | Covered |
| FR-4.5 (GET /status) | Should | TC-HTTP-003, TC-OTA-102 | Covered |
| FR-4.6 (POST /ota) | Should | TC-HTTP-004, TC-OTA-100 | Covered |
| FR-4.7 (POST /reset) | Should | TC-HTTP-005 | Covered |
| FR-5.1 (OTA HTTP update) | Must | TC-OTA-100 | Covered |
| FR-5.2 (A/B partition) | Must | TC-OTA-101, EC-OTA-200 | Covered |
| FR-5.3 (firmware integrity) | Must | EC-OTA-202 | Covered |
| FR-5.4 (OTA rollback) | Must | TC-OTA-101 | Covered |
| FR-5.5 (version via /status) | Should | TC-OTA-102 | Covered |
| FR-5.6 (OTA progress log) | Should | TC-LOG-100 | Covered |
| FR-6.1 (serial logging) | Must | TC-LOG-100 | Covered |
| FR-6.2 (UDP logging) | Should | TC-LOG-101, EC-LOG-200 | Covered |
| FR-6.3 (boot log with version) | Must | TC-LOG-100 | Covered |
| FR-6.4 (WiFi + OTA state logged) | Must | TC-LOG-100, TC-LOG-103 | Covered |
| FR-6.5 (configurable log level) | Should | TC-LOG-102 | Covered |
| FR-7.1 (NVS persistence) | Must | TC-NVS-100 | Covered |
| FR-7.2 (defaults on empty/corrupt NVS) | Must | TC-NVS-101, EC-NVS-200 | Covered |
| FR-7.3 (factory reset) | Must | TC-HTTP-005, TC-NVS-102 | Covered |
| FR-7.4 (no plaintext credentials in logs) | Must | EC-NVS-203 | Covered |
| NFR-1.1 (1 s time update) | Must | TC-DISP-001 | Covered |
| NFR-2.1 (60 s WiFi reconnect) | Must | WIFI-003 | Covered |
| NFR-3.1 (credential security) | Must | EC-NVS-203 | Covered |
| NFR-4.1 (HTTP < 2 s) | Should | TC-HTTP-001 | Covered |
| NFR-5.1 (no OTA brick) | Must | TC-OTA-101, EC-OTA-200 | Covered |
| NFR-7.1 (SPI refresh < 200 ms) | Should | TC-HW-003 | Covered |

---

## 9. Troubleshooting Guide

| Symptom | Likely Cause | Diagnostic Steps | Corrective Action |
|---------|-------------|-----------------|-------------------|
| TFT shows nothing | SPI wiring error or wrong CS/DC pin | Logic analyser on SPI; check GPIO assignment | Re-wire per Section 2.2 pinout |
| TFT shows garbled image | Wrong SPI mode or clock too high | Check `SPI_MODE` in driver config | Lower clock to 20 MHz; verify Mode 0 |
| WiFi won't connect | Wrong credentials or 5 GHz AP | Check serial for `wifi: reason=` code | Verify SSID/password; confirm 2.4 GHz band |
| Device IP not found | DHCP not assigned | Check serial for `IP: x.x.x.x` | Check router DHCP table; verify WiFi connect |
| Weather shows "N/A" | Invalid or missing API key | Check serial for HTTP 401 | Provide valid key via config portal |
| Crypto prices stale | CoinGecko rate-limiting | Check serial for HTTP 429 | Increase `crypto_interval_s` |
| OTA fails at download | Firmware URL unreachable | Check serial for OTA error code | Verify firmware URL reachable from device |
| OTA rollback triggered | New firmware crashes on boot | Check serial for `rst:0xc` | Fix firmware; reflash previous version |
| Portal unreachable | IP changed or HTTP server not started | Check serial for "HTTP server started" | Find new IP from router; check boot log |
| Free heap < 20% | Memory leak in fetch or render loop | Log `esp_get_free_heap_size()` at 1 h | Profile; reduce HTTP client buffer sizes |

---

## 10. Appendix

### 10.1 Partition Table (4 MB Flash)

```
# Name,       Type,  SubType,  Offset,    Size
nvs,          data,  nvs,      0x9000,    24K
phy_init,     data,  phy,      0xF000,    4K
factory,      app,   factory,  0x10000,   1M
ota_0,        app,   ota_0,    0x110000,  1M
ota_data,     data,  ota,      0x210000,  8K
```

### 10.2 Key Constants & Defaults

| Constant | Value | Notes |
|----------|-------|-------|
| SPI clock | 40 MHz | ILI9341 max write cycle |
| Display resolution | 320 × 240 px | Landscape orientation |
| Default crypto refresh | 60 s | Configurable 10–3600 s |
| Default weather refresh | 600 s | Configurable 60–3600 s |
| SNTP resync interval | 6 h | |
| WiFi max reconnect back-off | 60 s | Exponential from 1 s |
| HTTP server port | 80 | Configurable |
| UDP log default port | 5555 | |
| OTA minimum firmware size | 64 KB | Sanity check before write |

### 10.3 External API Reference

| Service | Base URL | Auth | Free Tier Limit |
|---------|----------|------|----------------|
| OpenWeatherMap | `https://api.openweathermap.org/data/2.5` | API key (query param) | 60 calls/min |
| CoinGecko | `https://api.coingecko.com/api/v3` | None | ~10–30 calls/min |
| NTP Pool | `pool.ntp.org` | None | UDP port 123 |

### 10.4 Build & Flash Quick Reference

```bash
export RISCV_BIN=~/.espressif/tools/riscv32-esp-elf/esp-15.2.0_20251204/riscv32-esp-elf/bin
export PATH="$RISCV_BIN:$PATH"
export IDF_PATH=~/esp/esp-idf
export IDF_PYTHON_ENV_PATH=~/.espressif/python_env/idf6.2_py3.14_env
export ESP_IDF_VERSION=6.2
alias idf="~/.espressif/python_env/idf6.2_py3.14_env/bin/python3 ~/esp/esp-idf/tools/idf.py"

cd ~/esp32DisplayProject
idf build
idf -p /dev/ttyACM0 flash monitor
```

---

## 11. Related

- `[[hello_world]]` — initial proof-of-concept firmware in this repository
- `[[esp32DisplayProject-idea]]` — original project idea notes

---

*Generated by fsd-writer skill — 2026-06-08*
