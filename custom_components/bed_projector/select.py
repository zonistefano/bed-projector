"""Select entities: the page shown and which thermostat the climate page shows."""

from __future__ import annotations

from homeassistant.components.select import SelectEntity
from homeassistant.const import EntityCategory
from homeassistant.exceptions import HomeAssistantError
from homeassistant.helpers.restore_state import RestoreEntity

from .const import CONF_CLIMATE_COOL, CONF_CLIMATE_HEAT, SEASON_AUTO, SEASON_SUMMER, SEASON_WINTER
from .entity import ProjectorEntity

SEASON_LABELS = {SEASON_AUTO: "Automatica", SEASON_WINTER: "Inverno (caloriferi)",
                 SEASON_SUMMER: "Estate (condizionatore)"}


async def async_setup_entry(hass, entry, async_add_entities):
    coordinator = entry.runtime_data
    entities: list[SelectEntity] = [ProjectorPage(coordinator)]
    # The season matters only when there are two thermostats to choose from.
    if entry.options.get(CONF_CLIMATE_HEAT) and entry.options.get(CONF_CLIMATE_COOL):
        entities.append(ThermostatSeason(coordinator))
    async_add_entities(entities)


class ProjectorPage(ProjectorEntity, SelectEntity):
    """Options are the page names shown on the web UI."""

    _attr_icon = "mdi:book-open-page-variant-outline"

    def __init__(self, coordinator):
        super().__init__(coordinator, "page", "Pagina")

    @property
    def options(self):
        return list(self.coordinator.page_labels().values())

    @property
    def current_option(self):
        return self.coordinator.page_labels().get(self.coordinator.data.get("page"))

    async def async_select_option(self, option):
        page = next((page_id for page_id, label in self.coordinator.page_labels().items()
                     if label == option), None)
        if page is None:
            raise HomeAssistantError(f"Pagina sconosciuta: {option}")
        await self.coordinator.command({"page": page})


class ThermostatSeason(ProjectorEntity, SelectEntity, RestoreEntity):
    """Radiators in winter, air conditioner in summer, or whichever is in use."""

    _attr_icon = "mdi:sun-snowflake"
    _attr_entity_category = EntityCategory.CONFIG
    _attr_options = list(SEASON_LABELS.values())

    def __init__(self, coordinator):
        super().__init__(coordinator, "thermostat_season", "Stagione termostato")

    async def async_added_to_hass(self) -> None:
        await super().async_added_to_hass()
        last = await self.async_get_last_state()
        season = next((key for key, label in SEASON_LABELS.items() if last and last.state == label), None)
        if season is not None and season != self.coordinator.season:
            self.coordinator.season = season
            self.coordinator.request_push()

    @property
    def current_option(self) -> str:
        return SEASON_LABELS[self.coordinator.season]

    async def async_select_option(self, option: str) -> None:
        season = next((key for key, label in SEASON_LABELS.items() if label == option), None)
        if season is None:
            raise HomeAssistantError(f"Stagione sconosciuta: {option}")
        self.coordinator.season = season
        self.async_write_ha_state()
        self.coordinator.request_push()
