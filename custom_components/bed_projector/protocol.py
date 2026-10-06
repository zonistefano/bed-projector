"""Bounded, transport-independent normalization of Home Assistant states."""

from __future__ import annotations

from typing import Any


def short(value: Any, max_bytes: int) -> str:
    """Fit a string into the firmware's UTF-8 byte buffers."""
    return str(value or "").encode("utf-8")[:max_bytes].decode("utf-8", "ignore")


def make_snapshot(states: dict[str, Any], options: dict[str, Any]) -> dict[str, Any]:
    """Build one complete state update; absent or unavailable inputs stay unknown."""
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
        "extras": extras,
    }
