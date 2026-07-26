/* ============================================================
 * weather.h — 天气模块
 *
 * 通过 Open-Meteo 免费 API (无需 Key) 获取实时天气，
 * 在独立屏幕显示温度、天气状况、湿度、风速。
 * ============================================================ */

#ifndef UI_WEATHER_H
#define UI_WEATHER_H

#include "lvgl.h"
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    WEATHER_CLEAR = 0,
    WEATHER_PARTLY_CLOUDY,
    WEATHER_CLOUDY,
    WEATHER_FOG,
    WEATHER_DRIZZLE,
    WEATHER_RAIN,
    WEATHER_SNOW,
    WEATHER_THUNDERSTORM,
    WEATHER_UNKNOWN,
} weather_condition_t;

typedef struct {
    float                temperature;
    int                  humidity;
    float                wind_speed;
    weather_condition_t  condition;
    char                 location[64];
    int64_t              last_update;
} weather_data_t;

lv_obj_t *ui_weather_create(void);
void ui_weather_set_back_callback(lv_event_cb_t cb);
bool weather_get_current(weather_data_t *out);
void weather_request_refresh(void);
esp_err_t weather_set_city(const char *city_name);
bool weather_get_city(char *buf, size_t size);
void weather_set_city_coord(const char *city, const char *lat, const char *lon);

#ifdef __cplusplus
}
#endif

#endif /* UI_WEATHER_H */