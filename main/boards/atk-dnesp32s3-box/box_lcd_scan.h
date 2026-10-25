#pragma once

#include <array>
#include <cstdint>
#include <esp_lcd_panel_io.h>

namespace box_lcd {

struct ScanSample {
    uint32_t us = 0;
    uint32_t line = 0;
};

struct ScanProbeResult {
    bool attempted = false;
    bool registers_match = false;
    bool scan_valid = false;
    bool pins_restored = false;
    uint32_t madctl = 0;
    uint32_t colmod = 0;
    uint32_t sample_count = 0;
    uint32_t transitions = 0;
    uint32_t wraps = 0;
    uint32_t min_line = 0;
    uint32_t max_line = 0;
    uint32_t frame_period_us = 0;
    std::array<ScanSample, 40> samples{};
};

struct ScanGateResult {
    bool ready = false;
    bool pins_restored = false;
    uint32_t line = 0;
    uint32_t wait_us = 0;
    // Conservative timestamp before starting the qualifying read. Includes read
    // preemptions in the caller's freshness budget, before GPIO route restore.
    uint32_t sampled_us = 0;
};

// Boot-time diagnostic only: caller owns the idle LCD bus before LVGL starts.
// Does not enable synchronization or alter panel registers/scanning direction.
void ProbeScanReadback(esp_lcd_panel_io_handle_t panel_io);
// The boot-time result is immutable before the HTTP server is started.
ScanProbeResult GetScanProbeResult();

// Caller owns the LVGL lock and the idle LCD bus. Waits for a measured scanline
// window without changing panel registers/direction. Pixel-write timing and
// native-order packing remain the caller's responsibility.
ScanGateResult WaitForScanWindow(esp_lcd_panel_io_handle_t panel_io,
                                uint32_t first_line = 64, uint32_t last_line = 80,
                                uint32_t timeout_us = 40000);

}  // namespace box_lcd
