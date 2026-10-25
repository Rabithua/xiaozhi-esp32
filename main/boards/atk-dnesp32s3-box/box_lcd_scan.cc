#include "box_lcd_scan.h"
#include "config.h"

#include <algorithm>
#include <array>
#include <limits>
#include <driver/gpio.h>
#include <esp_lcd_panel_commands.h>
#include <esp_rom_sys.h>
#include <esp_timer.h>
#include <soc/gpio_periph.h>
#include <soc/gpio_struct.h>
#include <soc/soc.h>

namespace box_lcd {
namespace {

constexpr std::array<gpio_num_t, 8> kDataPins = {
    GPIO_LCD_D0, GPIO_LCD_D1, GPIO_LCD_D2, GPIO_LCD_D3,
    GPIO_LCD_D4, GPIO_LCD_D5, GPIO_LCD_D6, GPIO_LCD_D7,
};
// CS is last so restoring any other output cannot select the LCD prematurely.
constexpr std::array<gpio_num_t, 12> kPins = {
    GPIO_LCD_D0, GPIO_LCD_D1, GPIO_LCD_D2, GPIO_LCD_D3,
    GPIO_LCD_D4, GPIO_LCD_D5, GPIO_LCD_D6, GPIO_LCD_D7,
    LCD_NUM_RD, LCD_NUM_WR, LCD_NUM_DC, LCD_NUM_CS,
};
constexpr uint32_t kExpectedMadctl = 0x60;  // Existing MX + MV configuration.
// RDDCOLMOD D2:0 describe the control/DBI interface used by this I80 bus.
// D6:4 describe the separate RGB/DPI interface. On this actual BOX the raw
// readback is 0x05 despite initialization writing 0x65; preserve that evidence
// without treating irrelevant DPI bits as failure of the read-only scan probe.
constexpr uint32_t kExpectedDbiFormat = 0x05;
constexpr int64_t kProbeBudgetUs = 100000;
constexpr uint32_t kSampleIntervalUs = 1500;
// GSCAN includes VSYNC and both porches, not just the 320 active lines.
// Normal-mode porch fields are seven bits each; leave ample blanking margin.
// Keep every raw sample even when this conservative diagnostic guard fails.
constexpr uint32_t kMaxProbeLine = 1023;
ScanProbeResult g_result;

struct PinState {
    uint32_t iomux = 0;
    uint32_t matrix = 0;
    uint32_t pin = 0;
    bool output_enabled = false;
    bool latch = false;
};

PinState ReadPin(gpio_num_t pin) {
    const uint32_t bit = 1U << (static_cast<uint32_t>(pin) & 31U);
    return {
        REG_READ(GPIO_PIN_MUX_REG[pin]),
        GPIO.func_out_sel_cfg[pin].val,
        GPIO.pin[pin].val,
        ((pin < 32 ? GPIO.enable : GPIO.enable1.val) & bit) != 0,
        ((pin < 32 ? GPIO.out : GPIO.out1.val) & bit) != 0,
    };
}

bool SamePin(const PinState& a, const PinState& b) {
    return a.iomux == b.iomux && a.matrix == b.matrix && a.pin == b.pin &&
           a.output_enabled == b.output_enabled && a.latch == b.latch;
}

void WriteLatch(gpio_num_t pin, bool high) {
    const uint32_t bit = 1U << (static_cast<uint32_t>(pin) & 31U);
    if (pin < 32) {
        if (high) GPIO.out_w1ts = bit;
        else GPIO.out_w1tc = bit;
    } else {
        if (high) GPIO.out1_w1ts.val = bit;
        else GPIO.out1_w1tc.val = bit;
    }
}

void WriteOutputEnable(gpio_num_t pin, bool enabled) {
    const uint32_t bit = 1U << (static_cast<uint32_t>(pin) & 31U);
    if (pin < 32) {
        if (enabled) GPIO.enable_w1ts = bit;
        else GPIO.enable_w1tc = bit;
    } else {
        if (enabled) GPIO.enable1_w1ts.val = bit;
        else GPIO.enable1_w1tc.val = bit;
    }
}

// ESP-IDF's public direction setter intentionally removes peripheral output
// routing. Save the complete output matrix word, IOMUX, GPIO pin configuration,
// output-enable bit and GPIO latch rather than trying to reconstruct the route.
// This is specific to the pinned ESP32-S3 / IDF v5.5 target.
class PinTakeover {
public:
    PinTakeover() {
        for (size_t i = 0; i < kPins.size(); ++i) saved_[i] = ReadPin(kPins[i]);
    }
    ~PinTakeover() { Restore(); }

    bool Begin() {
        changed_ = true;
        // Disconnect CS first, with its software latch already inactive. The
        // board config uses active-low CS and idle-high WR/RD.
        for (const auto pin : {LCD_NUM_CS, LCD_NUM_RD, LCD_NUM_WR, LCD_NUM_DC}) {
            WriteLatch(pin, true);
            if (gpio_set_direction(pin, GPIO_MODE_OUTPUT) != ESP_OK) return false;
        }
        for (const auto pin : kDataPins) {
            if (gpio_set_direction(pin, GPIO_MODE_INPUT) != ESP_OK) return false;
        }
        esp_rom_delay_us(1);
        return true;
    }

    bool Read(uint8_t command, uint8_t* output, size_t count) {
        WriteLatch(LCD_NUM_RD, true);
        WriteLatch(LCD_NUM_WR, true);
        WriteLatch(LCD_NUM_CS, true);
        for (size_t i = 0; i < kDataPins.size(); ++i) {
            WriteLatch(kDataPins[i], (command >> i) & 1U);
            if (gpio_set_direction(kDataPins[i], GPIO_MODE_OUTPUT) != ESP_OK) return false;
        }
        WriteLatch(LCD_NUM_DC, false);
        WriteLatch(LCD_NUM_CS, false);
        esp_rom_delay_us(1);
        WriteLatch(LCD_NUM_WR, false);
        esp_rom_delay_us(1);
        WriteLatch(LCD_NUM_WR, true);
        esp_rom_delay_us(1);
        WriteLatch(LCD_NUM_DC, true);
        // Release all eight ESP outputs before asserting RD. The LCD may drive
        // them throughout the read pulse, including the dummy read.
        for (const auto pin : kDataPins) {
            if (gpio_set_direction(pin, GPIO_MODE_INPUT) != ESP_OK) return false;
        }
        esp_rom_delay_us(1);
        ReadByte();  // ST7789 register reads begin with one dummy byte.
        for (size_t i = 0; i < count; ++i) output[i] = ReadByte();
        WriteLatch(LCD_NUM_RD, true);
        WriteLatch(LCD_NUM_CS, true);
        esp_rom_delay_us(1);  // >80ns LCD output-disable time before next write.
        return true;
    }

    bool Restore() {
        if (restored_) return matches_;
        if (changed_) {
            WriteLatch(LCD_NUM_RD, true);
            WriteLatch(LCD_NUM_CS, true);
            // Release ESP data outputs too, then allow the LCD to go high-Z.
            for (const auto pin : kDataPins) gpio_set_direction(pin, GPIO_MODE_INPUT);
            esp_rom_delay_us(1);
            for (size_t i = 0; i < kPins.size(); ++i) {
                const auto pin = kPins[i];
                const auto& state = saved_[i];
                // CS's saved GPIO latch may be low even though its peripheral
                // route is idle high. Keep GPIO CS inactive until that route is
                // back, then restore the now-disconnected software latch.
                if (pin != LCD_NUM_CS) WriteLatch(pin, state.latch);
                GPIO.pin[pin].val = state.pin;
                REG_WRITE(GPIO_PIN_MUX_REG[pin], state.iomux);
                WriteOutputEnable(pin, state.output_enabled);
                // Restore after direction/enable handling: public output setup
                // would otherwise silently replace this peripheral route.
                GPIO.func_out_sel_cfg[pin].val = state.matrix;
                if (pin == LCD_NUM_CS) WriteLatch(pin, state.latch);
            }
        }
        matches_ = true;
        for (size_t i = 0; i < kPins.size(); ++i) {
            matches_ = SamePin(saved_[i], ReadPin(kPins[i])) && matches_;
        }
        restored_ = true;
        return matches_;
    }

private:
    uint8_t ReadByte() {
        WriteLatch(LCD_NUM_RD, false);
        esp_rom_delay_us(1);
        uint8_t value = 0;
        for (size_t i = 0; i < kDataPins.size(); ++i) {
            if (gpio_get_level(kDataPins[i])) value |= static_cast<uint8_t>(1U << i);
        }
        WriteLatch(LCD_NUM_RD, true);
        esp_rom_delay_us(1);
        return value;
    }

    std::array<PinState, kPins.size()> saved_{};
    bool changed_ = false;
    bool restored_ = false;
    bool matches_ = false;
};

}  // namespace

void ProbeScanReadback(esp_lcd_panel_io_handle_t panel_io) {
    if (g_result.attempted) return;
    ScanProbeResult result;
    result.attempted = true;
    PinTakeover pins;
    // Called before LVGL exists. NOP drains any preceding parameter/color
    // traffic while the established hardware routes are still intact.
    if (!panel_io || esp_lcd_panel_io_tx_param(panel_io, LCD_CMD_NOP, nullptr, 0) != ESP_OK || !pins.Begin()) {
        result.pins_restored = pins.Restore();
        g_result = result;
        return;
    }
    const int64_t started_us = esp_timer_get_time();
    uint8_t value = 0;
    bool read_ok = pins.Read(LCD_CMD_RDD_MADCTL, &value, 1);
    result.madctl = value;
    value = 0;
    read_ok = pins.Read(LCD_CMD_RDD_COLMOD, &value, 1) && read_ok;
    result.colmod = value;
    result.registers_match = read_ok && result.madctl == kExpectedMadctl &&
        (result.colmod & 0x07U) == kExpectedDbiFormat;
    if (result.registers_match) {
        result.min_line = std::numeric_limits<uint32_t>::max();
        bool in_range = true;
        uint32_t first_wrap_us = 0;
        uint32_t last_wrap_us = 0;
        for (size_t i = 0; i < result.samples.size(); ++i) {
            if (esp_timer_get_time() - started_us >= kProbeBudgetUs) break;
            uint8_t bytes[2] = {};
            if (!pins.Read(LCD_CMD_GDCAN, bytes, sizeof(bytes))) break;
            const uint32_t line = (static_cast<uint32_t>(bytes[0]) << 8U) | bytes[1];
            const uint32_t elapsed = static_cast<uint32_t>(esp_timer_get_time() - started_us);
            result.samples[i] = {elapsed, line};
            ++result.sample_count;
            in_range = in_range && line <= kMaxProbeLine;
            result.min_line = std::min(result.min_line, line);
            result.max_line = std::max(result.max_line, line);
            if (i) {
                const auto previous = result.samples[i - 1].line;
                if (line != previous) ++result.transitions;
                if (previous <= kMaxProbeLine && line <= kMaxProbeLine && previous > line + 64) {
                    ++result.wraps;
                    if (!first_wrap_us) first_wrap_us = elapsed;
                    last_wrap_us = elapsed;
                }
            }
            if (i + 1 < result.samples.size()) esp_rom_delay_us(kSampleIntervalUs);
        }
        if (result.wraps >= 2) result.frame_period_us = (last_wrap_us - first_wrap_us) / (result.wraps - 1);
        // Conservative evidence of a live native 320-line scan, not a stuck
        // floating bus. This is a diagnostic classification, not sync enablement.
        result.scan_valid = in_range && result.sample_count == result.samples.size() &&
            result.transitions >= 12 && result.wraps >= 2 && result.min_line <= 80 &&
            result.max_line >= 256 && result.frame_period_us >= 8000 && result.frame_period_us <= 40000;
        if (!result.sample_count) result.min_line = 0;
    }
    result.pins_restored = pins.Restore();
    result.scan_valid = result.scan_valid && result.pins_restored;
    g_result = result;
}

ScanProbeResult GetScanProbeResult() { return g_result; }

ScanGateResult WaitForScanWindow(esp_lcd_panel_io_handle_t panel_io,
                                uint32_t first_line, uint32_t last_line,
                                uint32_t timeout_us) {
    ScanGateResult result;
    if (!panel_io || !g_result.registers_match || !g_result.scan_valid ||
        !g_result.pins_restored || first_line > last_line ||
        last_line > kMaxProbeLine || !timeout_us) {
        return result;
    }
    const int64_t started_us = esp_timer_get_time();
    // The caller's single-buffer fence has already completed the previous
    // pixel upload. Drain once more before taking any GPIO routing snapshots.
    if (esp_lcd_panel_io_tx_param(panel_io, LCD_CMD_NOP, nullptr, 0) != ESP_OK) {
        result.wait_us = static_cast<uint32_t>(esp_timer_get_time() - started_us);
        return result;
    }
    PinTakeover pins;
    bool matched = false;
    if (esp_timer_get_time() - started_us < timeout_us && pins.Begin()) {
        while (esp_timer_get_time() - started_us < timeout_us) {
            uint8_t bytes[2] = {};
            const int64_t read_started_us = esp_timer_get_time();
            if (!pins.Read(LCD_CMD_GDCAN, bytes, sizeof(bytes))) break;
            const int64_t sampled_us = esp_timer_get_time();
            result.line = (static_cast<uint32_t>(bytes[0]) << 8U) | bytes[1];
            if (sampled_us - started_us >= timeout_us || result.line > kMaxProbeLine) break;
            if (result.line >= first_line && result.line <= last_line) {
                // The scanline may latch before Read() returns. Include the
                // whole read in the caller's freshness budget so a preemption
                // between response bytes cannot make an old phase look new.
                result.sampled_us = static_cast<uint32_t>(read_started_us);
                matched = true;
                break;
            }
            // No critical section: audio/network interrupts remain available.
            esp_rom_delay_us(20);
        }
    }
    result.pins_restored = pins.Restore();
    result.wait_us = static_cast<uint32_t>(esp_timer_get_time() - started_us);
    result.ready = matched && result.pins_restored;
    return result;
}

}  // namespace box_lcd
