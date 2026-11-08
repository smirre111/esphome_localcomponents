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

CONFIG_SCHEMA = (
    switch.switch_schema(
        AutoModeSwitch,
        icon="mdi:calendar-clock",
        entity_category=ENTITY_CATEGORY_CONFIG,
    )
    .extend(cv.COMPONENT_SCHEMA)
    .extend(lora_client.LORA_CLIENT_SCHEMA)
)


async def to_code(config):
    var = await switch.new_switch(config)
    await cg.register_component(var, config)
    paren = await cg.get_variable(config[lora_client.CONF_LORA_CLIENT_ID])
    cg.add(var.set_lora_parent(paren))
    cg.add(paren.set_auto_mode_switch(var))
