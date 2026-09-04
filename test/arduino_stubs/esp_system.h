/* esp_system.h - stub, host compilation only. See Arduino.h in this folder. */
#ifndef ESP_SYSTEM_STUB_H
#define ESP_SYSTEM_STUB_H

/* RTC_DATA_ATTR normally places a variable in RTC slow memory so it survives a
 * reset. On the host it is just a variable - the boot counter that uses it will
 * always read 0 here, which is one more reason these stubs only prove that a
 * sketch compiles. */
#define RTC_DATA_ATTR

typedef enum {
    ESP_RST_UNKNOWN,
    ESP_RST_POWERON,
    ESP_RST_EXT,
    ESP_RST_SW,
    ESP_RST_PANIC,
    ESP_RST_INT_WDT,
    ESP_RST_TASK_WDT,
    ESP_RST_WDT,
    ESP_RST_DEEPSLEEP,
    ESP_RST_BROWNOUT,
    ESP_RST_SDIO
} esp_reset_reason_t;

esp_reset_reason_t esp_reset_reason(void);

#endif /* ESP_SYSTEM_STUB_H */
