"""Bounded, transport-independent normalization of Home Assistant states."""

from __future__ import annotations

from collections import Counter
from datetime import date, datetime, timedelta, tzinfo
from typing import Any


def short(value: Any, max_bytes: int) -> str:
    """Fit a string into the firmware's UTF-8 byte buffers.

    "#" is dropped: display labels use LVGL's "#rrggbb text#" recolor markup.
    """
    text = str(value or "").replace("#", "")
    return text.encode("utf-8")[:max_bytes].decode("utf-8", "ignore")


WEATHER_CONDITIONS = {
    "clear-night", "cloudy", "exceptional", "fog", "hail", "lightning", "lightning-rainy",
    "partlycloudy", "pouring", "rainy", "snowy", "snowy-rainy", "sunny", "windy", "windy-variant",
}


# Most severe first: breaks ties when a period has as many hours of each condition.
SEVERITY = (
    "lightning-rainy", "lightning", "hail", "snowy-rainy", "snowy", "pouring", "rainy", "fog",
    "exceptional", "windy-variant", "windy", "cloudy", "partlycloudy", "clear-night", "sunny",
)
DAY_HOURS = range(8, 18)
EVENING_HOURS = range(18, 24)
MAX_NAMES = 8
FORECAST_DAYS = 2


def whole_degrees(value: Any) -> int | None:
    """Round a forecast temperature for the display; non-numeric values are unknown."""
    try:
        number = float(value)
    except (TypeError, ValueError):
        return None
    if not -99 <= number <= 199:
        return None
    return int(number + 0.5) if number >= 0 else -int(-number + 0.5)


def number(value: Any) -> float | None:
    try:
        result = float(value)
    except (TypeError, ValueError):
        return None
    return result if result == result and abs(result) < 1000 else None


def percent(value: Any) -> int | None:
    result = number(value)
    return None if result is None else max(0, min(100, int(result + 0.5)))


def tenths(entity: Any) -> str:
    """A temperature sensor as "21.5°"; empty when unavailable or not numeric."""
    value = number(entity.state) if entity is not None else None
    return "" if value is None else short(f"{value:.1f}°", 11)


def local_time(item: dict[str, Any], tz: tzinfo) -> datetime | None:
    try:
        return datetime.fromisoformat(str(item.get("datetime"))).astimezone(tz)
    except (TypeError, ValueError):
        return None


def period(items: list[dict[str, Any]], warmest: bool) -> dict[str, Any] | None:
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


def forecast_days(today: date, tz: tzinfo, daily: list[dict[str, Any]] | None,
                  hourly: list[dict[str, Any]] | None, twice_daily: list[dict[str, Any]] | None,
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
    return [short(value, 31) for value in (values or []) if short(value, 31)][:MAX_NAMES]


def make_snapshot(states: dict[str, Any], options: dict[str, Any],
                  forecast: list[dict[str, Any]] | None = None, *,
                  hourly: list[dict[str, Any]] | None = None,
                  twice_daily: list[dict[str, Any]] | None = None,
                  today: date | None = None, tz: tzinfo | None = None,
                  sun: dict[date, tuple[datetime | None, datetime | None]] | None = None,
                  opening_names: list[str] | None = None,
                  light_names: list[str] | None = None) -> dict[str, Any]:
    """Build one complete state update; absent or unavailable inputs stay unknown.

    forecast is the daily list from weather.get_forecasts; its first entry is today.
    The per-day forecast is sent only when today and tz (HA's local date and zone) are given.
    """
    # One numeric sensor (e.g. sensor.number_open_contacts) already counts open contacts.
    openings_id = options.get("openings")
    openings_entity = states.get(openings_id) if isinstance(openings_id, str) else None
    openings, openings_known = 0, False
    try:
        value = float(openings_entity.state) if openings_entity is not None else None
    except (TypeError, ValueError):
        value = None
    if value is not None and value >= 0 and value.is_integer():
        openings, openings_known = min(int(value), 99), True

    alarm_entity = states.get(options.get("alarm", ""))
    valid_alarm = {
        "disarmed", "armed_home", "armed_away", "armed_night", "armed_vacation",
        "armed_custom_bypass", "arming", "pending", "triggered",
    }
    alarm = alarm_entity.state if alarm_entity and alarm_entity.state in valid_alarm else "unknown"

    weather_entity = states.get(options.get("weather", ""))
    weather = ""
    temperature = ""
    if weather_entity and weather_entity.state not in ("unknown", "unavailable"):
        weather = short(weather_entity.state, 31)
        value = weather_entity.attributes.get("temperature")
        unit = weather_entity.attributes.get("temperature_unit", "°C")
        if value is not None:
            temperature = short(f"{value}{unit}", 15)

    first = forecast[0] if forecast and isinstance(forecast[0], dict) else {}
    condition = first.get("condition") or (weather_entity.state if weather_entity else "")
    condition = condition if condition in WEATHER_CONDITIONS else ""
    temp_high = whole_degrees(first.get("temperature")) if condition else None
    temp_low = whole_degrees(first.get("templow")) if condition else None

    # Counted from the names, which leave out the projector's own light: a
    # generic "lights on" helper would count the projector whenever it is on.
    lights = min(len(light_names), 99) if light_names is not None else None

    feels = humidity = None
    wind = ""
    if weather_entity and weather_entity.state not in ("unknown", "unavailable"):
        attributes = weather_entity.attributes
        feels = whole_degrees(attributes.get("apparent_temperature"))
        humidity = percent(attributes.get("humidity"))
        speed = number(attributes.get("wind_speed"))
        if speed is not None:
            wind = short(f"{speed:.0f} {attributes.get('wind_speed_unit', 'km/h')}", 15)
    outdoor = tenths(states.get(options.get("outdoor", "")))
    if not outdoor and weather_entity and weather_entity.state not in ("unknown", "unavailable"):
        value = number(weather_entity.attributes.get("temperature"))
        outdoor = "" if value is None else short(f"{value:.1f}°", 11)

    extras = []
    for key in ("entity1", "entity2", "entity3", "entity4"):
        entity = states.get(options.get(key, ""))
        if entity and entity.state not in ("unknown", "unavailable"):
            label = entity.attributes.get("friendly_name", key)
            unit = entity.attributes.get("unit_of_measurement", "")
            extras.append(short(f"{label}: {entity.state}{unit}", 31))
        else:
            extras.append("")
    return {
        "openings": openings,
        "openings_known": openings_known,
        "alarm": alarm,
        "weather": weather,
        "temperature": temperature,
        "condition": condition,
        "temp_high": temp_high,
        "temp_low": temp_low,
        "extras": extras,
        "lights": lights or 0,
        "lights_known": lights is not None,
        "opening_names": names(opening_names),
        "light_names": names(light_names),
        "indoor": tenths(states.get(options.get("indoor", ""))),
        "outdoor": outdoor,
        "temp_now": whole_degrees(weather_entity.attributes.get("temperature"))
        if weather and weather_entity else None,
        "feels": f"{feels}°" if feels is not None else "",
        "humidity": humidity,
        "wind": wind,
        "days": forecast_days(today, tz, forecast, hourly, twice_daily, sun,
                              (summary(states, options, "summary_today"),
                               summary(states, options, "summary_tomorrow")))
        if today is not None and tz is not None else [],
    }


def summary(states: dict[str, Any], options: dict[str, Any], key: str) -> str:
    entity = states.get(options.get(key, ""))
    if entity is None or entity.state in ("unknown", "unavailable"):
        return ""
    return str(entity.state)
