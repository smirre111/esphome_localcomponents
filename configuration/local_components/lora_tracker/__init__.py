from __future__ import annotations

from dataclasses import dataclass
import logging

from esphome import automation
import esphome.codegen as cg
from esphome.components import cover
from esphome.components import esp32_ble
from esphome.components import time as time_
from esphome.components import homeassistant
import esphome.components.homeassistant as ha

from esphome.components.esp32 import add_idf_sdkconfig_option
import esphome.config_validation as cv
from esphome.const import (
    # CONF_PIN, 
    CONF_ID,
    # CONF_MAC_ADDRESS,
    # CONF_TRIGGER_ID,
)


from esphome.core import CORE, CoroPriority, coroutine_with_priority
from esphome.enum import StrEnum
from esphome.types import ConfigType

AUTO_LOAD = ["esp32_ble"]
CODEOWNERS = ["@buxtronix"]
AUTO_LOAD = ["lora_tracker", "blindsproto"]

# CONF_INVERT_POSITION = "invert_position"
CONF_LORA_TRACKER_ID = "lora_tracker_id"


# Tier-1 key sourcing (docs/mac-separation-implementation-plan.md): the fleet
# AES key as a YAML config value, normally resolved via !secret, rather than
# a literal in lora_client.cpp. Default matches the node's Kconfig default
# (BLINDS_LORA_FLEET_KEY) so an existing setup with no secrets.yaml entry for
# this keeps working unchanged.
CONF_FLEET_KEY = "fleet_key"

_LOGGER = logging.getLogger(__name__)


lora_tracker_ns = cg.esphome_ns.namespace("lora_tracker")
LORATracker = lora_tracker_ns.class_("LORATracker", cg.Component)

CONFIG_SCHEMA = (
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(LORATracker),
            cv.Optional(CONF_FLEET_KEY, default="LoRaHome"): cv.string_strict,
        }
    )
)


LORA_TRACKER_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_LORA_TRACKER_ID): cv.use_id(LORATracker),
    }
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    add_idf_sdkconfig_option("CONFIG_BT_ENABLED", True)
    # Tier-1 key sourcing: turn the YAML value into a compile-time #define,
    # so lora_client.cpp's component source never has to contain the real
    # secret — only a named, obviously-a-placeholder dev fallback.
    cg.add_define("LORA_FLEET_KEY", config[CONF_FLEET_KEY])


async def register_client(var: cg.SafeExpType, config: ConfigType) -> cg.SafeExpType:
    paren = await cg.get_variable(config[CONF_LORA_TRACKER_ID])
    cg.add(paren.register_client(var))
    return var

