"""Configuration and entity mapping for Bed Projector."""

from __future__ import annotations

from aiohttp import ClientError, ClientTimeout
import voluptuous as vol
from homeassistant import config_entries
from homeassistant.core import callback
from homeassistant.helpers import selector
from homeassistant.helpers.aiohttp_client import async_get_clientsession

from .const import (CONF_ALARM, CONF_EXTRAS, CONF_INDOOR, CONF_LIGHT_LIST,
                    CONF_OPENING_LIST, CONF_OPENINGS, CONF_OUTDOOR, CONF_SUMMARY_TODAY,
                    CONF_SUMMARY_TOMORROW, CONF_TOKEN, CONF_WEATHER, DOMAIN)


class BedProjectorConfigFlow(config_entries.ConfigFlow, domain=DOMAIN):
    VERSION = 1

    async def async_step_user(self, user_input=None):
        errors = {}
        if user_input is not None:
            host = user_input["host"].strip()
            port = user_input["port"]
            token = user_input[CONF_TOKEN].strip()
            try:
                session = async_get_clientsession(self.hass)
                async with session.get(
                    f"http://{host}:{port}/api/v1/status",
                    headers={"Authorization": f"Bearer {token}"},
                    timeout=ClientTimeout(total=8),
                ) as response:
                    data = await response.json()
                    if response.status != 200 or data.get("api_version") != 1:
                        errors["base"] = "cannot_connect"
            except (ClientError, TimeoutError, ValueError):
                errors["base"] = "cannot_connect"
            if not errors:
                await self.async_set_unique_id(f"{host}:{port}")
                self._abort_if_unique_id_configured()
                return self.async_create_entry(title=f"Bed Projector ({host})",
                                               data={"host": host, "port": port, CONF_TOKEN: token})
        schema = vol.Schema({
            vol.Required("host", default="bed-projector.local"): str,
            vol.Required("port", default=80): vol.All(vol.Coerce(int), vol.Range(min=1, max=65535)),
            vol.Required(CONF_TOKEN): str,
        })
        return self.async_show_form(step_id="user", data_schema=schema, errors=errors)

    @staticmethod
    @callback
    def async_get_options_flow(config_entry):
        return BedProjectorOptionsFlow()


class BedProjectorOptionsFlow(config_entries.OptionsFlow):
    async def async_step_init(self, user_input=None):
        if user_input is not None:
            return self.async_create_entry(title="", data=user_input)
        current = self.config_entry.options
        schema_fields = {}
        fields = (
            (CONF_OPENINGS, "sensor", False), (CONF_OPENING_LIST, "binary_sensor", True),
            (CONF_LIGHT_LIST, "light", True),
            (CONF_ALARM, "alarm_control_panel", False), (CONF_WEATHER, "weather", False),
            (CONF_INDOOR, "sensor", False), (CONF_OUTDOOR, "sensor", False),
            (CONF_SUMMARY_TODAY, "sensor", False), (CONF_SUMMARY_TOMORROW, "sensor", False),
        )
        for key, domain, multiple in fields:
            # Older versions stored openings as a list of binary_sensors: drop that default.
            default = current.get(key) if isinstance(current.get(key), list if multiple else str) else None
            marker = vol.Optional(key, default=default) if default else vol.Optional(key)
            schema_fields[marker] = selector.EntitySelector(
                selector.EntitySelectorConfig(domain=domain, multiple=multiple))
        for key in CONF_EXTRAS:
            marker = vol.Optional(key, default=current[key]) if current.get(key) else vol.Optional(key)
            schema_fields[marker] = selector.EntitySelector(selector.EntitySelectorConfig())
        schema = vol.Schema(schema_fields)
        return self.async_show_form(step_id="init", data_schema=schema)
