"""Local HTTP client and state synchronization for Bed Projector."""

from __future__ import annotations

import asyncio
import logging
import time
from datetime import timedelta
from typing import Any

from aiohttp import ClientError, ClientTimeout
from homeassistant.const import SUN_EVENT_SUNRISE, SUN_EVENT_SUNSET
from homeassistant.core import HomeAssistant
from homeassistant.exceptions import HomeAssistantError
from homeassistant.helpers import entity_registry as er
from homeassistant.helpers.aiohttp_client import async_get_clientsession
from homeassistant.helpers.sun import get_astral_event_date
from homeassistant.helpers.update_coordinator import DataUpdateCoordinator, UpdateFailed
from homeassistant.util import dt as dt_util

from .const import (CONF_LIGHT_LIST, CONF_OPENING_LIST, CONF_SINGLE, CONF_TOKEN, CONF_WEATHER,
                    DOMAIN, OPENING_CLASSES)
from .protocol import FORECAST_DAYS, make_snapshot

# Forecasts change slowly; state pushes happen on every watched change.
FORECAST_TTL = 600


class ProjectorCoordinator(DataUpdateCoordinator[dict[str, Any]]):
    """Poll device status and push selected HA entities to the display."""

    def __init__(self, hass: HomeAssistant, entry) -> None:
        super().__init__(hass, logger=logging.getLogger(__name__),
                         name=DOMAIN, update_interval=timedelta(seconds=30))
        self.entry = entry
        self.base = f"http://{entry.data['host']}:{entry.data.get('port', 80)}"
        self.headers = {"Authorization": f"Bearer {entry.data[CONF_TOKEN]}"}
        self.push_task: asyncio.Task | None = None
        self.push_dirty = False
        self.forecasts: dict[str, list[dict[str, Any]] | None] = {}
        self.forecasts_at = 0.0

    async def request(self, method: str, path: str, payload: dict | None = None) -> dict:
        session = async_get_clientsession(self.hass)
        try:
            async with session.request(method, self.base + path, headers=self.headers,
                                       json=payload, timeout=ClientTimeout(total=8)) as response:
                data = await response.json()
                if response.status >= 400:
                    raise UpdateFailed(f"Device HTTP {response.status}: {data.get('error', '')}")
                return data
        except (ClientError, asyncio.TimeoutError, ValueError) as error:
            raise UpdateFailed(f"Device unreachable: {error}") from error

    async def _async_update_data(self) -> dict[str, Any]:
        status, pages = await asyncio.gather(
            self.request("GET", "/api/v1/status"),
            self.request("GET", "/api/v1/pages"),
        )
        if status.get("api_version") != 1:
            raise UpdateFailed("Unsupported projector API")
        status["pages"] = pages.get("pages", [])
        return status

    async def command(self, payload: dict) -> None:
        await self.request("POST", "/api/v1/display", payload)
        await self.async_request_refresh()

    def watched_entities(self) -> list[str]:
        options = self.entry.options
        values = [options.get(key) for key in CONF_SINGLE]
        values += [*self._list(CONF_OPENING_LIST), *self._list(CONF_LIGHT_LIST)]
        return list(dict.fromkeys(value for value in values if isinstance(value, str) and value))

    def _list(self, key: str) -> list[str]:
        value = self.entry.options.get(key)
        return [item for item in value if isinstance(item, str)] if isinstance(value, list) else []

    def _own_entities(self) -> set[str]:
        """Entities of this integration, such as the projector's own light."""
        registry = er.async_get(self.hass)
        return {entry.entity_id for entry in er.async_entries_for_config_entry(registry, self.entry.entry_id)}

    def listed(self, state) -> bool:
        """Whether a state belongs to the door/window or light name lists."""
        # Runs on every state change in HA: cheap checks first.
        if state is None or state.domain not in ("binary_sensor", "light") or \
                state.entity_id in self._own_entities():
            return False
        domain = state.domain
        configured = self._list(CONF_OPENING_LIST if domain == "binary_sensor" else CONF_LIGHT_LIST)
        if configured:
            return state.entity_id in configured
        if domain == "binary_sensor":
            return state.attributes.get("device_class") in OPENING_CLASSES
        return "entity_id" not in state.attributes  # Groups would repeat their members.

    def _names_on(self, domain: str) -> list[str]:
        """Friendly names of the listed entities that are on; without a configured
        list, every door/window contact or every single light but the projector."""
        return sorted(state.name for state in self.hass.states.async_all(domain)
                      if state.state == "on" and self.listed(state))

    def request_push(self) -> None:
        self.push_dirty = True
        if self.push_task is None or self.push_task.done():
            self.push_task = self.hass.async_create_task(self._push_loop())

    async def _push_loop(self) -> None:
        while self.push_dirty:
            self.push_dirty = False
            await asyncio.sleep(0.2)
            selected = self.watched_entities()
            states = {entity_id: self.hass.states.get(entity_id) for entity_id in selected}
            forecasts = await self._forecasts()
            now = dt_util.now()
            today = now.date()
            sun = {}
            for offset in range(FORECAST_DAYS):
                day = today + timedelta(days=offset)
                sun[day] = (get_astral_event_date(self.hass, SUN_EVENT_SUNRISE, day),
                            get_astral_event_date(self.hass, SUN_EVENT_SUNSET, day))
            payload = make_snapshot(
                states, self.entry.options, forecasts.get("daily"),
                hourly=forecasts.get("hourly"), twice_daily=forecasts.get("twice_daily"),
                today=today, tz=now.tzinfo, sun=sun,
                opening_names=self._names_on("binary_sensor"),
                light_names=self._names_on("light"))
            try:
                await self.request("POST", "/api/v1/ha/state", payload)
            except UpdateFailed:
                # A later event or the periodic full snapshot will retry.
                pass

    async def _forecasts(self) -> dict[str, list[dict[str, Any]] | None]:
        """Daily, hourly and twice-daily forecasts, cached for FORECAST_TTL seconds.

        Weather entities expose forecasts only through weather.get_forecasts.
        """
        entity_id = self.entry.options.get(CONF_WEATHER)
        if not isinstance(entity_id, str) or not entity_id:
            return {}
        if self.forecasts and time.monotonic() - self.forecasts_at < FORECAST_TTL:
            return self.forecasts
        forecasts: dict[str, list[dict[str, Any]] | None] = {}
        for kind in ("daily", "hourly", "twice_daily"):
            try:
                response = await self.hass.services.async_call(
                    "weather", "get_forecasts", {"entity_id": entity_id, "type": kind},
                    blocking=True, return_response=True)
            except HomeAssistantError:
                continue  # The provider does not support this forecast type.
            forecasts[kind] = (response or {}).get(entity_id, {}).get("forecast") or None
        if not forecasts.get("daily") and forecasts.get("twice_daily"):
            # Providers without a daily forecast: daytime entries stand in for days.
            forecasts["daily"] = [item for item in forecasts["twice_daily"] if item.get("is_daytime")]
        self.forecasts, self.forecasts_at = forecasts, time.monotonic()
        return forecasts

    async def async_stop(self) -> None:
        if self.push_task and not self.push_task.done():
            self.push_task.cancel()
            try:
                await self.push_task
            except asyncio.CancelledError:
                pass
