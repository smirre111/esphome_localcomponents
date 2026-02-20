from __future__ import annotations

from dataclasses import dataclass
import logging

from esphome import automation
import esphome.codegen as cg
# from esphome.components import cover
from esphome.components import lora_tracker
from esphome.components import esp32_ble
from esphome.components import time as time_
from esphome.components import homeassistant
import esphome.components.homeassistant as ha

from esphome.components.esp32 import add_idf_sdkconfig_option
import esphome.config_validation as cv
from esphome.const import (
    CONF_NAME, 
    CONF_ID,
    CONF_MAC_ADDRESS,
    CONF_TRIGGER_ID,
)


from esphome.core import CORE, CoroPriority, coroutine_with_priority
from esphome.enum import StrEnum
from esphome.types import ConfigType
from esphome.core.entity_helpers import entity_duplicate_validator, setup_entity

# AUTO_LOAD = ["esp32_ble"]
CODEOWNERS = ["@buxtronix"]
AUTO_LOAD = ["lora_tracker", "blindsproto"]
DEPENDENCIES = ["lora_tracker", "time"]

# CONF_INVERT_POSITION = "invert_position"
# CONF_LORA_TRACKER_ID = "lora_tracker_id"
CONF_LORA_LISTENER_ID = "lora_listener_id"
CONF_LORA_CLIENT_ID = "lora_client_id"
CONF_LORA_CLIENT_NODE_ID = "lora_client_node_id"
CONF_ON_SLEEP_START = "on_sleep_start"

CONF_SHORT_ADDRESS = "short_address"
CONF_SUBNET_ADDRESS = "subnet_address"
CONF_SLEEP_DURATION = "sleep_duration"
CONF_TIME_ID = "time_id"  # New config key for time component

_LOGGER = logging.getLogger(__name__)


# Enum for LORA features
class LORAFeatures(StrEnum):
    LORA_DEVICE = "LORA_DEVICE"

# Dataclass for registration counts
@dataclass
class RegistrationCounts:
    listeners: int = 0
    clients: int = 0


lora_tracker_ns = cg.esphome_ns.namespace("lora_tracker")
# LORATracker = lora_tracker_ns.class_("LORATracker", cg.Component)


LORAListener = lora_tracker_ns.class_("LORAListener", cg.EntityBase,cg.Component)
LORAListenerConstRef = LORAListener.operator("ref").operator("const")


LORAClient = lora_tracker_ns.class_("LORAClient",  LORAListener, cg.EntityBase, cg.Component)
LORAClientNode = lora_tracker_ns.class_("LORAClientNode")


HomeassistantTime = ha.homeassistant_ns.class_("HomeassistantTime", time_.RealTimeClock)



# Triggers
SleepTrigger = lora_tracker_ns.class_(
    "SleepTrigger", automation.Trigger.template(LORAListenerConstRef)
)

SleepAction = lora_tracker_ns.class_("SleepAction", automation.Action)




TIME_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_TIME_ID): cv.use_id(HomeassistantTime),
    }
)

# CONFIG_SCHEMA = (
#     cover.cover_schema(LoraCoverComponent)
#     .extend(
#         {
#             cv.Required(CONF_MAC_ADDRESS): cv.mac_address,
#             cv.Optional(CONF_SHORT_ADDRESS, default=0x11): cv.int_range(min=0, max=0xFF),
#             cv.Optional(CONF_SUBNET_ADDRESS, default=0x11): cv.int_range(min=0, max=0xFF),
#             cv.Optional(CONF_PIN, default=8888): cv.int_range(min=0, max=0xFFFF),
#             cv.Optional(CONF_INVERT_POSITION, default=False): cv.boolean,
#             cv.Optional(CONF_OPEN_DURATION, default=False): cv.int_range(0, 120),
#             cv.Optional(CONF_CLOSE_DURATION, default=False): cv.int_range(0, 120),
#             cv.Optional(CONF_SLEEP_DURATION, default=False): cv.int_range(0, 864000),
#             cv.Required(CONF_TIME_ID): cv.use_id(time_.RealTimeClock),  # Require time component ID
#         }
#     )
#     .extend(lora_tracker.LORA_CLIENT_SCHEMA)
#     # .extend(TIME_SCHEMA)
#     .extend(cv.COMPONENT_SCHEMA)
# )

# CONFIG_SCHEMA = (
#     cv.Schema(
#         {
#             cv.GenerateID(): cv.declare_id(LORAListener),
#             cv.Optional(CONF_NAME): cv.string,
#             cv.Required(CONF_MAC_ADDRESS): cv.mac_address,
#             cv.Required(CONF_SHORT_ADDRESS): cv.int_range(min=0, max=0xFF),
#             cv.Required(CONF_SUBNET_ADDRESS): cv.int_range(min=0, max=0xFF),
#             cv.Required(CONF_TIME_ID): cv.use_id(time_.RealTimeClock),  # Require time component ID
#             cv.Optional(CONF_ON_SLEEP_START): automation.validate_automation(
#                 {
#                     cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(SleepTrigger),
#                     cv.Optional(CONF_MAC_ADDRESS): cv.ensure_list(cv.mac_address),
#                 }
#             ),
#         }
#     )
# )

# CONFIG_SCHEMA = (
#     cv.Schema(
#         {
#             cv.GenerateID(): cv.declare_id(LORAListener),
#             cv.Optional(CONF_NAME): cv.string,
#             cv.Required(CONF_MAC_ADDRESS): cv.mac_address,
#             cv.Required(CONF_SHORT_ADDRESS): cv.int_range(min=0, max=0xFF),
#             cv.Required(CONF_SUBNET_ADDRESS): cv.int_range(min=0, max=0xFF),
#             cv.Required(CONF_TIME_ID): cv.use_id(time_.RealTimeClock),  # Require time component ID
#             cv.Optional(CONF_ON_SLEEP_START): automation.validate_automation(
#                 {
#                     cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(SleepTrigger),
#                     cv.Optional(CONF_MAC_ADDRESS): cv.ensure_list(cv.mac_address),
#                 }
#             ),
#         }
#     )
# )

# # Triggers
# SleepTrigger = lora_tracker_ns.class_(
#     "SleepTrigger", automation.Trigger.template(LORAListenerConstRef)
# )

# SleepAction = lora_tracker_ns.class_("SleepAction", automation.Action)




# CONFIG_SCHEMA = (
#     cv.Schema(
#         {
#             cv.GenerateID(): cv.declare_id(LORATracker),
#             cv.Optional(CONF_ON_SLEEP_START): automation.validate_automation(
#                 {
#                     cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(SleepTrigger),
#                     cv.Optional(CONF_MAC_ADDRESS): cv.ensure_list(cv.mac_address),
#                 }
#             ),
#         }
#     )
# )






# CONFIG_SCHEMA = (
#     cv.Schema(
#         {
#             cv.GenerateID(): cv.declare_id(LORAListener),
#             cv.Required(CONF_MAC_ADDRESS): cv.mac_address,
#             cv.Optional(CONF_SHORT_ADDRESS, default=0x11): cv.int_range(min=0, max=0xFF),
#             cv.Optional(CONF_SUBNET_ADDRESS, default=0x11): cv.int_range(min=0, max=0xFF),
#             cv.Optional(CONF_PIN, default=8888): cv.int_range(min=0, max=0xFFFF),
#             cv.Optional(CONF_ON_SLEEP_START): automation.validate_automation(
#                 {
#                     cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(SleepTrigger),
#                     cv.Optional(CONF_MAC_ADDRESS): cv.ensure_list(cv.mac_address),
#                 }
#             ),
#         }
#     )

# )

# CONFIG_SCHEMA = (
#     cv.Schema(
#         {
#             cv.GenerateID(): cv.declare_id(LORAClient),
#             cv.Required(CONF_MAC_ADDRESS): cv.mac_address,
#             cv.Optional(CONF_SHORT_ADDRESS, default=0x11): cv.int_range(min=0, max=0xFF),
#             cv.Optional(CONF_SUBNET_ADDRESS, default=0x11): cv.int_range(min=0, max=0xFF),
#             cv.Optional(CONF_PIN, default=8888): cv.int_range(min=0, max=0xFFFF),
#             cv.Optional(CONF_ON_SLEEP_START): automation.validate_automation(
#                 {
#                     cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(SleepTrigger),
#                     cv.Optional(CONF_MAC_ADDRESS): cv.ensure_list(cv.mac_address),
#                 }
#             ),
#         }
#     )

# )

# CONFIG_SCHEMA = cv.All(
#     cv.Schema(
#         {
#             cv.GenerateID(): cv.declare_id(LORAClient),
#             cv.Optional(CONF_NAME): cv.string,

#             cv.Required(CONF_MAC_ADDRESS): cv.mac_address,
#             cv.Required(CONF_SHORT_ADDRESS): cv.int_range(min=0, max=0xFF),
#             cv.Required(CONF_SUBNET_ADDRESS): cv.int_range(min=0, max=0xFF),
#             cv.Required(CONF_TIME_ID): cv.use_id(time_.RealTimeClock),  # Require time component ID
#             cv.Optional(CONF_SLEEP_DURATION, default=False): cv.int_range(0, 864000),
            
#             cv.Optional(CONF_ON_SLEEP_START): automation.validate_automation(
#                 {
#                     cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(SleepTrigger),
#                     cv.Optional(CONF_MAC_ADDRESS): cv.ensure_list(cv.mac_address),
#                 }
#             ),
#         }

#     )
#     # .extend(TIME_SCHEMA)
#     .extend(lora_tracker.LORA_TRACKER_SCHEMA)
#     .extend(cv.COMPONENT_SCHEMA)
# )

CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(LORAClient),
            # cv.Optional(CONF_NAME): cv.string,

            cv.Required(CONF_MAC_ADDRESS): cv.mac_address,
            cv.Required(CONF_SHORT_ADDRESS): cv.int_range(min=0, max=0xFF),
            cv.Required(CONF_SUBNET_ADDRESS): cv.int_range(min=0, max=0xFF),
            cv.Required(CONF_TIME_ID): cv.use_id(time_.RealTimeClock),  # Require time component ID
            cv.Optional(CONF_SLEEP_DURATION, default=False): cv.int_range(0, 864000),
            
            cv.Optional(CONF_ON_SLEEP_START): automation.validate_automation(
                {
                    cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(SleepTrigger),
                    cv.Optional(CONF_MAC_ADDRESS): cv.ensure_list(cv.mac_address),
                }
            ),
        }

    )
    .extend(cv.ENTITY_BASE_SCHEMA)
    .extend(cv.MQTT_COMMAND_COMPONENT_SCHEMA)
    # .extend(TIME_SCHEMA)
    .extend(lora_tracker.LORA_TRACKER_SCHEMA)
    .extend(cv.COMPONENT_SCHEMA)
)

# LORA_TRACKER_SCHEMA = cv.Schema(
#     {
#         cv.GenerateID(CONF_LORA_TRACKER_ID): cv.use_id(LORATracker),
#     }
# )

LORA_LISTENER_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_LORA_LISTENER_ID): cv.use_id(LORAListener),
    }
)

LORA_CLIENT_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_LORA_CLIENT_ID): cv.use_id(LORAClient),
    }
)

LORA_CLIENT_NODE_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_LORA_CLIENT_NODE_ID): cv.use_id(LORAClientNode),
    }
)

SLEEP_ACTION_SCHEMA = automation.maybe_simple_id(
    {
        cv.Required(CONF_ID): cv.use_id(LORAListener),
        # cv.Required(CONF_STATE): cv.templatable(cv.boolean),
    }
)


SLEEP_CONTROL_ACTION_SCHEMA = automation.maybe_simple_id(
    {
        cv.Required(CONF_ID): cv.use_id(LORAListener),
        # cv.Required(CONF_STATE): cv.templatable(cv.boolean),
    }
)


@automation.register_action(
    "lora_tracker.sleep_control", SleepAction, SLEEP_CONTROL_ACTION_SCHEMA
)
async def sleep_control_to_code(config, action_id, template_arg, args):
    paren = await cg.get_variable(config[CONF_ID])
    var = cg.new_Pvariable(action_id, template_arg, paren)
    # template_ = await cg.templatable(config[CONF_STATE], args, bool)
    # cg.add(var.set_state(template_))
    return var

@automation.register_action("loracover.on_sleep_start", SleepAction, SLEEP_ACTION_SCHEMA)
async def tracker_sleep_start_to_code(config, action_id, template_arg, args):
    paren = await cg.get_variable(config[CONF_ID])
    return cg.new_Pvariable(action_id, template_arg, paren)

# CORE.data keys for state management
LORA_TRACKER_REQUIRED_FEATURES_KEY = "lora_tracker_required_features"
LORA_TRACKER_REGISTRATION_COUNTS_KEY = "lora_tracker_registration_counts"


def _get_required_features() -> set[LORAFeatures]:
    """Get the set of required LORA features from CORE.data."""
    return CORE.data.setdefault(LORA_TRACKER_REQUIRED_FEATURES_KEY, set())


def _get_registration_counts() -> RegistrationCounts:
    """Get the registration counts from CORE.data."""
    return CORE.data.setdefault(
        LORA_TRACKER_REGISTRATION_COUNTS_KEY, RegistrationCounts()
    )


def register_lora_features(features: set[LORAFeatures]) -> None:
    """Register LORA features that a component needs.

    Args:
        features: Set of LORAFeatures enum members
    """
    _get_required_features().update(features)







async def to_code(config):

    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await setup_entity(var, config, "lora_client")
    add_idf_sdkconfig_option("CONFIG_BT_ENABLED", True)

    # cg.add(var.set_pin(config[CONF_PIN]))
    cg.add(var.set_short_address(config[CONF_SHORT_ADDRESS]))
    cg.add(var.set_subnet_address(config[CONF_SUBNET_ADDRESS]))
    cg.add(var.set_address(config[CONF_MAC_ADDRESS].as_hex))

    # cg.add(var.set_invert_position(config[CONF_INVERT_POSITION]))
    # cg.add(var.set_open_duration(config[CONF_OPEN_DURATION]))
    # cg.add(var.set_close_duration(config[CONF_CLOSE_DURATION]))
    cg.add(var.set_sleep_duration(config[CONF_SLEEP_DURATION]))

    # Get the time component variable and set it
    timeInstance = await cg.get_variable(config[CONF_TIME_ID])
    cg.add(var.set_time(timeInstance))


    # Register LORA listener feature if any of the automation triggers are used
    if (config.get(CONF_ON_SLEEP_START) ):
        register_lora_features({LORAFeatures.LORA_DEVICE})

    registration_counts = _get_registration_counts()

    # await cg.register_component(var, config)
    await lora_tracker.register_client(var, config)

    for conf in config.get(CONF_ON_SLEEP_START, []):
        registration_counts.listeners += 1
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        if CONF_MAC_ADDRESS in conf:
            addr_list = [it.as_hex for it in conf[CONF_MAC_ADDRESS]]
            cg.add(trigger.set_addresses(addr_list))
        await automation.build_automation(trigger, [(LORAListenerConstRef, "x")], conf)


async def register_node(var: cg.SafeExpType, config: ConfigType) -> cg.SafeExpType:
    # register_lora_features({LORAFeatures.LORA_DEVICE})
    # _get_registration_counts().clients += 1
    paren = await cg.get_variable(config[CONF_LORA_CLIENT_ID])
    cg.add(paren.register_lora_node(var))
    return var














