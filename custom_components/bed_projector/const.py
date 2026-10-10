"""Constants for the Bed Projector Home Assistant integration."""

DOMAIN = "bed_projector"
PLATFORMS = ["light", "select", "button", "sensor", "number"]
CONF_TOKEN = "token"
CONF_OPENINGS = "openings"
CONF_ALARM = "alarm"
CONF_WEATHER = "weather"
CONF_EXTRAS = ("entity1", "entity2", "entity3", "entity4")
CONF_OPENING_LIST = "opening_list"
CONF_LIGHT_LIST = "light_list"
CONF_INDOOR = "indoor"
CONF_OUTDOOR = "outdoor"
CONF_SUMMARY_TODAY = "summary_today"
CONF_SUMMARY_TOMORROW = "summary_tomorrow"
# Single entities forwarded to the display, in options form order.
CONF_SINGLE = (CONF_OPENINGS, CONF_ALARM, CONF_WEATHER, CONF_INDOOR, CONF_OUTDOOR,
               CONF_SUMMARY_TODAY, CONF_SUMMARY_TOMORROW, *CONF_EXTRAS)
# Contact sensor classes counted as doors and windows when no list is configured.
OPENING_CLASSES = ("door", "window", "garage_door", "opening")
