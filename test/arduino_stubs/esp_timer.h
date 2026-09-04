/* esp_timer.h - stub, host compilation only. See Arduino.h in this folder.
 *
 * The Arduino ESP32 core pulls this in through Arduino.h, so sketches use
 * esp_timer_* without including anything. Declared here for the same reason.
 * Nothing schedules: a periodic timer started on the host never fires, so any
 * callback-driven logic is compiled and never exercised.
 */
#ifndef ESP_TIMER_STUB_H
#define ESP_TIMER_STUB_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct esp_timer *esp_timer_handle_t;
typedef int esp_err_t;

#define ESP_OK 0

typedef enum {
    ESP_TIMER_TASK,
    ESP_TIMER_ISR
} esp_timer_dispatch_t;

typedef struct {
    void (*callback)(void *arg);
    void *arg;
    esp_timer_dispatch_t dispatch_method;
    const char *name;
    bool skip_unhandled_events;
} esp_timer_create_args_t;

esp_err_t esp_timer_create(const esp_timer_create_args_t *args,
                           esp_timer_handle_t *out_handle);
esp_err_t esp_timer_start_periodic(esp_timer_handle_t timer, uint64_t period_us);
esp_err_t esp_timer_start_once(esp_timer_handle_t timer, uint64_t timeout_us);
esp_err_t esp_timer_stop(esp_timer_handle_t timer);
esp_err_t esp_timer_delete(esp_timer_handle_t timer);
int64_t   esp_timer_get_time(void);

#ifdef __cplusplus
}
#endif

#endif /* ESP_TIMER_STUB_H */
