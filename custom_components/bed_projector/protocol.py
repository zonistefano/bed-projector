"""Bounded, transport-independent normalization of Home Assistant states."""

from __future__ import annotations

from collections import Counter
from datetime import date, datetime, time, timedelta, tzinfo
from typing import Any

from .const import (CONF_ALARM, CONF_CLIMATE_COOL, CONF_CLIMATE_HEAT, CONF_EXTRAS, CONF_INDOOR,
                    CONF_NEXT_ALARM, CONF_NEXT_ALARM_ENABLED, CONF_OUTDOOR, CONF_SUMMARY_TODAY,
                    CONF_SUMMARY_TOMORROW, CONF_WEATHER, MESSAGE_DURATION_DEFAULT,
                    MESSAGE_DURATION_RANGE, MESSAGE_ICONS, SEASON_AUTO, SEASON_SUMMER, SEASON_WINTER)

WEATHER_CONDITIONS = {
    "clear-night", "cloudy", "exceptional", "fog", "hail", "lightning", "lightning-rainy",
    "partlycloudy", "pouring", "rainy", "snowy", "snowy-rainy", "sunny", "windy", "windy-variant",
}
# Most severe first: breaks ties when a period has as many hours of each condition.
SEVERITY = (
    "lightning-rainy", "lightning", "hail", "snowy-rainy", "snowy", "pouring", "rainy", "fog",
    "exceptional", "windy-variant", "windy", "cloudy", "partlycloudy", "clear-night", "sunny",
)
ALARM_STATES = {
    "disarmed", "armed_home", "armed_away", "armed_night", "armed_vacation",
    "armed_custom_bypass", "arming", "pending", "triggered",
}
DAY_HOURS = range(8, 18)
EVENING_HOURS = range(18, 24)
MAX_NAMES = 8
FORECAST_DAYS = 2
MAX_EVENTS = 3
# The display names alarms by weekday, so later ones are left out.
WAKE_HORIZON = timedelta(days=7)
# Home Assistant's HVACMode and HVACAction values, as main/projector.c accepts them.
HVAC_MODES = ("off", "heat", "cool", "heat_cool", "auto", "dry", "fan_only")
HVAC_ACTIONS = ("off", "heating", "cooling", "drying", "idle", "fan", "preheating", "defrosting")
# Temperatures travel in tenths of a degree within the firmware's range.
TENTHS_RANGE = (-500, 1000)
# Dial scale in tenths, narrowed to the entity's min_temp and max_temp: the full range of many
# thermostats (7-35 °C) would crowd the useful degrees into a few ticks.
DIAL_RANGE = (100, 300)
# Heating season, for when neither appliance shows which one is in use.
HEATING_MONTHS = (10, 11, 12, 1, 2, 3, 4)
# Common climate preset_mode values as the display names them; others are shown as they are.
PRESETS = {"eco": "Eco", "away": "Fuori casa", "comfort": "Comfort", "boost": "Boost",
           "sleep": "Notte", "home": "In casa", "activity": "Attività"}

Forecast = list[dict[str, Any]]


def short(value: Any, max_bytes: int) -> str:
    """Fit a string into the firmware's UTF-8 byte buffers.

    "#" is dropped: display labels use LVGL's "#rrggbb text#" recolor markup.
    """
    text = str(value or "").replace("#", "")
    return text.encode("utf-8")[:max_bytes].decode("utf-8", "ignore")


def available(entity: Any) -> bool:
    """Whether a state object exists and carries a usable value."""
    return entity is not None and entity.state not in ("unknown", "unavailable")


def number(value: Any) -> float | None:
    """A finite number below 1000 in magnitude, or None."""
    try:
        result = float(value)
    except (TypeError, ValueError):
        return None
    return result if result == result and abs(result) < 1000 else None


def whole_degrees(value: Any) -> int | None:
    """Round a temperature half away from zero; out of range or non-numeric is unknown."""
    result = number(value)
    if result is None or not -99 <= result <= 199:
        return None
    return int(result + 0.5) if result >= 0 else -int(-result + 0.5)


def percent(value: Any) -> int | None:
    result = number(value)
    return None if result is None else max(0, min(100, int(result + 0.5)))


def tenths(value: Any) -> str:
    """A temperature as "21.5°"; empty when not numeric."""
    result = number(value)
    return "" if result is None else short(f"{result:.1f}°", 11)


def local_time(item: dict[str, Any], tz: tzinfo) -> datetime | None:
    try:
        return datetime.fromisoformat(str(item.get("datetime"))).astimezone(tz)
    except (TypeError, ValueError):
        return None


def period(items: Forecast, warmest: bool) -> dict[str, Any] | None:
    """Summarize forecast hours: most frequent condition, warmest or mean temperature,
    highest rain chance."""
    conditions = [item.get("condition") for item in items if item.get("condition") in WEATHER_CONDITIONS]
    if not conditions:
        return None
    counts = Counter(conditions)
    condition = min(counts, key=lambda c: (-counts[c], SEVERITY.index(c)))
    temperatures = [t for t in (number(item.get("temperature")) for item in items) if t is not None]
    temperature = None
    if temperatures:
        temperature = whole_degrees(max(temperatures) if warmest else sum(temperatures) / len(temperatures))
    chances = [p for p in (percent(item.get("precipitation_probability")) for item in items) if p is not None]
    return {"condition": condition, "temperature": temperature,
            "precipitation": max(chances) if chances else None}


def forecast_days(today: date, tz: tzinfo, daily: Forecast | None, hourly: Forecast | None,
                  twice_daily: Forecast | None,
                  sun: dict[date, tuple[datetime | None, datetime | None]] | None,
                  summaries: tuple[str, str]) -> list[dict[str, Any]]:
    """Today and tomorrow with their HA local dates, so the display can pick by its clock.

    Daytime is 08-18 and evening 18-24 of the hourly forecast; without hourly data
    the day falls back to the daily forecast and the evening to twice_daily's night.
    """
    days = []
    for offset in range(FORECAST_DAYS):
        day = today + timedelta(days=offset)
        entry: dict[str, Any] = {"date": day.isoformat()}
        for item in daily or []:
            when = local_time(item, tz)
            if when and when.date() == day and item.get("condition") in WEATHER_CONDITIONS:
                entry.update(condition=item["condition"],
                             high=whole_degrees(item.get("temperature")),
                             low=whole_degrees(item.get("templow")),
                             precipitation=percent(item.get("precipitation_probability")))
                break
        hours = [(local_time(item, tz), item) for item in hourly or []]
        hours = [(when, item) for when, item in hours if when and when.date() == day]
        daytime = period([item for when, item in hours if when.hour in DAY_HOURS], True)
        evening = period([item for when, item in hours if when.hour in EVENING_HOURS], False)
        if daytime is None and "condition" in entry:
            daytime = {"condition": entry["condition"], "temperature": entry.get("high"),
                       "precipitation": entry.get("precipitation")}
        if evening is None:
            for item in twice_daily or []:
                when = local_time(item, tz)
                if when and when.date() == day and item.get("is_daytime") is False:
                    evening = period([item], False)
        entry["day"] = daytime
        entry["evening"] = evening
        sunrise, sunset = (sun or {}).get(day, (None, None))
        entry["sunrise"] = sunrise.astimezone(tz).strftime("%H:%M") if sunrise else ""
        entry["sunset"] = sunset.astimezone(tz).strftime("%H:%M") if sunset else ""
        entry["summary"] = short(summaries[offset], 63)
        days.append(entry)
    return days


def names(values: list[str] | None) -> list[str]:
    return [name for name in (short(value, 31) for value in values or []) if name][:MAX_NAMES]


def template_names(value: Any) -> list[str] | None:
    """Names rendered by a user template: a list or a comma/newline separated string.

    None (render error) stays unknown; "unknown" or "unavailable" too, since a
    template over unavailable entities must not read as "nothing on".
    """
    if value is None:
        return None
    if isinstance(value, (list, tuple, set)):
        items = [str(item) for item in value]
    else:
        text = str(value).strip()
        if text in ("unknown", "unavailable"):
            return None
        items = text.replace("\n", ",").split(",")
    return [item for item in (item.strip() for item in items) if item]


def _minute(now: datetime) -> datetime:
    """Start of the current minute: an alarm still counts during the minute it rings."""
    return now.replace(second=0, microsecond=0)


def _alarm_moment(entity: Any, now: datetime) -> datetime | None:
    """The alarm as an aware datetime, from a timestamp sensor or an input_datetime.

    An input_datetime (has_time attribute) holds HA local time: with a date it is that moment,
    without one it rings every day, so it is the next time the clock reaches it.
    """
    text = str(entity.state)
    attributes = getattr(entity, "attributes", None) or {}
    try:
        if not attributes.get("has_time"):
            when = datetime.fromisoformat(text)
            return when if when.tzinfo is not None else None
        if attributes.get("has_date"):
            return datetime.fromisoformat(text).replace(tzinfo=now.tzinfo)
        when = datetime.combine(now.date(), time.fromisoformat(text), now.tzinfo)
    except ValueError:
        return None
    return when if when >= _minute(now) else when + timedelta(days=1)


def wake_time(entity: Any, now: datetime,
              enabled: Any = None) -> tuple[bool, dict[str, str] | None]:
    """(known, next alarm as HA local date and HH:MM or None) from a timestamp sensor or an
    input_datetime.

    A missing or unavailable entity is unknown; a past alarm or one beyond a week is none.
    enabled is the optional on/off entity of the alarm: off means no alarm whatever time the
    alarm entity holds, and an unusable state makes the alarm unknown.
    """
    if not available(entity):
        return False, None
    if enabled is not None:
        if enabled.state == "off":
            return True, None
        if enabled.state != "on":
            return False, None
    when = _alarm_moment(entity, now)
    if when is None:
        return False, None
    when = when.astimezone(now.tzinfo)
    if when < _minute(now) or when - now > WAKE_HORIZON:
        return True, None
    return True, {"date": when.date().isoformat(), "time": when.strftime("%H:%M")}


def _event_bounds(item: dict[str, Any], tz: tzinfo) -> tuple[datetime, datetime, bool] | None:
    """Local start and end of a calendar event, and whether it lasts all day."""
    try:
        start, end = str(item["start"]), str(item.get("end") or item["start"])
        if "T" not in start:
            first, last = date.fromisoformat(start), date.fromisoformat(end[:10])
            return (datetime.combine(first, time(), tz),
                    datetime.combine(max(last, first + timedelta(days=1)), time(), tz), True)
        begin = datetime.fromisoformat(start)
        finish = datetime.fromisoformat(end)
    except (KeyError, ValueError):
        return None
    if begin.tzinfo is None or finish.tzinfo is None:
        return None
    return begin.astimezone(tz), finish.astimezone(tz), False


def upcoming_events(items: list[dict[str, Any]], now: datetime) -> list[dict[str, str]]:
    """Events from now to the end of tomorrow as returned by calendar.get_events, earliest
    first. Ongoing events count as today's, without a start time when they began earlier."""
    tz = now.tzinfo
    today = now.date()
    horizon = datetime.combine(today + timedelta(days=2), time(), tz)
    found = []
    for item in items:
        bounds = _event_bounds(item, tz)
        title = short(item.get("summary"), 31).strip()
        if bounds is None or not title:
            continue
        start, end, all_day = bounds
        if end <= now or start >= horizon:
            continue
        shown = max(start, datetime.combine(today, time(), tz))
        clock = "" if all_day or start.date() < today else start.strftime("%H:%M")
        found.append((shown, clock, {"date": shown.date().isoformat(), "time": clock, "title": title}))
    # All-day events lead their day, then by start time and title.
    found.sort(key=lambda entry: (entry[0].date(), entry[1] != "", entry[0], entry[2]["title"]))
    return [entry[2] for entry in found[:MAX_EVENTS]]


def tenth_degrees(value: Any) -> int | None:
    """A temperature in tenths of a degree, rounded half away from zero; None when unusable."""
    result = number(value)
    if result is None:
        return None
    scaled = int(abs(result) * 10 + 0.5) * (1 if result >= 0 else -1)
    return scaled if TENTHS_RANGE[0] <= scaled <= TENTHS_RANGE[1] else None


def _configured(options: dict[str, Any], key: str) -> bool:
    return isinstance(options.get(key), str) and bool(options[key])


def _running(entity: Any) -> bool:
    """Heating, cooling or otherwise working right now, as far as hvac_action tells."""
    return entity.attributes.get("hvac_action") not in (None, "idle", "off")


def thermostat_source(states: dict[str, Any], options: dict[str, Any], season: str,
                      month: int | None) -> str | None:
    """"heat" (the radiators) or "cool" (the air conditioner): the climate entity to show.

    A season picks one; automatically, the one switched on, then the one running, then the
    heating season by month. None when no climate entity is configured.
    """
    sources = {"heat": CONF_CLIMATE_HEAT, "cool": CONF_CLIMATE_COOL}
    configured = [source for source, key in sources.items() if _configured(options, key)]
    if len(configured) < 2:
        return configured[0] if configured else None
    if season == SEASON_WINTER:
        return "heat"
    if season == SEASON_SUMMER:
        return "cool"
    entities = {source: states.get(options[sources[source]]) for source in configured}
    on = [source for source, entity in entities.items()
          if available(entity) and entity.state in HVAC_MODES and entity.state != "off"]
    running = [source for source in on if _running(entities[source])]
    for candidates in (on, running):
        if len(candidates) == 1:
            return candidates[0]
    return "heat" if month is None or month in HEATING_MONTHS else "cool"


def thermostat(entity: Any, source: str, room: Any) -> dict[str, Any] | None:
    """The thermostat as the display draws it; None when the entity is unusable.

    The room temperature falls back to the bedroom sensor for climate entities without one;
    with a heat_cool range the set temperature is the bound this appliance works towards.
    """
    if not available(entity) or entity.state not in HVAC_MODES:
        return None
    attributes = entity.attributes
    target = tenth_degrees(attributes.get("temperature"))
    if target is None:
        target = tenth_degrees(attributes.get("target_temp_low" if source == "heat" else "target_temp_high"))
    current = tenth_degrees(attributes.get("current_temperature"))
    if current is None and available(room):
        current = tenth_degrees(room.state)
    low, high = DIAL_RANGE
    entity_low = tenth_degrees(attributes.get("min_temp"))
    entity_high = tenth_degrees(attributes.get("max_temp"))
    if entity_low is not None:
        low = max(low, entity_low)
    if entity_high is not None:
        high = min(high, entity_high)
    if low >= high:
        low, high = DIAL_RANGE
    if target is not None:
        low, high = min(low, target), max(high, target)
    action = attributes.get("hvac_action")
    preset = attributes.get("preset_mode")
    preset = "" if preset in (None, "none") else short(PRESETS.get(str(preset), preset), 15)
    return {
        "source": source,
        "mode": entity.state,
        "action": action if action in HVAC_ACTIONS else None,
        "current": current,
        "target": target,
        "min": low,
        "max": high,
        "humidity": percent(attributes.get("current_humidity")),
        "preset": preset,
    }


def message_payload(text: Any, icon: str | None = None,
                    duration: int = MESSAGE_DURATION_DEFAULT) -> dict[str, Any]:
    """Body of POST /api/message; ValueError when the firmware would refuse it."""
    message = short(text, 63).strip()
    if not message:
        raise ValueError("empty message")
    if icon is not None and icon not in MESSAGE_ICONS:
        raise ValueError(f"unknown icon {icon}")
    if not MESSAGE_DURATION_RANGE[0] <= duration <= MESSAGE_DURATION_RANGE[1]:
        raise ValueError("duration out of range")
    payload: dict[str, Any] = {"text": message, "duration": duration}
    if icon is not None:
        payload["icon"] = icon
    return payload


def make_snapshot(states: dict[str, Any], options: dict[str, Any], *,
                  daily: Forecast | None = None, hourly: Forecast | None = None,
                  twice_daily: Forecast | None = None,
                  today: date | None = None, tz: tzinfo | None = None,
                  sun: dict[date, tuple[datetime | None, datetime | None]] | None = None,
                  opening_names: list[str] | None = None,
                  light_names: list[str] | None = None,
                  now: datetime | None = None,
                  events: list[dict[str, Any]] | None = None,
                  season: str = SEASON_AUTO) -> dict[str, Any]:
    """Build one complete state update; absent or unavailable inputs stay unknown.

    The per-day forecast is sent only when today and tz (HA's local date and zone) are given;
    the next alarm and the calendar events only with now, HA's aware local time. events is
    the raw calendar.get_events list, None when no calendar is configured or it failed.
    season picks the thermostat shown when both climate entities are configured.
    """
    def entity(key: str) -> Any:
        entity_id = options.get(key)
        return states.get(entity_id) if isinstance(entity_id, str) else None

    alarm_entity = entity(CONF_ALARM)
    alarm = alarm_entity.state if alarm_entity and alarm_entity.state in ALARM_STATES else "unknown"

    weather_entity = entity(CONF_WEATHER)
    weather = temperature = wind = feels = ""
    temp_now = humidity = None
    outdoor = ""
    if available(weather_entity):
        attributes = weather_entity.attributes
        weather = short(weather_entity.state, 31)
        value = attributes.get("temperature")
        if value is not None:
            temperature = short(f"{value}{attributes.get('temperature_unit', '°C')}", 15)
        temp_now = whole_degrees(value)
        outdoor = tenths(value)
        apparent = whole_degrees(attributes.get("apparent_temperature"))
        feels = f"{apparent}°" if apparent is not None else ""
        humidity = percent(attributes.get("humidity"))
        speed = number(attributes.get("wind_speed"))
        if speed is not None:
            wind = short(f"{speed:.0f} {attributes.get('wind_speed_unit', 'km/h')}", 15)
    outdoor_entity = entity(CONF_OUTDOOR)
    if available(outdoor_entity) and tenths(outdoor_entity.state):
        outdoor = tenths(outdoor_entity.state)
    indoor_entity = entity(CONF_INDOOR)

    extras = []
    for key in CONF_EXTRAS:
        extra = entity(key)
        if available(extra):
            label = extra.attributes.get("friendly_name", key)
            unit = extra.attributes.get("unit_of_measurement", "")
            extras.append(short(f"{label}: {extra.state}{unit}", 31))
        else:
            extras.append("")

    def summary(key: str) -> str:
        source = entity(key)
        return str(source.state) if available(source) else ""

    wake_known, wake = (wake_time(entity(CONF_NEXT_ALARM), now, entity(CONF_NEXT_ALARM_ENABLED))
                        if now is not None else (False, None))
    source = thermostat_source(states, options, season, today.month if today is not None else None)
    climate = None
    if source is not None:
        climate = thermostat(entity(CONF_CLIMATE_HEAT if source == "heat" else CONF_CLIMATE_COOL), source,
                             indoor_entity)
    events_known = events is not None and now is not None

    # Both counts come from the names; for lights this leaves out the projector's
    # own light, which a generic "lights on" helper would count whenever it is on.
    return {
        "openings": min(len(opening_names), 99) if opening_names is not None else 0,
        "openings_known": opening_names is not None,
        "alarm": alarm,
        "weather": weather,
        "temperature": temperature,
        "extras": extras,
        "lights": min(len(light_names), 99) if light_names is not None else 0,
        "lights_known": light_names is not None,
        "opening_names": names(opening_names),
        "light_names": names(light_names),
        "indoor": tenths(indoor_entity.state) if available(indoor_entity) else "",
        "outdoor": outdoor,
        "temp_now": temp_now,
        "feels": feels,
        "humidity": humidity,
        "wind": wind,
        "days": forecast_days(today, tz, daily, hourly, twice_daily, sun,
                              (summary(CONF_SUMMARY_TODAY), summary(CONF_SUMMARY_TOMORROW)))
        if today is not None and tz is not None else [],
        "wake_known": wake_known,
        "wake": wake,
        "events_known": events_known,
        "events": upcoming_events(events, now) if events_known else [],
        "thermostat": climate,
    }
