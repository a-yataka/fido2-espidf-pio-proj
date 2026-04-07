#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "board_mode.h"
#include "transport.h"

static const char *TAG = "main";

void app_main(void)
{
    transport_mode_t selected_mode = board_detect_mode();
    const transport_vtable_t *transport = NULL;

    if (selected_mode == TRANSPORT_MODE_USB_HID) {
        ESP_LOGI(TAG, "Mode selected: USB HID");
        transport = transport_usb_hid_get();
    } else {
        ESP_LOGI(TAG, "Mode selected: BLE");
        transport = transport_ble_get();
    }

    if (!transport || !transport->init()) {
        ESP_LOGE(TAG, "Transport init failed");
        while (1) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    uint8_t buf[64];
    size_t len = 0;

    while (1) {
        transport->poll();

        if (transport->recv(buf, sizeof(buf), &len)) {
            ESP_LOGI(TAG, "Received %u bytes in mode %d",
                     (unsigned)len, (int)transport->mode());
            transport->send(buf, len);
        }

        if (transport->mode() == TRANSPORT_MODE_BLE) {
            // 今は未実装なので静かに待つだけ
            vTaskDelay(pdMS_TO_TICKS(100));
        } else {
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }
}