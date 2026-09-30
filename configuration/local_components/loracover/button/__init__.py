from logging import config

import esphome.codegen as cg
from esphome.components import button
from esphome.components import lora_client
import esphome.config_validation as cv
from esphome.const import (
    CONF_BATTERY_LEVEL,
    CONF_ID,
    CONF_BATTERY_VOLTAGE,
    DEVICE_CLASS_BATTERY,
    ENTITY_CATEGORY_DIAGNOSTIC,
    UNIT_PERCENT,
    UNIT_VOLT,
    ENTITY_CATEGORY_CONFIG
)


AUTO_LOAD = ["loracover", "blindsproto"]
CODEOWNERS = ["@buxtronix"]
DEPENDENCIES = ["lora_tracker", "lora_client"]



loracov_ns = cg.esphome_ns.namespace("loracov")


TriggerOtaButton = loracov_ns.class_("TriggerOtaButton", lora_client.LORAClientNode, button.Button, cg.Component)
DebugSysopButton = loracov_ns.class_("DebugSysopButton", lora_client.LORAClientNode, button.Button, cg.Component)



# Haier buttons
CONF_TRIGGER_OTA = "trigger_ota"
# Additional icons
ICON_OTA_UPDATE = "mdi:update"

# Two button kinds share this platform, discriminated by whether `op:` is
# given (2026-09-29) -- like switch/__init__.py's `slot` discriminator.
# Without `op:` you get the original OTA-trigger button (unchanged, so the
# two existing "Trigger OTA1/OTA2" entries in loradevices.yml keep working
# exactly as before); with it you get a runtime debug toggle
# (CMD_DEBUG_IDLE_CURRENT_ON/OFF, CMD_DEBUG_TRACE_CAPTURE_ARM -- see
# DebugFlags.h, node repo).
CONF_OP = "op"
ICON_DEBUG = "mdi:bug-outline"

# Maps the YAML string straight to the wire value from proto/blinds.proto's
# ClientOperation enum (CMD_DEBUG_IDLE_CURRENT_ON=7 etc). NOT the C++ symbol
# name: DebugSysopButton takes a raw int32_t (see debug_sysop_button.h for
# why blinds.pb-c.h cannot be included from a header this widely shared), so
# there is no C++ enumerand for codegen to reference by name here -- these
# three integers must be kept in sync with blinds.proto by hand.
_DEBUG_OPS = {
    "idle_current_on": 7,
    "idle_current_off": 8,
    "trace_capture_arm": 9,
}

TRIGGER_OTA_SCHEMA = button.button_schema(
    TriggerOtaButton,
    icon=ICON_OTA_UPDATE,
    entity_category=ENTITY_CATEGORY_CONFIG,
).extend(cv.COMPONENT_SCHEMA).extend(lora_client.LORA_CLIENT_SCHEMA)

DEBUG_SYSOP_SCHEMA = (
    button.button_schema(
        DebugSysopButton,
        icon=ICON_DEBUG,
        entity_category=ENTITY_CATEGORY_CONFIG,
    )
    .extend({cv.Required(CONF_OP): cv.enum(_DEBUG_OPS, lower=True)})
    .extend(cv.COMPONENT_SCHEMA)
    .extend(lora_client.LORA_CLIENT_SCHEMA)
)


def _pick_schema(config):
    if CONF_OP in config:
        return DEBUG_SYSOP_SCHEMA(config)
    return TRIGGER_OTA_SCHEMA(config)


CONFIG_SCHEMA = _pick_schema

async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await button.register_button(var, config)
    await lora_client.register_node(var, config)
    if CONF_OP in config:
        cg.add(var.set_op(config[CONF_OP]))
