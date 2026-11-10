"""Delay before a secondary page returns to the clock."""

from homeassistant.components.number import NumberEntity, NumberMode
from homeassistant.const import EntityCategory, UnitOfTime
from homeassistant.exceptions import HomeAssistantError

from .const import PAGE_TIMEOUT_RANGE
from .entity import ProjectorEntity


async def async_setup_entry(hass, entry, async_add_entities):
    async_add_entities([ProjectorPageTimeout(entry.runtime_data)])


class ProjectorPageTimeout(ProjectorEntity, NumberEntity):
    """0 keeps the selected page; otherwise PAGE_TIMEOUT_RANGE seconds."""

    _attr_native_min_value = 0
    _attr_native_max_value = PAGE_TIMEOUT_RANGE[1]
    _attr_native_step = 1
    _attr_native_unit_of_measurement = UnitOfTime.SECONDS
    _attr_mode = NumberMode.BOX
    _attr_entity_category = EntityCategory.CONFIG
    _attr_icon = "mdi:timer-refresh-outline"

    def __init__(self, coordinator):
        super().__init__(coordinator, "page_timeout", "Ritorno alla pagina Ora")

    @property
    def native_value(self):
        return self.coordinator.data.get("page_timeout")

    async def async_set_native_value(self, value):
        seconds = int(value)
        low, high = PAGE_TIMEOUT_RANGE
        if seconds != value or (seconds and not low <= seconds <= high):
            raise HomeAssistantError(f"Usa 0 (mai) oppure un valore da {low} a {high} secondi")
        await self.coordinator.command({"page_timeout": seconds})
