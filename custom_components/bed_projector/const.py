"""Constants for the Bed Projector Home Assistant integration."""

DOMAIN = "bed_projector"
PLATFORMS = ["light", "select", "button", "sensor", "number"]
CONF_TOKEN = "token"
CONF_ALARM = "alarm"
CONF_WEATHER = "weather"
CONF_EXTRAS = ("entity1", "entity2", "entity3", "entity4")
CONF_OPENING_LIST = "opening_list"
CONF_LIGHT_LIST = "light_list"
# Templates that render the names to show; when set they replace the entity lists.
CONF_OPENING_TEMPLATE = "opening_template"
CONF_LIGHT_TEMPLATE = "light_template"
CONF_INDOOR = "indoor"
CONF_OUTDOOR = "outdoor"
CONF_SUMMARY_TODAY = "summary_today"
CONF_SUMMARY_TOMORROW = "summary_tomorrow"
# Timestamp sensor of the next phone alarm, e.g. the Companion App's next_alarm.
CONF_NEXT_ALARM = "next_alarm"
CONF_CALENDARS = "calendars"
# Bedroom climate entities: the radiators in winter, the air conditioner in summer.
CONF_CLIMATE_HEAT = "climate_heat"
CONF_CLIMATE_COOL = "climate_cool"
# Single entities forwarded to the display, in options form order.
CONF_SINGLE = (CONF_ALARM, CONF_WEATHER, CONF_INDOOR, CONF_OUTDOOR,
               CONF_SUMMARY_TODAY, CONF_SUMMARY_TOMORROW, CONF_NEXT_ALARM,
               CONF_CLIMATE_HEAT, CONF_CLIMATE_COOL, *CONF_EXTRAS)
# Contact sensor classes counted as doors and windows when no list is configured.
OPENING_CLASSES = ("door", "window", "garage_door", "opening")
# Page timeout accepted by the firmware: 0 (never) or this range in seconds.
PAGE_TIMEOUT_RANGE = (5, 3600)
# Message icons the firmware draws (main/projector.c message_icons) and duration in seconds.
MESSAGE_ICONS = ("bell", "information", "alert", "washing-machine", "package-variant", "door-open",
                 "lightbulb-on", "phone", "alarm", "calendar")
MESSAGE_DURATION_RANGE = (5, 3600)
MESSAGE_DURATION_DEFAULT = 30
# Which climate entity the thermostat page shows: chosen automatically, the radiators or the
# air conditioner.
SEASON_AUTO = "auto"
SEASON_WINTER = "winter"
SEASON_SUMMER = "summer"
SEASONS = (SEASON_AUTO, SEASON_WINTER, SEASON_SUMMER)
