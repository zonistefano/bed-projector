"""Local HTTP client and state synchronization for Bed Projector."""

from __future__ import annotations

import asyncio
import logging
from datetime import timedelta
from typing import Any

from aiohttp import ClientError, ClientTimeout
from homeassistant.core import HomeAssistant
from homeassistant.helpers.aiohttp_client import async_get_clientsession
from homeassistant.helpers.update_coordinator import DataUpdateCoordinator, UpdateFailed

from .const import CONF_ALARM, CONF_EXTRAS, CONF_OPENINGS, CONF_TOKEN, CONF_WEATHER, DOMAIN
from .protocol import make_snapshot


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
        values = [options.get(key) for key in (CONF_OPENINGS, CONF_ALARM, CONF_WEATHER, *CONF_EXTRAS)]
        return [value for value in values if isinstance(value, str) and value]

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
            payload = make_snapshot(states, self.entry.options)
            try:
                await self.request("POST", "/api/v1/ha/state", payload)
            except UpdateFailed:
                # A later event or the periodic full snapshot will retry.
                pass

    async def async_stop(self) -> None:
        if self.push_task and not self.push_task.done():
            self.push_task.cancel()
            try:
                await self.push_task
            except asyncio.CancelledError:
                pass
