# ESP32 InfoDisplay — Hardware Design

## Bill of Materials

| # | Component | Part / Notes |
|---|-----------|-------------|
| 1 | MCU | ESP32-C3 dev board (e.g. ESP32-C3-DevKitM-1) |
| 2 | Display | GMT028 TFT 2.8" — ILI9341 controller, 320×240, RGB565, SPI |
| 3 | Cap | 10 µF electrolytic — 3.3 V rail decoupling |
| 4 | Cap | 100 nF ceramic × 2 — close to ESP32-C3 VDD and display VCC pins |
| 5 | Resistor | 10 kΩ pull-up on RST line (optional, board-dependent) |
| 6 | USB cable | USB-A to USB-C (dev board power + serial/JTAG) |

---

## SPI Pin Assignment

| ESP32-C3 GPIO | ILI9341 Pin | Signal | Notes |
|---------------|-------------|--------|-------|
| GPIO 6 | MOSI / SDI | SPI data out | |
| GPIO 4 | SCK | SPI clock | 40 MHz |
| GPIO 7 | CS | Chip select | Active LOW |
| GPIO 5 | DC / RS | Data / Command | HIGH = data, LOW = command |
| GPIO 3 | RESET | Hardware reset | Active LOW pulse on init |
| GPIO 8 | LED / BL | Backlight | HIGH = on |
| 3V3 | VCC | Power | 3.3 V |
| GND | GND | Ground | |

> **Note**: MISO is not connected — ILI9341 is write-only in this configuration.

---

## Breadboard Wiring Diagram

```
ESP32-C3 DevKit              GMT028 ILI9341 Module
┌──────────────┐             ┌─────────────────────┐
│          3V3 ├─────────────┤ VCC                  │
│          GND ├─────────────┤ GND                  │
│       GPIO 6 ├─────────────┤ MOSI / SDI           │
│       GPIO 4 ├─────────────┤ SCK                  │
│       GPIO 7 ├─────────────┤ CS                   │
│       GPIO 5 ├─────────────┤ DC / RS              │
│       GPIO 3 ├─────────────┤ RESET                │
│       GPIO 8 ├─────────────┤ LED / BL             │
└──────────────┘             └─────────────────────┘

Decoupling:
  10µF between 3V3 and GND (near display VCC)
 100nF between 3V3 and GND (near ESP32-C3 VDD pin)
 100nF between 3V3 and GND (near display VCC pin)
```

---

## Power Budget

| Component | Current (typical) |
|-----------|-------------------|
| ESP32-C3 (WiFi active) | ~80 mA |
| ILI9341 display logic | ~20 mA |
| ILI9341 backlight (full) | ~80–120 mA |
| **Total** | **~200 mA** |

USB 5V supply → onboard LDO → 3.3V. Ensure the dev board LDO is rated ≥ 500 mA.
Use a quality USB cable; thin cables cause brown-outs at peak WiFi transmit.

---

## SPI Timing Notes

- **Clock**: 40 MHz during normal operation. ILI9341 write cycle min is 66 ns
  (≈ 15 MHz), but most modules handle 40 MHz without issues. If display shows
  corruption, lower to 20 MHz in `display.c` (`LCD_FREQ_HZ`).
- **Mode**: SPI Mode 0 (CPOL=0, CPHA=0).
- **Byte order**: ILI9341 expects RGB565 big-endian (MSB first over SPI).
  Software byte-swaps before sending.

---

## Phase 1 Exit Criteria (TC-HW-001 to TC-HW-005)

| Test | Pass Condition |
|------|---------------|
| TC-HW-001 | 3.3 V rail measures 3.30 ± 0.05 V under load |
| TC-HW-002 | SPI clock and MOSI signals visible on logic analyser during init |
| TC-HW-003 | TFT fills solid red, then green, then blue without stuck pixels |
| TC-HW-004 | "Hello InfoDisplay" text renders without corruption |
| TC-HW-005 | All GPIO lines probed and match this pinout table |
