#include <esp_log.h>
#include <esp_err.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <driver/gpio.h>
#include <esp_event.h>

#include "application.h"
#include "system_info.h"

#define TAG "main"

extern "C" void app_main(void)
{
    // Initialize the default event loop
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Initialize NVS flash for WiFi configuration
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
#if CONFIG_ENABLE_LOCAL_OTA
        // Development updates must never recover by deleting Wi-Fi or identity.
        // Leave the pending image unconfirmed so a reset can roll back instead.
        ESP_LOGE(TAG, "NVS initialization failed; preserving stored configuration");
        return;
#else
        ESP_LOGW(TAG, "Erasing NVS flash to fix corruption");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
#endif
    }
    ESP_ERROR_CHECK(ret);

    // Launch the application
    auto& app = Application::GetInstance();
    app.Start();
}
