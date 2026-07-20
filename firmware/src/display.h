#pragma once

// Display backend selector.
//
// Four backends share the same LovyanGFX/TFT_eSPI-style drawing API, so
// ui.cpp talks to `DisplayTFT` / `DisplaySprite` and stays backend-agnostic:
//
//   USE_LOVYANGFX_DSI  Waveshare ESP32-P4 (e.g. P4-NANO) + 8.8" 480x1920
//                      MIPI-DSI panel, rotated to landscape 1920x480
//                      (LovyanGFX Bus_DSI/Panel_DSI). Defines LAYOUT_WIDE.
//   LGFX_SDL           desktop emulator (LovyanGFX SDL backend) at 1920x480,
//                      see emulator/. Defines LAYOUT_WIDE.
//   USE_LOVYANGFX      Waveshare ESP32-C6-LCD-1.47, 172x320 ST7789 over SPI
//                      (TFT_eSPI has no working C6 driver).
//   (none)             TFT_eSPI: Sunton ESP32-3248S035 and bare ILI9488,
//                      480x320 landscape.
//
// DISPLAY_IS_LGFX marks the LovyanGFX-API backends (they differ from
// TFT_eSPI in a few calls: drawWideLine has no bg-color arg, drawJpg exists).

#if defined(USE_LOVYANGFX_DSI)

// ---- Waveshare ESP32-P4 + 8.8inch DSI LCD (480x1920, 2-lane MIPI-DSI) ----
//
// The panel is from Waveshare's Raspberry Pi DSI line: it self-initializes
// (no vendor DCS init sequence), so it only needs correct DPI video timings
// plus a couple of I2C writes to the display's onboard controller (addr 0x45)
// for panel power and backlight. Timings come from the Raspberry Pi kernel
// driver (panel-waveshare-dsi.c, ws_panel_8_8_mode): 83.333 MHz pixel clock,
// h 480+50+50+50, v 1920+20+20+20.
//
// Untested-on-hardware caveats, in case the screen comes up blank/odd:
//  - LovyanGFX Panel_DSI drives the link in RGB565. If colours are wrong or
//    the panel stays dark, it may want RGB888 like the Pi uses.
//  - lane_mbps 1000 has ~50% headroom over the required ~667 Mbps.

#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <lgfx/v1/platforms/esp32p4/Panel_DSI.hpp>  // not pulled in by LovyanGFX.hpp
#include <Wire.h>

class LGFX : public lgfx::LGFX_Device {
    lgfx::Bus_DSI   _bus;
    lgfx::Panel_DSI _panel;

public:
    LGFX() {
        {
            auto cfg = _bus.config();
            cfg.lane_mbps      = 1000;
            cfg.lane_num       = 2;
            cfg.ldo_voltage_mv = 2500;  // VDD_MIPI_DPHY on LDO channel 3
            cfg.ldo_chan_id    = 3;
            _bus.config(cfg);
            _panel.setBus(&_bus);
        }
        {
            auto cfg = _panel.config();
            cfg.memory_width  = 480;
            cfg.memory_height = 1920;
            cfg.panel_width   = 480;
            cfg.panel_height  = 1920;
            cfg.offset_x      = 0;
            cfg.offset_y      = 0;
            _panel.config(cfg);
        }
        {
            auto cfg = _panel.config_detail();
            cfg.dpi_freq_mhz       = 83;
            cfg.hsync_front_porch  = 50;
            cfg.hsync_pulse_width  = 50;
            cfg.hsync_back_porch   = 50;
            cfg.vsync_front_porch  = 20;
            cfg.vsync_pulse_width  = 20;
            cfg.vsync_back_porch   = 20;
            _panel.config_detail(cfg);
        }
        setPanel(&_panel);
    }
};

typedef LGFX                DisplayTFT;
typedef lgfx::LGFX_Sprite   DisplaySprite;

// Native portrait 480x1920, rotated to landscape. Use 3 if mounted flipped.
static const int SCREEN_ROTATION = 1;
static const int SCREEN_W = 1920;
static const int SCREEN_H = 480;

#define LAYOUT_WIDE 1
#define DISPLAY_IS_LGFX 1

// The display's onboard controller (I2C addr 0x45, on the DSI connector's
// I2C: SDA 7 / SCL 8 on the P4-NANO) handles panel power and backlight.
// Same register protocol as the Raspberry Pi kernel driver.
static const uint8_t WS_PANEL_I2C_ADDR = 0x45;
static const int     WS_PANEL_I2C_SDA  = 7;
static const int     WS_PANEL_I2C_SCL  = 8;

inline void wsPanelWrite(uint8_t reg, uint8_t val) {
    Wire.beginTransmission(WS_PANEL_I2C_ADDR);
    Wire.write(reg);
    Wire.write(val);
    Wire.endTransmission();
}

inline void displaySetBacklight(uint8_t brightness) {  // 0..255
    wsPanelWrite(0xAB, 0xFF - brightness);
    wsPanelWrite(0xAA, 0x01);
}

// Call before tft.init(): powers the panel so it can train the DSI link.
inline void displayBoardInit() {
    Wire.begin(WS_PANEL_I2C_SDA, WS_PANEL_I2C_SCL, 100000);
    wsPanelWrite(0xC0, 0x01);
    wsPanelWrite(0xC2, 0x01);
    wsPanelWrite(0xAC, 0x01);
    delay(20);
    displaySetBacklight(255);
}

#elif defined(LGFX_SDL)

// ---- desktop emulator: LovyanGFX SDL backend, one window per display ----

#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <LGFX_AUTODETECT.hpp>  // provides lgfx::LGFX(width, height) on SDL

static const int SCREEN_ROTATION = 0;
static const int SCREEN_W = 1920;
static const int SCREEN_H = 480;

class EmuLGFX : public lgfx::LGFX {
public:
    EmuLGFX() : lgfx::LGFX(SCREEN_W, SCREEN_H) {}
};

typedef EmuLGFX             DisplayTFT;
typedef lgfx::LGFX_Sprite   DisplaySprite;

#define LAYOUT_WIDE 1
#define DISPLAY_IS_LGFX 1

inline void displayBoardInit() {}

#elif defined(USE_LOVYANGFX)

#include <LovyanGFX.hpp>

// Waveshare ESP32-C6-LCD-1.47 — 172x320 ST7789 panel on SPI2 with PWM backlight.
class LGFX : public lgfx::LGFX_Device {
    lgfx::Panel_ST7789 _panel;
    lgfx::Bus_SPI      _bus;
    lgfx::Light_PWM    _light;

public:
    LGFX() {
        {
            auto cfg = _bus.config();
            cfg.spi_host    = SPI2_HOST;
            cfg.spi_mode    = 0;
            cfg.freq_write  = 40000000;
            cfg.freq_read   = 16000000;
            cfg.spi_3wire   = true;
            cfg.use_lock    = true;
            cfg.dma_channel = SPI_DMA_CH_AUTO;
            cfg.pin_sclk    = 7;
            cfg.pin_mosi    = 6;
            cfg.pin_miso    = -1;
            cfg.pin_dc      = 15;
            _bus.config(cfg);
            _panel.setBus(&_bus);
        }
        {
            auto cfg = _panel.config();
            cfg.pin_cs          = 14;
            cfg.pin_rst         = 21;
            cfg.pin_busy        = -1;
            cfg.memory_width    = 240;   // ST7789 GRAM is 240 wide; panel is 172
            cfg.memory_height   = 320;
            cfg.panel_width     = 172;
            cfg.panel_height    = 320;
            cfg.offset_x        = 34;    // (240 - 172) / 2, centers the 172px window
            cfg.offset_y        = 0;
            cfg.offset_rotation = 0;
            cfg.readable        = false;
            cfg.invert          = true;  // ST7789 needs colour inversion on
            cfg.rgb_order       = false; // Waveshare panel is BGR
            cfg.dlen_16bit      = false;
            cfg.bus_shared      = false;
            _panel.config(cfg);
        }
        {
            auto cfg = _light.config();
            cfg.pin_bl      = 22;
            cfg.invert      = false;
            cfg.freq        = 12000;
            cfg.pwm_channel = 0;
            _light.config(cfg);
            _panel.setLight(&_light);
        }
        setPanel(&_panel);
    }
};

typedef LGFX                DisplayTFT;
typedef lgfx::LGFX_Sprite   DisplaySprite;

// Portrait 172x320 panel.
static const int SCREEN_ROTATION = 0;
static const int SCREEN_W = 172;
static const int SCREEN_H = 320;

#define DISPLAY_IS_LGFX 1

inline void displayBoardInit() {}

#else  // ---- TFT_eSPI: Sunton ESP32-3248S035 and generic ESP32 + ILI9488 ----

#include <SPI.h>
#include <TFT_eSPI.h>

typedef TFT_eSPI    DisplayTFT;
typedef TFT_eSprite DisplaySprite;

// 480x320 panels driven in landscape. The UI is laid out for the 172x320
// portrait panel above, so it renders the same stacked layout here, just wider.
static const int SCREEN_ROTATION = 1;
static const int SCREEN_W = 480;
static const int SCREEN_H = 320;

#define DISPLAY_IS_LGFX 0

inline void displayBoardInit() {}

#endif

#ifndef LAYOUT_WIDE
#define LAYOUT_WIDE 0
#endif
