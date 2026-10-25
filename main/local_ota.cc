#include "local_ota.h"

#include "application.h"
#include "board.h"
#include "display/display.h"
#if defined(CONFIG_BOARD_TYPE_ATK_DNESP32S3_BOX)
#include "boards/atk-dnesp32s3-box/box_lcd_scan.h"
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <string>

#include <cJSON.h>
#include <esp_app_desc.h>
#include <esp_app_format.h>
#include <esp_http_server.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_ota_ops.h>
#include <esp_random.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <mbedtls/sha256.h>
#include <nvs.h>

namespace {

constexpr const char* kTag = "LocalOta";
constexpr const char* kBoard = "atk-dnesp32s3-box";
constexpr const char* kProject = "xiaozhi";
// Also present in every eligible image. This catches accidental uploads of a
// different board's app; bearer authentication remains a separate requirement.
__attribute__((used)) constexpr char kBoardMarker[] =
    "xiaozhi-local-ota-board:atk-dnesp32s3-box:v1";
constexpr size_t kMarkerBytes = sizeof(kBoardMarker) - 1;
constexpr size_t kPrefixBytes = sizeof(esp_image_header_t) +
    sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t);
constexpr size_t kBufferBytes = 4096;
constexpr int64_t kStallUs = 30LL * 1000 * 1000;
constexpr int64_t kUploadUs = 180LL * 1000 * 1000;

struct ServerContext {
    httpd_handle_t server = nullptr;
    esp_timer_handle_t reboot_timer = nullptr;
    std::array<char, 65> token{};
    std::array<char, 17> device_id{};
    std::atomic<bool> uploading{false};
};

ServerContext& Context() {
    static ServerContext context;
    return context;
}

bool IsHex(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
           (c >= 'A' && c <= 'F');
}

void ToHex(const uint8_t* data, size_t size, char* output) {
    constexpr char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < size; ++i) {
        output[i * 2] = digits[data[i] >> 4];
        output[i * 2 + 1] = digits[data[i] & 15];
    }
    output[size * 2] = '\0';
}

bool ReadDigest(const char* text, std::array<uint8_t, 32>& output) {
    auto nibble = [](char c) -> unsigned {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        return c - 'A' + 10;
    };
    if (std::strlen(text) != 64) return false;
    for (size_t i = 0; i < output.size(); ++i) {
        if (!IsHex(text[i * 2]) || !IsHex(text[i * 2 + 1])) return false;
        output[i] = static_cast<uint8_t>((nibble(text[i * 2]) << 4) | nibble(text[i * 2 + 1]));
    }
    return true;
}

bool EqualBytes(const void* a, const void* b, size_t size) {
    const auto* x = static_cast<const uint8_t*>(a);
    const auto* y = static_cast<const uint8_t*>(b);
    uint8_t difference = 0;
    for (size_t i = 0; i < size; ++i) difference |= x[i] ^ y[i];
    return difference == 0;
}

class Digest {
public:
    Digest() { mbedtls_sha256_init(&context_); }
    ~Digest() { mbedtls_sha256_free(&context_); }
    bool Start() { return mbedtls_sha256_starts(&context_, 0) == 0; }
    bool Update(const uint8_t* data, size_t size) {
        return mbedtls_sha256_update(&context_, data, size) == 0;
    }
    bool Finish(uint8_t* result) { return mbedtls_sha256_finish(&context_, result) == 0; }
private:
    mbedtls_sha256_context context_;
};

// KMP retains only the matched suffix length between chunks. The prefix table
// is constant flash data, so scanning never buffers an image or an extra block.
constexpr std::array<size_t, kMarkerBytes> MarkerPrefix() {
    std::array<size_t, kMarkerBytes> result{};
    for (size_t i = 1, j = 0; i < kMarkerBytes; ++i) {
        while (j && kBoardMarker[i] != kBoardMarker[j]) j = result[j - 1];
        if (kBoardMarker[i] == kBoardMarker[j]) ++j;
        result[i] = j;
    }
    return result;
}
constexpr auto kMarkerPrefix = MarkerPrefix();

class MarkerScanner {
public:
    void Feed(const uint8_t* data, size_t size) {
        if (found_) return;
        for (size_t i = 0; i < size; ++i) {
            while (matched_ && data[i] != static_cast<uint8_t>(kBoardMarker[matched_]))
                matched_ = kMarkerPrefix[matched_ - 1];
            if (data[i] == static_cast<uint8_t>(kBoardMarker[matched_])) ++matched_;
            if (matched_ == kMarkerBytes) {
                found_ = true;
                return;
            }
        }
    }
    bool Found() const { return found_; }
private:
    size_t matched_ = 0;
    bool found_ = false;
};

using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;

esp_err_t SendJson(httpd_req_t* request, const char* status, cJSON* object) {
    if (!object) return ESP_ERR_NO_MEM;
    char* text = cJSON_PrintUnformatted(object);
    if (!text) return ESP_ERR_NO_MEM;
    httpd_resp_set_status(request, status);
    httpd_resp_set_type(request, "application/json; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    httpd_resp_set_hdr(request, "Connection", "close");
    // No CORS headers: a cross-origin page cannot authorize a browser upload.
    const auto error = httpd_resp_send(request, text, HTTPD_RESP_USE_STRLEN);
    cJSON_free(text);
    return error;
}

esp_err_t Reject(httpd_req_t* request, const char* status,
                 const char* code, const char* message) {
    Json object(cJSON_CreateObject(), cJSON_Delete);
    if (object) {
        cJSON_AddBoolToObject(object.get(), "ok", false);
        cJSON_AddStringToObject(object.get(), "error", code);
        cJSON_AddStringToObject(object.get(), "message", message);
        SendJson(request, status, object.get());
    }
    // Close even if the response was sent successfully: the body may be huge
    // and intentionally unread, and must not become a subsequent HTTP request.
    return ESP_FAIL;
}

bool Header(httpd_req_t* request, const char* name, char* output, size_t capacity) {
    const size_t size = httpd_req_get_hdr_value_len(request, name);
    return size && size < capacity &&
        httpd_req_get_hdr_value_str(request, name, output, capacity) == ESP_OK;
}

bool Authorized(httpd_req_t* request) {
    std::array<char, 72> header{};
    if (!Header(request, "Authorization", header.data(), header.size()) ||
        std::strlen(header.data()) != 71 || std::memcmp(header.data(), "Bearer ", 7) != 0 ||
        !EqualBytes(header.data() + 7, Context().token.data(), 64)) {
        httpd_resp_set_hdr(request, "WWW-Authenticate", "Bearer");
        Reject(request, "401 Unauthorized", "unauthorized", "A valid pairing token is required");
        return false;
    }
    return true;
}

const esp_partition_t* UpdatePartition() {
    const auto* running = esp_ota_get_running_partition();
    const auto* next = esp_ota_get_next_update_partition(nullptr);
    if (!running || !next || next->type != ESP_PARTITION_TYPE_APP ||
        next->subtype < ESP_PARTITION_SUBTYPE_APP_OTA_MIN ||
        next->subtype >= ESP_PARTITION_SUBTYPE_APP_OTA_MAX ||
        next->address == running->address) return nullptr;
    return next;
}

bool BootValid() {
    const auto* running = esp_ota_get_running_partition();
    if (!running) return false;
    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    const auto error = esp_ota_get_state_partition(running, &state);
    // A healthy app loaded by the bootloader's fallback from a serial-written
    // OTA slot can legitimately have no otadata record. Do not create one here.
    if (error == ESP_ERR_NOT_FOUND) return true;
    if (error != ESP_OK) return false;
    return state != ESP_OTA_IMG_PENDING_VERIFY && state != ESP_OTA_IMG_INVALID &&
           state != ESP_OTA_IMG_ABORTED && state != ESP_OTA_IMG_NEW;
}

const char* DeviceStateName(DeviceState state) {
    switch (state) {
        case kDeviceStateUnknown: return "unknown";
        case kDeviceStateStarting: return "starting";
        case kDeviceStateWifiConfiguring: return "configuring";
        case kDeviceStateIdle: return "idle";
        case kDeviceStateConnecting: return "connecting";
        case kDeviceStateListening: return "listening";
        case kDeviceStateSpeaking: return "speaking";
        case kDeviceStateUpgrading: return "upgrading";
        case kDeviceStateActivating: return "activating";
        case kDeviceStateAudioTesting: return "audio_testing";
        case kDeviceStateFatalError: return "fatal_error";
        default: return "invalid_state";
    }
}

bool LoadToken(ServerContext& context) {
    nvs_handle_t nvs = 0;
    auto error = nvs_open("local_ota", NVS_READWRITE, &nvs);
    if (error != ESP_OK) {
        ESP_LOGE(kTag, "Could not open local OTA settings: %s", esp_err_to_name(error));
        return false;
    }
    size_t size = context.token.size();
    error = nvs_get_str(nvs, "token", context.token.data(), &size);
    bool generated = false;
    if (error == ESP_ERR_NVS_NOT_FOUND) {
        std::array<uint8_t, 32> bytes{};
        esp_fill_random(bytes.data(), bytes.size());
        ToHex(bytes.data(), bytes.size(), context.token.data());
        error = nvs_set_str(nvs, "token", context.token.data());
        if (error == ESP_OK) error = nvs_commit(nvs);
        generated = error == ESP_OK;
    } else if (error == ESP_OK) {
        if (size != 65 || context.token[64] != '\0' ||
            !std::all_of(context.token.begin(), context.token.begin() + 64, IsHex))
            error = ESP_ERR_INVALID_STATE;
    }
    nvs_close(nvs);
    if (error != ESP_OK) {
        context.token.fill(0);
        ESP_LOGE(kTag, "Could not load local OTA token: %s", esp_err_to_name(error));
        return false;
    }
    // The only credential log, only after the first successful persistent save.
    // The developer captures this through the explicitly authorized private UART.
    if (generated) ESP_LOGI(kTag, "Pairing token: %s", context.token.data());
    return true;
}

esp_err_t Health(httpd_req_t* request) {
    Json object(cJSON_CreateObject(), cJSON_Delete);
    if (!object) return ESP_ERR_NO_MEM;
    cJSON_AddStringToObject(object.get(), "service", "xiaozhi-local-ota");
    cJSON_AddNumberToObject(object.get(), "api", 1);
    return SendJson(request, "200 OK", object.get());
}

esp_err_t Status(httpd_req_t* request) {
    if (!Authorized(request)) return ESP_FAIL;
    const auto* description = esp_app_get_description();
    const auto* running = esp_ota_get_running_partition();
    const auto* next = UpdatePartition();
    std::array<char, 65> elf{};
    ToHex(description->app_elf_sha256, sizeof(description->app_elf_sha256), elf.data());
    Json object(cJSON_CreateObject(), cJSON_Delete);
    if (!object) return ESP_ERR_NO_MEM;
    cJSON_AddNumberToObject(object.get(), "api", 1);
    cJSON_AddStringToObject(object.get(), "project", kProject);
    cJSON_AddStringToObject(object.get(), "board", kBoard);
    cJSON_AddStringToObject(object.get(), "version", description->version);
    cJSON_AddStringToObject(object.get(), "idf_version", description->idf_ver);
    cJSON_AddStringToObject(object.get(), "app_elf_sha256", elf.data());
    cJSON_AddStringToObject(object.get(), "device_id", Context().device_id.data());
    cJSON_AddStringToObject(object.get(), "running_partition", running ? running->label : "");
    cJSON_AddNumberToObject(object.get(), "running_address", running ? running->address : 0);
    cJSON_AddStringToObject(object.get(), "next_partition", next ? next->label : "");
    cJSON_AddNumberToObject(object.get(), "max_image_bytes", next ? next->size : 0);
#ifdef CONFIG_ENABLE_OTA_UPGRADE
    cJSON_AddBoolToObject(object.get(), "server_ota_enabled", true);
#else
    cJSON_AddBoolToObject(object.get(), "server_ota_enabled", false);
#endif
    auto& application = Application::GetInstance();
    cJSON_AddStringToObject(object.get(), "device_state", DeviceStateName(application.GetDeviceState()));
    cJSON_AddBoolToObject(object.get(), "updating", Context().uploading.load() || application.IsLocalUpgradeActive());
    cJSON_AddBoolToObject(object.get(), "boot_valid", BootValid());
    // Authenticated development diagnostics; no serial session/reset required.
    // DMA completion is measured by the BOX driver, not by HTTP or LVGL submit.
    auto* display = Board::GetInstance().GetDisplay();
    const auto refresh = display ? display->GetRefreshStats() : DisplayRefreshStats{};
    auto* refresh_json = cJSON_AddObjectToObject(object.get(), "display_refresh");
    if (refresh_json) {
        cJSON_AddBoolToObject(refresh_json, "available", refresh.available);
        cJSON_AddBoolToObject(refresh_json, "full_frame", refresh.full_frame);
        cJSON_AddNumberToObject(refresh_json, "buffer_bytes", refresh.buffer_bytes);
        cJSON_AddNumberToObject(refresh_json, "completed_frames", refresh.completed_frames);
        cJSON_AddNumberToObject(refresh_json, "last_dma_us", refresh.last_dma_us);
        cJSON_AddNumberToObject(refresh_json, "max_dma_us", refresh.max_dma_us);
        cJSON_AddNumberToObject(refresh_json, "transfer_errors", refresh.transfer_errors);
        cJSON_AddBoolToObject(refresh_json, "scan_synchronized", refresh.scan_synchronized);
        cJSON_AddBoolToObject(refresh_json, "native_order", refresh.native_order);
        cJSON_AddNumberToObject(refresh_json, "sync_timeouts", refresh.sync_timeouts);
        cJSON_AddNumberToObject(refresh_json, "last_scan_line", refresh.last_scan_line);
        cJSON_AddNumberToObject(refresh_json, "last_wait_us", refresh.last_wait_us);
        cJSON_AddNumberToObject(refresh_json, "last_phase_delay_us", refresh.last_phase_delay_us);
        cJSON_AddNumberToObject(refresh_json, "late_gate_skips", refresh.late_gate_skips);
    }
    cJSON_AddNumberToObject(object.get(), "internal_free_bytes", heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(object.get(), "internal_min_free_bytes", heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
#if defined(CONFIG_BOARD_TYPE_ATK_DNESP32S3_BOX)
    const auto scan = box_lcd::GetScanProbeResult();
    auto* scan_json = cJSON_AddObjectToObject(object.get(), "lcd_scan_probe");
    if (scan_json) {
        cJSON_AddBoolToObject(scan_json, "attempted", scan.attempted);
        cJSON_AddBoolToObject(scan_json, "registers_match", scan.registers_match);
        cJSON_AddBoolToObject(scan_json, "scan_valid", scan.scan_valid);
        cJSON_AddBoolToObject(scan_json, "pins_restored", scan.pins_restored);
        cJSON_AddNumberToObject(scan_json, "madctl", scan.madctl);
        cJSON_AddNumberToObject(scan_json, "colmod", scan.colmod);
        cJSON_AddNumberToObject(scan_json, "sample_count", scan.sample_count);
        cJSON_AddNumberToObject(scan_json, "transitions", scan.transitions);
        cJSON_AddNumberToObject(scan_json, "wraps", scan.wraps);
        cJSON_AddNumberToObject(scan_json, "min_line", scan.min_line);
        cJSON_AddNumberToObject(scan_json, "max_line", scan.max_line);
        cJSON_AddNumberToObject(scan_json, "frame_period_us", scan.frame_period_us);
        auto* samples = cJSON_AddArrayToObject(scan_json, "samples");
        if (samples) {
            for (uint32_t i = 0; i < std::min<uint32_t>(scan.sample_count, scan.samples.size()); ++i) {
                auto* sample = cJSON_CreateObject();
                if (!sample) break;
                cJSON_AddNumberToObject(sample, "us", scan.samples[i].us);
                cJSON_AddNumberToObject(sample, "line", scan.samples[i].line);
                cJSON_AddItemToArray(samples, sample);
            }
        }
    }
#endif
    return SendJson(request, "200 OK", object.get());
}

struct ReadyContext {
    SemaphoreHandle_t semaphore = xSemaphoreCreateBinary();
    std::mutex mutex;
    bool complete = false;
    bool accepted = false;
    bool timed_out = false;
    bool restored = false;
    ~ReadyContext() { if (semaphore) vSemaphoreDelete(semaphore); }
};

// The callback owns its context even after HTTP has timed out. Exactly one side
// restores an accepted request, including acceptance arriving after the timeout.
bool WaitForApplication(bool& timed_out) {
    auto ready = std::make_shared<ReadyContext>();
    if (!ready->semaphore) return false;
    Application::GetInstance().RequestLocalUpgrade([ready](bool accepted) {
        bool restore = false;
        {
            std::lock_guard<std::mutex> lock(ready->mutex);
            ready->complete = true;
            ready->accepted = accepted;
            if (ready->timed_out && accepted && !ready->restored) {
                ready->restored = true;
                restore = true;
            }
            xSemaphoreGive(ready->semaphore);
        }
        if (restore) Application::GetInstance().FinishLocalUpgrade(false, "Local upload readiness timed out");
    });
    if (xSemaphoreTake(ready->semaphore, pdMS_TO_TICKS(3000)) == pdTRUE) {
        std::lock_guard<std::mutex> lock(ready->mutex);
        return ready->accepted;
    }
    bool restore = false;
    {
        std::lock_guard<std::mutex> lock(ready->mutex);
        ready->timed_out = true;
        timed_out = true;
        if (ready->complete && ready->accepted && !ready->restored) {
            ready->restored = true;
            restore = true;
        }
    }
    if (restore) Application::GetInstance().FinishLocalUpgrade(false, "Local upload readiness timed out");
    return false;
}

struct UpdateSession {
    esp_ota_handle_t handle = 0;
    bool application_ready = false;
    bool committed = false;
    bool reboot_armed = false;
    void ArmReboot() {
        reboot_armed = true;
        if (esp_timer_start_once(Context().reboot_timer, 1000 * 1000) != ESP_OK) {
            ESP_LOGE(kTag, "Could not arm reboot timer; rebooting after reply delay");
            vTaskDelay(pdMS_TO_TICKS(1000));
            esp_restart();
        }
    }
    ~UpdateSession() {
        if (handle) esp_ota_abort(handle);
        if (committed) {
            // Even an allocation failure while making the reply must not leave
            // a committed boot selection waiting indefinitely for a reboot.
            if (!reboot_armed) ArmReboot();
            return;
        }
        if (application_ready) {
            try {
                Application::GetInstance().FinishLocalUpgrade(false, "Local firmware upload interrupted");
            } catch (...) {
                ESP_LOGE(kTag, "Could not queue local upload recovery");
            }
        }
        Context().uploading.store(false);
    }
    void Fail(const char* reason) {
        if (handle) {
            esp_ota_abort(handle);
            handle = 0;
        }
        if (application_ready) {
            Application::GetInstance().FinishLocalUpgrade(false, reason);
            application_ready = false;
        }
    }
};

bool ValidPrefix(const uint8_t* data) {
    esp_image_header_t image{};
    esp_image_segment_header_t segment{};
    esp_app_desc_t app{};
    std::memcpy(&image, data, sizeof(image));
    std::memcpy(&segment, data + sizeof(image), sizeof(segment));
    std::memcpy(&app, data + sizeof(image) + sizeof(segment), sizeof(app));
    return image.magic == ESP_IMAGE_HEADER_MAGIC && image.chip_id == ESP_CHIP_ID_ESP32S3 &&
        image.segment_count > 0 && image.segment_count <= ESP_IMAGE_MAX_SEGMENTS &&
        segment.data_len >= sizeof(esp_app_desc_t) &&
        app.magic_word == ESP_APP_DESC_MAGIC_WORD &&
        std::memcmp(app.project_name, "xiaozhi\0", 8) == 0 &&
        std::strncmp(app.idf_ver, "v5.5", 4) == 0;
}

enum class ReadResult { Data, Timeout, Closed };

ReadResult Receive(httpd_req_t* request, uint8_t* buffer, size_t wanted,
                   int64_t started, int64_t& last_data, size_t& count) {
    while (true) {
        const auto now = esp_timer_get_time();
        if (now - started >= kUploadUs || now - last_data >= kStallUs) return ReadResult::Timeout;
        const int received = httpd_req_recv(request, reinterpret_cast<char*>(buffer), wanted);
        const auto finished = esp_timer_get_time();
        if (finished - started >= kUploadUs || finished - last_data >= kStallUs)
            return ReadResult::Timeout;
        if (received > 0) {
            count = static_cast<size_t>(received);
            last_data = finished;
            return ReadResult::Data;
        }
        if (received == HTTPD_SOCK_ERR_TIMEOUT) continue;
        return ReadResult::Closed;
    }
}

esp_err_t Update(httpd_req_t* request) {
    if (!Authorized(request)) return ESP_FAIL;
    std::array<char, 65> digest_header{};
    std::array<uint8_t, 32> expected_digest{};
    std::array<char, 64> board_header{};
    std::array<char, 64> type_header{};
    if (!Header(request, "Content-Type", type_header.data(), type_header.size()) ||
        std::strcmp(type_header.data(), "application/octet-stream") != 0 ||
        httpd_req_get_hdr_value_len(request, "Transfer-Encoding") != 0 ||
        httpd_req_get_hdr_value_len(request, "Content-Encoding") != 0)
        return Reject(request, "415 Unsupported Media Type", "content_type", "Send an unencoded raw application/octet-stream body");
    if (!Header(request, "X-Board-Type", board_header.data(), board_header.size()) ||
        std::strcmp(board_header.data(), kBoard) != 0)
        return Reject(request, "422 Unprocessable Content", "board", "The board type must match this BOX");
    if (!Header(request, "X-Firmware-SHA256", digest_header.data(), digest_header.size()) ||
        !ReadDigest(digest_header.data(), expected_digest))
        return Reject(request, "422 Unprocessable Content", "sha256_header", "A 64-digit firmware SHA256 is required");
    const auto* target = UpdatePartition();
    if (!target) return Reject(request, "409 Conflict", "partition", "No inactive OTA application slot is available");
    if (request->content_len < kPrefixBytes)
        return Reject(request, "422 Unprocessable Content", "image_size", "The firmware image is too short");
    if (request->content_len > target->size)
        return Reject(request, "413 Content Too Large", "image_size", "The firmware exceeds the inactive OTA slot");
    if (!BootValid())
        return Reject(request, "409 Conflict", "boot_pending", "The running image has not passed local health confirmation");
    auto& application = Application::GetInstance();
    bool idle = false;
    if (application.IsLocalUpgradeActive() || !Context().uploading.compare_exchange_strong(idle, true))
        return Reject(request, "409 Conflict", "busy", "A local upgrade is already active");
    UpdateSession session;
    auto fail = [&](const char* status, const char* code, const char* reason) {
        ESP_LOGW(kTag, "Local update rejected: %s", reason);
        session.Fail(reason);
        return Reject(request, status, code, reason);
    };
    std::unique_ptr<uint8_t[]> buffer(new (std::nothrow) uint8_t[kBufferBytes]);
    if (!buffer) return fail("503 Service Unavailable", "memory", "Upload memory is unavailable");
    Digest digest;
    if (!digest.Start()) return fail("500 Internal Server Error", "sha256", "Could not start firmware hashing");
    const int64_t started = esp_timer_get_time();
    int64_t last_data = started;
    size_t received = 0;
    while (received < kPrefixBytes) {
        size_t count = 0;
        const auto result = Receive(request, buffer.get() + received, kPrefixBytes - received,
                                    started, last_data, count);
        if (result != ReadResult::Data)
            return fail(result == ReadResult::Timeout ? "408 Request Timeout" : "400 Bad Request",
                        "upload_interrupted", "The firmware header upload was interrupted");
        received += count;
    }
    if (!ValidPrefix(buffer.get()))
        return fail("422 Unprocessable Content", "image_header", "This is not a compatible Xiaozhi ESP32-S3 IDF v5.5 application");
    bool readiness_timeout = false;
    if (!WaitForApplication(readiness_timeout))
        return fail(readiness_timeout ? "503 Service Unavailable" : "409 Conflict", "application_busy",
                    readiness_timeout ? "The application did not become ready in time" : "The application cannot accept an upgrade now");
    session.application_ready = true;
    // Incremental erasing avoids a long full-slot erase before any progress.
    auto error = esp_ota_begin(target, OTA_WITH_SEQUENTIAL_WRITES, &session.handle);
    if (error != ESP_OK) {
        ESP_LOGE(kTag, "esp_ota_begin failed: %s", esp_err_to_name(error));
        return fail("500 Internal Server Error", "ota_begin", "Could not begin writing the inactive OTA slot");
    }
    MarkerScanner marker;
    marker.Feed(buffer.get(), received);
    if (!digest.Update(buffer.get(), received) || esp_ota_write(session.handle, buffer.get(), received) != ESP_OK)
        return fail("500 Internal Server Error", "ota_write", "Could not write the firmware header");
    ESP_LOGI(kTag, "Receiving local firmware: %u bytes into %s", static_cast<unsigned>(request->content_len), target->label);
    int last_progress = -1;
    while (received < request->content_len) {
        size_t count = 0;
        const auto result = Receive(request, buffer.get(), std::min(kBufferBytes, request->content_len - received),
                                    started, last_data, count);
        if (result != ReadResult::Data)
            return fail(result == ReadResult::Timeout ? "408 Request Timeout" : "400 Bad Request",
                        "upload_interrupted", "The firmware upload was interrupted");
        marker.Feed(buffer.get(), count);
        if (!digest.Update(buffer.get(), count) || esp_ota_write(session.handle, buffer.get(), count) != ESP_OK)
            return fail("500 Internal Server Error", "ota_write", "Could not write the firmware data");
        received += count;
        const int progress = std::min(99, static_cast<int>(received * 100 / request->content_len));
        if (progress != last_progress) {
            application.ReportLocalUpgradeProgress(progress);
            last_progress = progress;
        }
    }
    std::array<uint8_t, 32> actual_digest{};
    if (!digest.Finish(actual_digest.data()))
        return fail("500 Internal Server Error", "sha256", "Could not finish firmware hashing");
    if (!EqualBytes(actual_digest.data(), expected_digest.data(), actual_digest.size()))
        return fail("422 Unprocessable Content", "sha256_mismatch", "The uploaded firmware SHA256 does not match");
    if (!marker.Found())
        return fail("422 Unprocessable Content", "board_marker", "The firmware does not contain this BOX board marker");
    if (esp_timer_get_time() - started >= kUploadUs)
        return fail("408 Request Timeout", "upload_timeout", "The firmware upload exceeded its time limit");
    const auto handle = session.handle;
    session.handle = 0; // esp_ota_end frees its handle on every return path.
    error = esp_ota_end(handle);
    if (error != ESP_OK) {
        ESP_LOGE(kTag, "esp_ota_end failed: %s", esp_err_to_name(error));
        return fail("422 Unprocessable Content", "image_invalid", "The uploaded application failed ESP image validation");
    }
    if (esp_timer_get_time() - started >= kUploadUs)
        return fail("408 Request Timeout", "upload_timeout", "The firmware validation exceeded its time limit");
    error = esp_ota_set_boot_partition(target);
    if (error != ESP_OK) {
        ESP_LOGE(kTag, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(error));
        return fail("500 Internal Server Error", "boot_partition", "Could not select the validated application for boot");
    }
    session.committed = true;
    try {
        application.ReportLocalUpgradeProgress(100);
        application.FinishLocalUpgrade(true, "Local firmware verified; rebooting");
    } catch (...) {
        ESP_LOGE(kTag, "Could not queue the final upgrade notification");
    }
    std::array<char, 65> actual_hex{};
    ToHex(actual_digest.data(), actual_digest.size(), actual_hex.data());
    ESP_LOGI(kTag, "Local firmware validated: bytes=%u sha256=%s target=%s", static_cast<unsigned>(received), actual_hex.data(), target->label);
    Json response(cJSON_CreateObject(), cJSON_Delete);
    esp_err_t sent = ESP_ERR_NO_MEM;
    if (response) {
        cJSON_AddBoolToObject(response.get(), "ok", true);
        cJSON_AddBoolToObject(response.get(), "rebooting", true);
        cJSON_AddNumberToObject(response.get(), "bytes", received);
        cJSON_AddStringToObject(response.get(), "sha256", actual_hex.data());
        cJSON_AddStringToObject(response.get(), "target", target->label);
        sent = SendJson(request, "200 OK", response.get());
    }
    // A lost HTTP response does not undo an already committed boot selection.
    // The Mac verifies the new ELF identity after reconnection instead.
    session.ArmReboot();
    return sent;
}

template <esp_err_t (*Handler)(httpd_req_t*)>
esp_err_t GuardHandler(httpd_req_t* request) {
    try {
        return Handler(request);
    } catch (...) {
        ESP_LOGE(kTag, "Local OTA request could not be completed");
        return Reject(request, "500 Internal Server Error", "internal", "The local request could not be completed");
    }
}

} // namespace

LocalOta& LocalOta::GetInstance() {
    static LocalOta instance;
    return instance;
}

bool LocalOta::Start() {
    auto& context = Context();
    if (context.server) return true;
    try {
        if (!LoadToken(context)) return false;
        Digest digest;
        const auto uuid = Board::GetInstance().GetUuid();
        std::array<uint8_t, 32> uuid_hash{};
        if (!digest.Start() || !digest.Update(reinterpret_cast<const uint8_t*>(uuid.data()), uuid.size()) ||
            !digest.Finish(uuid_hash.data())) return false;
        ToHex(uuid_hash.data(), 8, context.device_id.data());
        esp_timer_create_args_t timer{};
        timer.callback = [](void*) { esp_restart(); };
        timer.name = "local_ota_reboot";
        if (esp_timer_create(&timer, &context.reboot_timer) != ESP_OK) return false;
        httpd_config_t config = HTTPD_DEFAULT_CONFIG();
        config.server_port = 8080;
        config.ctrl_port = ESP_HTTPD_DEF_CTRL_PORT + 1;
        config.stack_size = 8192;
        config.max_open_sockets = 3;
        config.max_uri_handlers = 3;
        config.recv_wait_timeout = 5;
        config.send_wait_timeout = 5;
        auto error = httpd_start(&context.server, &config);
        if (error == ESP_OK) {
            const httpd_uri_t routes[] = {
                {.uri = "/health", .method = HTTP_GET, .handler = GuardHandler<Health>, .user_ctx = nullptr},
                {.uri = "/api/status", .method = HTTP_GET, .handler = GuardHandler<Status>, .user_ctx = nullptr},
                {.uri = "/api/update", .method = HTTP_POST, .handler = GuardHandler<Update>, .user_ctx = nullptr},
            };
            for (const auto& route : routes) {
                error = httpd_register_uri_handler(context.server, &route);
                if (error != ESP_OK) break;
            }
        }
        if (error != ESP_OK) {
            ESP_LOGE(kTag, "Could not start local OTA service: %s", esp_err_to_name(error));
            if (context.server) httpd_stop(context.server);
            context.server = nullptr;
            esp_timer_delete(context.reboot_timer);
            context.reboot_timer = nullptr;
            return false;
        }
        ESP_LOGI(kTag, "Authenticated local OTA service ready on port 8080");
        return true;
    } catch (...) {
        if (context.server) httpd_stop(context.server);
        context.server = nullptr;
        if (context.reboot_timer) esp_timer_delete(context.reboot_timer);
        context.reboot_timer = nullptr;
        ESP_LOGE(kTag, "Local OTA service is unavailable");
        return false;
    }
}
