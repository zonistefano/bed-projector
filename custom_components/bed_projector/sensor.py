"""Device diagnostic sensors."""

from __future__ import annotations

from dataclasses import dataclass

from homeassistant.components.sensor import (SensorDeviceClass, SensorEntity,
                                             SensorEntityDescription, SensorStateClass)
from homeassistant.const import EntityCategory, UnitOfInformation, UnitOfTime

from .entity import ProjectorEntity


@dataclass(frozen=True, kw_only=True)
class ProjectorSensorDescription(SensorEntityDescription):
    field: str


SENSORS = (
    ProjectorSensorDescription(key="uptime", field="uptime_seconds", name="Tempo di attività",
                               device_class=SensorDeviceClass.DURATION,
                               native_unit_of_measurement=UnitOfTime.SECONDS,
                               entity_category=EntityCategory.DIAGNOSTIC),
    ProjectorSensorDescription(key="heap", field="free_heap", name="Memoria libera",
                               device_class=SensorDeviceClass.DATA_SIZE,
                               native_unit_of_measurement=UnitOfInformation.BYTES,
                               state_class=SensorStateClass.MEASUREMENT,
                               entity_category=EntityCategory.DIAGNOSTIC),
)


async def async_setup_entry(hass, entry, async_add_entities):
    async_add_entities(ProjectorSensor(entry.runtime_data, description) for description in SENSORS)


class ProjectorSensor(ProjectorEntity, SensorEntity):
    entity_description: ProjectorSensorDescription

    def __init__(self, coordinator, description: ProjectorSensorDescription):
        super().__init__(coordinator, description.key, description.name)
        self.entity_description = description

    @property
    def native_value(self):
        return self.coordinator.data.get(self.entity_description.field)
