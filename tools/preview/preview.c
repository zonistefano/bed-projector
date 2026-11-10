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

static void event(projector_event_t *e, const char *date, const char *time, const char *title)
{
    snprintf(e->date, sizeof(e->date), "%s", date);
    snprintf(e->time, sizeof(e->time), "%s", time);
    snprintf(e->title, sizeof(e->title), "%s", title);
}

/* Thermostat variants: radiators heating, radiators idle on a preset, air conditioner
 * cooling, radiators off; anything else leaves it unknown. */
static void thermostat(projector_thermostat_t *t, int variant)
{
    static const struct {
        bool cooling;
        const char *mode, *action, *preset;
        int current, target, min, max, humidity;
    } variants[] = {
        {false, "heat", "heating", "", 193, 215, 100, 300, 48},
        {false, "heat", "idle", "Eco", 208, 180, 100, 300, PROJECTOR_UNKNOWN_PERCENT},
        {true, "cool", "cooling", "", 274, 245, 160, 320, 62},
        {false, "off", "off", "", 176, PROJECTOR_UNKNOWN_TENTHS, 100, 300, 51},
    };
    if (variant < 0 || variant >= (int)(sizeof(variants) / sizeof(variants[0]))) return;
    *t = (projector_thermostat_t){.known=true, .cooling=variants[variant].cooling,
        .current=variants[variant].current, .target=variants[variant].target, .min=variants[variant].min,
        .max=variants[variant].max, .humidity=variants[variant].humidity};
    snprintf(t->mode, sizeof(t->mode), "%s", variants[variant].mode);
    snprintf(t->action, sizeof(t->action), "%s", variants[variant].action);
    snprintf(t->preset, sizeof(t->preset), "%s", variants[variant].preset);
}

static void sample(projector_snapshot_t *s, bool busy)
{
    memset(s, 0, sizeof(*s));
    s->power = true;
    snprintf(s->wake_display, sizeof(s->wake_display), "icon");
    s->page_count = 5;
    s->pages[0] = (projector_page_t){.id="clock", .name="Ora", .layout="clock"};
    s->pages[1] = (projector_page_t){.id="home", .name="Casa", .layout="home"};
    s->pages[2] = (projector_page_t){.id="weather", .name="Meteo", .layout="weather"};
    s->pages[3] = (projector_page_t){.id="agenda", .name="Agenda", .layout="agenda"};
    s->pages[4] = (projector_page_t){.id="page1", .name="Elenco", .layout="list",
                                     .widgets={"weather", "alarm", "entity1"}};
    s->ha_fresh = true;
    s->ha.openings_known = true;
    s->ha.lights_known = true;
    snprintf(s->ha.alarm, sizeof(s->ha.alarm), busy ? "armed_night" : "disarmed");
    snprintf(s->ha.weather, sizeof(s->ha.weather), "partlycloudy");
    snprintf(s->ha.temperature, sizeof(s->ha.temperature), "17.3\xC2\xB0" "C");
    snprintf(s->ha.extras[0], sizeof(s->ha.extras[0]), "CO2 camera: 640ppm");
    snprintf(s->ha.indoor, sizeof(s->ha.indoor), "21.5\xC2\xB0");
    snprintf(s->ha.outdoor, sizeof(s->ha.outdoor), "17.3\xC2\xB0");
    snprintf(s->ha.feels, sizeof(s->ha.feels), "18\xC2\xB0");
    snprintf(s->ha.wind, sizeof(s->ha.wind), "2 km/h");
    s->ha.temp_now_known = true;
    s->ha.temp_now = 17;
    s->ha.humidity = 71;
    s->ha.wake_known = true;
    s->ha.events_known = true;
    if (busy) {
        snprintf(s->ha.wake_date, sizeof(s->ha.wake_date), "2026-10-11");
        snprintf(s->ha.wake_time, sizeof(s->ha.wake_time), "06:45");
        s->ha.event_count = 3;
        event(&s->ha.events[0], "2026-10-10", "18:30", "Cena da Marco");
        event(&s->ha.events[1], "2026-10-11", "", "Compleanno Giulia");
        event(&s->ha.events[2], "2026-10-11", "09:00", "Corsa al parco con Luca e Anna");
        s->ha.openings = 2;
        s->ha.opening_name_count = 2;
        snprintf(s->ha.opening_names[0], 32, "Finestra Sala");
        snprintf(s->ha.opening_names[1], 32, "Finestra Cucina");
        s->ha.lights = 3;
        s->ha.light_name_count = 3;
        snprintf(s->ha.light_names[0], 32, "Luce Soggiorno");
        snprintf(s->ha.light_names[1], 32, "Lampada Comò");
        snprintf(s->ha.light_names[2], 32, "Corridoio");
    }
    day(&s->ha.days[0], "2026-10-10", "partlycloudy", 22, 14, "Nubi sparse fino a sera.",
        "partlycloudy", 22, 20, "sunny", 16, 0);
    day(&s->ha.days[1], "2026-10-11", "rainy", 19, 13, "Pioggia dal pomeriggio, più fresco.",
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
    const struct {
        const char *name;
        int page;
        bool busy;
        time_t time;
        int diameter;
        const char *const *widgets; /* Replaces the list page widgets when set. */
        const char *message, *icon;
        bool stale;
        int thermostat; /* Variant for the climate page, shown in place of the list page when > 0. */
        const char *wake_display; /* Replaces the default "icon" when set. */
    } scenarios[] = {
        {"clock_quiet", 0, false, afternoon, 112},
        {"clock_busy", 0, true, afternoon, 112},
        {"clock_night", 0, true, night, 112},
        {"clock_night_time", 0, true, night, 112, NULL, NULL, NULL, false, 0, "time"},
        {"clock_night_countdown", 0, true, night, 112, NULL, NULL, NULL, false, 0, "countdown"},
        {"clock_night_countdown_96", 0, true, night, 96, NULL, NULL, NULL, false, 0, "countdown"},
        {"clock_night_time_128", 0, true, night, 128, NULL, NULL, NULL, false, 0, "time"},
        {"clock_night_none", 0, true, night, 112, NULL, NULL, NULL, false, 0, "none"},
        {"home_quiet", 1, false, afternoon, 112},
        {"home_busy", 1, true, afternoon, 112},
        {"weather_day", 2, false, afternoon, 112},
        {"weather_night", 2, false, night, 112},
        {"clock_busy_96", 0, true, afternoon, 96},
        {"home_busy_96", 1, true, afternoon, 96},
        {"weather_day_96", 2, false, afternoon, 96},
        {"clock_busy_128", 0, true, afternoon, 128},
        {"weather_day_128", 2, false, afternoon, 128},
        {"list_busy", 4, true, afternoon, 112},
        {"list_rooms", 4, true, afternoon, 112, (const char *const[]){"indoor", "outdoor", "sun"}},
        {"list_agenda", 4, true, afternoon, 112, (const char *const[]){"wake", "event", "moon"}},
        {"list_agenda_quiet", 4, false, afternoon, 112, (const char *const[]){"wake", "event", "lights"}},
        {"agenda_busy", 3, true, afternoon, 112},
        {"agenda_quiet", 3, false, afternoon, 112},
        {"agenda_stale", 3, true, afternoon, 112, NULL, NULL, NULL, true},
        {"agenda_busy_96", 3, true, afternoon, 96},
        {"agenda_busy_128", 3, true, afternoon, 128},
        {"message_short", 1, false, afternoon, 112, NULL, "Lavatrice finita", "washing-machine"},
        {"message_long", 1, false, afternoon, 112, NULL,
         "Il cancello del garage \xC3\xA8 rimasto aperto da dieci minuti", "alert"},
        {"message_long_96", 1, false, afternoon, 96, NULL,
         "Il cancello del garage \xC3\xA8 rimasto aperto da dieci minuti", "alert"},
        {"message_plain", 0, false, afternoon, 112, NULL, "Buonanotte"},
        {"climate_heating", 4, false, afternoon, 112, NULL, NULL, NULL, false, 1},
        {"climate_idle", 4, false, afternoon, 112, NULL, NULL, NULL, false, 2},
        {"climate_cooling", 4, false, afternoon, 112, NULL, NULL, NULL, false, 3},
        {"climate_off", 4, false, afternoon, 112, NULL, NULL, NULL, false, 4},
        {"climate_unknown", 4, false, afternoon, 112, NULL, NULL, NULL, false, 9},
        {"climate_heating_96", 4, false, afternoon, 96, NULL, NULL, NULL, false, 1},
        {"climate_cooling_128", 4, false, afternoon, 128, NULL, NULL, NULL, false, 3},
        {"list_thermostat", 4, false, afternoon, 112, (const char *const[]){"thermostat", "indoor", "outdoor"},
         NULL, NULL, false, 0},
    };
    for (size_t i = 0; i < sizeof(scenarios) / sizeof(scenarios[0]); ++i) {
        static projector_snapshot_t s;
        sample(&s, scenarios[i].busy);
        s.page_index = scenarios[i].page;
        if (scenarios[i].wake_display)
            snprintf(s.wake_display, sizeof(s.wake_display), "%s", scenarios[i].wake_display);
        if (scenarios[i].widgets)
            for (int w = 0; w < PROJECTOR_MAX_WIDGETS; ++w)
                snprintf(s.pages[4].widgets[w], sizeof(s.pages[4].widgets[w]), "%s", scenarios[i].widgets[w]);
        if (scenarios[i].message) {
            s.message.active = true;
            snprintf(s.message.text, sizeof(s.message.text), "%s", scenarios[i].message);
            snprintf(s.message.icon, sizeof(s.message.icon), "%s", scenarios[i].icon ? scenarios[i].icon : "");
        }
        thermostat(&s.ha.thermostat, scenarios[i].thermostat ? scenarios[i].thermostat - 1 : 0);
        if (scenarios[i].thermostat)
            s.pages[4] = (projector_page_t){.id="climate", .name="Termostato", .layout="climate"};
        if (scenarios[i].stale) {
            /* As projector_snapshot() delivers HA data after its TTL. */
            memset(&s.ha, 0, sizeof(s.ha));
            s.ha_fresh = false;
        }
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
