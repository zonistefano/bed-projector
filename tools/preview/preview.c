/* Host preview of the projector pages: renders bed_ui.c with the firmware's
 * LVGL options and writes one raw RGB565 frame per scenario. Built and
 * converted to PNG by tools/render_preview.py. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "bed_ui.h"

static uint16_t frame[128 * 128];
static uint32_t ticks;

static uint32_t tick(void) { return ticks; }

static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *pixels)
{
    const uint16_t *source = (const uint16_t *)pixels;
    for (int y = area->y1; y <= area->y2; ++y)
        for (int x = area->x1; x <= area->x2; ++x) frame[y * 128 + x] = *source++;
    lv_display_flush_ready(display);
}

static void run(uint32_t ms)
{
    for (uint32_t t = 0; t < ms; t += 10) {
        ticks += 10;
        lv_timer_handler();
    }
}

static void day(projector_day_t *d, const char *date, const char *condition, int high, int low,
                const char *summary, const char *day_condition, int day_temp, int day_rain,
                const char *evening_condition, int evening_temp, int evening_rain)
{
    *d = (projector_day_t){.high_known=true, .low_known=true, .high=high, .low=low, .precipitation=30};
    snprintf(d->date, sizeof(d->date), "%s", date);
    snprintf(d->condition, sizeof(d->condition), "%s", condition);
    snprintf(d->sunrise, sizeof(d->sunrise), "07:33");
    snprintf(d->sunset, sizeof(d->sunset), "18:47");
    snprintf(d->summary, sizeof(d->summary), "%s", summary);
    d->day = (projector_period_t){.temperature_known=true, .temperature=day_temp, .precipitation=day_rain};
    snprintf(d->day.condition, sizeof(d->day.condition), "%s", day_condition);
    d->evening = (projector_period_t){.temperature_known=true, .temperature=evening_temp,
                                      .precipitation=evening_rain};
    snprintf(d->evening.condition, sizeof(d->evening.condition), "%s", evening_condition);
}

static void sample(projector_snapshot_t *s, bool busy)
{
    memset(s, 0, sizeof(*s));
    s->power = true;
    s->page_count = 3;
    s->pages[0] = (projector_page_t){.id="clock", .name="Ora", .layout="clock"};
    s->pages[1] = (projector_page_t){.id="home", .name="Casa", .layout="home"};
    s->pages[2] = (projector_page_t){.id="weather", .name="Meteo", .layout="weather"};
    s->ha_fresh = true;
    s->openings_known = true;
    s->lights_known = true;
    snprintf(s->alarm, sizeof(s->alarm), busy ? "armed_night" : "disarmed");
    snprintf(s->weather, sizeof(s->weather), "partlycloudy");
    snprintf(s->indoor, sizeof(s->indoor), "21.5\xC2\xB0");
    snprintf(s->outdoor, sizeof(s->outdoor), "17.3\xC2\xB0");
    snprintf(s->feels, sizeof(s->feels), "18\xC2\xB0");
    snprintf(s->wind, sizeof(s->wind), "2 km/h");
    s->temp_now_known = true;
    s->temp_now = 17;
    s->humidity = 71;
    if (busy) {
        s->openings = 2;
        s->opening_name_count = 2;
        snprintf(s->opening_names[0], 32, "Finestra Sala");
        snprintf(s->opening_names[1], 32, "Finestra Cucina");
        s->lights = 3;
        s->light_name_count = 3;
        snprintf(s->light_names[0], 32, "Luce Soggiorno");
        snprintf(s->light_names[1], 32, "Lampada Comò");
        snprintf(s->light_names[2], 32, "Corridoio");
    }
    day(&s->days[0], "2026-10-10", "partlycloudy", 22, 14, "Nubi sparse fino a sera.",
        "partlycloudy", 22, 20, "sunny", 16, 0);
    day(&s->days[1], "2026-10-11", "rainy", 19, 13, "Pioggia dal pomeriggio, più fresco.",
        "cloudy", 19, 30, "rainy", 14, 70);
}

int main(int argc, char **argv)
{
    const char *out = argc > 1 ? argv[1] : ".";
    setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
    tzset();
    lv_init();
    lv_tick_set_cb(tick);
    lv_display_t *display = lv_display_create(128, 128);
    static uint16_t buffer[128 * 128];
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display, buffer, NULL, sizeof(buffer), LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(display, flush);
    bed_ui_create(lv_screen_active());

    /* 2026-10-10 14:35 and 22:10 local time. */
    const time_t afternoon = 1791635700, night = 1791663000;
    static const struct {
        const char *name;
        int page;
        bool busy;
        time_t time;
        int diameter;
    } scenarios[] = {
        {"clock_quiet", 0, false, afternoon, 112},
        {"clock_busy", 0, true, afternoon, 112},
        {"clock_night", 0, true, night, 112},
        {"home_quiet", 1, false, afternoon, 112},
        {"home_busy", 1, true, afternoon, 112},
        {"weather_day", 2, false, afternoon, 112},
        {"weather_night", 2, false, night, 112},
        {"clock_busy_96", 0, true, afternoon, 96},
        {"home_busy_96", 1, true, afternoon, 96},
        {"weather_day_96", 2, false, afternoon, 96},
        {"clock_busy_128", 0, true, afternoon, 128},
        {"weather_day_128", 2, false, afternoon, 128},
    };
    for (size_t i = 0; i < sizeof(scenarios) / sizeof(scenarios[0]); ++i) {
        static projector_snapshot_t s;
        sample(&s, scenarios[i].busy);
        s.page_index = scenarios[i].page;
        s.calibration = (projector_calibration_t){.center_x=64, .center_y=64,
                                                  .diameter=scenarios[i].diameter};
        bed_ui_render(&s, scenarios[i].time, NULL, NULL);
        run(200);
        bed_ui_render(&s, scenarios[i].time, NULL, NULL);
        lv_obj_invalidate(lv_screen_active());
        run(100);
        char path[512];
        snprintf(path, sizeof(path), "%s/%s_%d.rgb565", out, scenarios[i].name, scenarios[i].diameter);
        FILE *file = fopen(path, "wb");
        if (!file) return 1;
        fwrite(frame, sizeof(frame), 1, file);
        fclose(file);
    }
    return 0;
}
