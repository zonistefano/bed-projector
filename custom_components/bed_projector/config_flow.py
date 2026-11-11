"""Configuration and entity mapping for Bed Projector."""

from __future__ import annotations

import asyncio

from aiohttp import ClientError, ClientTimeout
import voluptuous as vol
from homeassistant import config_entries
from homeassistant.core import callback
from homeassistant.exceptions import TemplateError
from homeassistant.helpers import selector
from homeassistant.helpers.aiohttp_client import async_get_clientsession
from homeassistant.helpers.service_info.zeroconf import ZeroconfServiceInfo
from homeassistant.helpers.template import Template

from .const import (CONF_ALARM, CONF_CALENDARS, CONF_CLIMATE_COOL, CONF_CLIMATE_HEAT, CONF_EXTRAS,
                    CONF_INDOOR, CONF_LIGHT_LIST, CONF_LIGHT_TEMPLATE, CONF_NEXT_ALARM,
                    CONF_NEXT_ALARM_ENABLED, CONF_OPENING_LIST, CONF_OPENING_TEMPLATE, CONF_OUTDOOR,
                    CONF_SUMMARY_TODAY, CONF_SUMMARY_TOMORROW, CONF_TOKEN, CONF_WEATHER, DOMAIN)

# Options form: key, entity domain or domains (None = any; "template" = template field), multiple
# entities.
OPTION_FIELDS = (
    (CONF_OPENING_LIST, "binary_sensor", True),
    (CONF_OPENING_TEMPLATE, "template", False),
    (CONF_LIGHT_LIST, "light", True), (CONF_LIGHT_TEMPLATE, "template", False),
    (CONF_ALARM, "alarm_control_panel", False), (CONF_WEATHER, "weather", False),
    (CONF_INDOOR, "sensor", False), (CONF_OUTDOOR, "sensor", False),
    (CONF_SUMMARY_TODAY, "sensor", False), (CONF_SUMMARY_TOMORROW, "sensor", False),
    (CONF_NEXT_ALARM, ["sensor", "input_datetime"], False),
    (CONF_NEXT_ALARM_ENABLED, ["input_boolean", "switch", "binary_sensor"], False),
    (CONF_CALENDARS, "calendar", True),
    (CONF_CLIMATE_HEAT, "climate", False), (CONF_CLIMATE_COOL, "climate", False),
    *((key, None, False) for key in CONF_EXTRAS),
)


class BedProjectorConfigFlow(config_entries.ConfigFlow, domain=DOMAIN):
    VERSION = 1

    def __init__(self) -> None:
        self.host = ""
        self.port = 80

    async def _check(self, host: str, port: int, token: str) -> tuple[str | None, str]:
        """(None, device id) when the device answers with this token, else (error key, "")."""
        try:
            session = async_get_clientsession(self.hass)
            async with session.get(
                f"http://{host}:{port}/api/status",
                headers={"Authorization": f"Bearer {token}"},
                timeout=ClientTimeout(total=8),
            ) as response:
                if response.status == 401:
                    return "invalid_auth", ""
                data = await response.json(content_type=None)
        except (ClientError, asyncio.TimeoutError, ValueError):
            return "cannot_connect", ""
        if response.status != 200 or not isinstance(data, dict) or not isinstance(data.get("id"), str):
            return "cannot_connect", ""
        return None, data["id"]

    async def _create(self, host: str, port: int, token: str) -> str | config_entries.ConfigFlowResult:
        """Create the entry, or return an error key for the form."""
        error, device_id = await self._check(host, port, token)
        if error:
            return error
        await self.async_set_unique_id(device_id, raise_on_progress=False)
        # The same projector reached at a new address keeps its entry.
        self._abort_if_unique_id_configured(updates={"host": host, "port": port})
        return self.async_create_entry(title=f"Bed Projector ({host})",
                                       data={"host": host, "port": port, CONF_TOKEN: token})

    async def async_step_user(self, user_input=None):
        errors = {}
        if user_input is not None:
            result = await self._create(user_input["host"].strip(), user_input["port"],
                                        user_input[CONF_TOKEN].strip())
            if not isinstance(result, str):
                return result
            errors["base"] = result
        defaults = user_input or {}
        schema = vol.Schema({
            vol.Required("host", default=defaults.get("host", "bed-projector.local")): str,
            vol.Required("port", default=defaults.get("port", 80)):
                vol.All(vol.Coerce(int), vol.Range(min=1, max=65535)),
            vol.Required(CONF_TOKEN): str,
        })
        return self.async_show_form(step_id="user", data_schema=schema, errors=errors)

    async def async_step_zeroconf(self, discovery_info: ZeroconfServiceInfo):
        device_id = discovery_info.properties.get("id")
        if not device_id or discovery_info.ip_address.version != 4:
            return self.async_abort(reason="not_supported")
        # The IP address, not bed-projector.local: mDNS names may not resolve inside HA's container.
        self.host = str(discovery_info.ip_address)
        self.port = discovery_info.port or 80
        await self.async_set_unique_id(device_id)
        self._abort_if_unique_id_configured(updates={"host": self.host, "port": self.port})
        self.context["title_placeholders"] = {"host": self.host}
        return await self.async_step_zeroconf_confirm()

    async def async_step_zeroconf_confirm(self, user_input=None):
        errors = {}
        if user_input is not None:
            result = await self._create(self.host, self.port, user_input[CONF_TOKEN].strip())
            if not isinstance(result, str):
                return result
            errors["base"] = result
        return self.async_show_form(step_id="zeroconf_confirm", errors=errors,
                                    data_schema=vol.Schema({vol.Required(CONF_TOKEN): str}),
                                    description_placeholders={"host": self.host})

    @staticmethod
    @callback
    def async_get_options_flow(config_entry):
        return BedProjectorOptionsFlow()


class BedProjectorOptionsFlow(config_entries.OptionsFlow):
    async def async_step_init(self, user_input=None):
        errors = {}
        if user_input is not None:
            for key in (CONF_OPENING_TEMPLATE, CONF_LIGHT_TEMPLATE):
                text = user_input.get(key, "").strip()
                if not text:
                    user_input.pop(key, None)
                    continue
                try:
                    Template(text, self.hass).ensure_valid()
                except TemplateError:
                    errors[key] = "invalid_template"
            if not errors:
                return self.async_create_entry(title="", data=user_input)
        current = user_input or self.config_entry.options
        schema_fields = {}
        for key, domain, multiple in OPTION_FIELDS:
            if domain == "template":
                # A suggested value, unlike a default, lets the user clear the field.
                marker = vol.Optional(key, description={"suggested_value": current.get(key)})
                schema_fields[marker] = selector.TemplateSelector()
                continue
            default = current.get(key)
            marker = vol.Optional(key, default=default) if default else vol.Optional(key)
            config = selector.EntitySelectorConfig(multiple=multiple)
            if domain:
                config = selector.EntitySelectorConfig(domain=domain, multiple=multiple)
            schema_fields[marker] = selector.EntitySelector(config)
        return self.async_show_form(step_id="init", data_schema=vol.Schema(schema_fields), errors=errors)
