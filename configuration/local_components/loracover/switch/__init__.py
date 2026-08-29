import esphome.codegen as cg
from esphome.components import switch
from esphome.components import lora_client
import esphome.config_validation as cv
from esphome.const import (
    CONF_ID,
    ENTITY_CATEGORY_CONFIG,
)

# P4b: Home Assistant control of automatic (scheduled) mode.
#
# The switch expresses INTENT. The node still refuses auto mode without a valid
# clock and a schedule that can fire, and a physical button press flips it back
# to interactive on its own — so the published state is corrected from what the
# node actually reports in its wake beacon.

AUTO_LOAD = ["loracover", "blindsproto"]
CODEOWNERS = ["@buxtronix"]
DEPENDENCIES = ["lora_tracker", "lora_client"]

loracov_ns = cg.esphome_ns.namespace("loracov")

AutoModeSwitch = loracov_ns.class_("AutoModeSwitch", switch.Switch, cg.Component)
ScheduleEnableSwitch = loracov_ns.class_(
    "ScheduleEnableSwitch", switch.Switch, cg.Component
)

CONF_SLOT = "slot"

# Two switch kinds share this platform, discriminated by whether `slot` is
# given: without it you get the auto-mode switch (as before), with it you get a
# per-slot enable. Keeping them in one platform avoids a second directory for
# ~20 lines of boilerplate.
AUTO_MODE_SCHEMA = (
    switch.switch_schema(
        AutoModeSwitch,
        icon="mdi:calendar-clock",
        entity_category=ENTITY_CATEGORY_CONFIG,
    )
    .extend(cv.COMPONENT_SCHEMA)
    .extend(lora_client.LORA_CLIENT_SCHEMA)
)

SLOT_ENABLE_SCHEMA = (
    switch.switch_schema(
        ScheduleEnableSwitch,
        icon="mdi:calendar-check",
        entity_category=ENTITY_CATEGORY_CONFIG,
    )
    .extend({cv.Required(CONF_SLOT): cv.int_range(min=0, max=7)})
    .extend(cv.COMPONENT_SCHEMA)
    .extend(lora_client.LORA_CLIENT_SCHEMA)
)


def _pick_schema(config):
    if CONF_SLOT in config:
        return SLOT_ENABLE_SCHEMA(config)
    return AUTO_MODE_SCHEMA(config)


CONFIG_SCHEMA = _pick_schema


async def to_code(config):
    var = await switch.new_switch(config)
    await cg.register_component(var, config)
    paren = await cg.get_variable(config[lora_client.CONF_LORA_CLIENT_ID])
    cg.add(var.set_lora_parent(paren))
    if CONF_SLOT in config:
        cg.add(var.set_slot(config[CONF_SLOT]))
    else:
        cg.add(paren.set_auto_mode_switch(var))
