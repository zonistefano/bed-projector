"""Shared entity identity for the Bed Projector."""

from homeassistant.helpers.update_coordinator import CoordinatorEntity


class ProjectorEntity(CoordinatorEntity):
    _attr_has_entity_name = True

    def __init__(self, coordinator, key: str, name: str) -> None:
        super().__init__(coordinator)
        self._attr_unique_id = f"{coordinator.entry.entry_id}_{key}"
        self._attr_name = name
        self._attr_device_info = {
            "identifiers": {("bed_projector", coordinator.entry.entry_id)},
            "name": coordinator.entry.title,
            "manufacturer": "Bed Projector",
            "model": "ESP32-S3 projection clock",
        }
