"""Page timing: return to the clock and automatic rotation."""

from __future__ import annotations

from homeassistant.components.number import NumberEntity, NumberMode
from homeassistant.const import EntityCategory, UnitOfTime
from homeassistant.exceptions import HomeAssistantError

from .const import CAROUSEL_RANGE, PAGE_TIMEOUT_RANGE
from .entity import ProjectorEntity


async def async_setup_entry(hass, entry, async_add_entities):
    coordinator = entry.runtime_data
    async_add_entities([
        ProjectorSeconds(coordinator, "page_timeout", "Ritorno alla pagina Ora", "mdi:timer-refresh-outline",
                         PAGE_TIMEOUT_RANGE, "mai"),
        ProjectorSeconds(coordinator, "carousel", "Rotazione pagine", "mdi:rotate-right",
                         CAROUSEL_RANGE, "spenta"),
    ])


class ProjectorSeconds(ProjectorEntity, NumberEntity):
    """A device setting in seconds: 0 turns it off, otherwise within a range."""

    _attr_native_min_value = 0
    _attr_native_step = 1
    _attr_native_unit_of_measurement = UnitOfTime.SECONDS
    _attr_mode = NumberMode.BOX
    _attr_entity_category = EntityCategory.CONFIG

    def __init__(self, coordinator, key: str, name: str, icon: str, valid: tuple[int, int],
                 zero: str) -> None:
        super().__init__(coordinator, key, name)
        self.key = key
        self.valid = valid
        self.zero = zero
        self._attr_icon = icon
        self._attr_native_max_value = valid[1]

    @property
    def native_value(self):
        return self.coordinator.data.get(self.key)

    async def async_set_native_value(self, value):
        seconds = int(value)
        low, high = self.valid
        if seconds != value or (seconds and not low <= seconds <= high):
            raise HomeAssistantError(f"Usa 0 ({self.zero}) oppure un valore da {low} a {high} secondi")
        await self.coordinator.command({self.key: seconds})
