"""Device and house status sensors."""

from homeassistant.components.sensor import SensorEntity

from .const import DOMAIN
from .entity import ProjectorEntity


async def async_setup_entry(hass, entry, async_add_entities):
    coordinator = hass.data[DOMAIN][entry.entry_id]
    async_add_entities([
        ProjectorSensor(coordinator, "openings", "Aperture", "openings"),
        ProjectorSensor(coordinator, "alarm", "Allarme", "alarm"),
        ProjectorSensor(coordinator, "uptime", "Tempo di attività", "uptime_seconds"),
        ProjectorSensor(coordinator, "heap", "Memoria libera", "free_heap"),
    ])


class ProjectorSensor(ProjectorEntity, SensorEntity):
    def __init__(self, coordinator, key, name, field):
        super().__init__(coordinator, key, name)
        self.field = field
        if key == "heap":
            self._attr_native_unit_of_measurement = "B"
        if key == "uptime":
            self._attr_native_unit_of_measurement = "s"

    @property
    def native_value(self):
        if self.field in ("openings", "alarm") and not self.coordinator.data.get("ha_fresh"):
            return None
        return self.coordinator.data.get(self.field)
