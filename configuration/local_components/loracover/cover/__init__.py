import esphome.codegen as cg
from esphome.components import cover
from esphome.components import time as time_
from esphome.components import homeassistant
import esphome.components.homeassistant as ha

from esphome.components import lora_client
import esphome.config_validation as cv
# from esphome.const import (CONF_PIN, CONF_MAC_ADDRESS)

CODEOWNERS = ["@buxtronix"]
DEPENDENCIES = ["lora_tracker", "lora_client"]
AUTO_LOAD = ["loracover", "blindsproto"]

CONF_INVERT_POSITION = "invert_position"
# CONF_SHORT_ADDRESS = "short_address"
# CONF_SUBNET_ADDRESS = "subnet_address"
CONF_OPEN_DURATION = "open_duration"
CONF_CLOSE_DURATION = "close_duration"
CONF_SLEEP_DURATION = "sleep_duration"
# CONF_TIME_ID = "time_id"  # New config key for time component


loracov_ns = cg.esphome_ns.namespace("loracov")
LoraCoverComponent = loracov_ns.class_(
    "LoraCoverComponent", cover.Cover, lora_client.LORAClientNode, cg.Component
)

# HomeassistantTime = ha.homeassistant_ns.class_("HomeassistantTime", time_.RealTimeClock)

# TIME_SCHEMA = cv.Schema(
#     {
#         cv.GenerateID(CONF_TIME_ID): cv.use_id(HomeassistantTime),
#     }
# )

CONFIG_SCHEMA = (
    cover.cover_schema(LoraCoverComponent)
    .extend(
        {
            # cv.Required(CONF_MAC_ADDRESS): cv.mac_address,
            # cv.Optional(CONF_SHORT_ADDRESS, default=0x11): cv.int_range(min=0, max=0xFF),
            # cv.Optional(CONF_SUBNET_ADDRESS, default=0x11): cv.int_range(min=0, max=0xFF),
            # cv.Optional(CONF_PIN, default=8888): cv.int_range(min=0, max=0xFFFF),
            cv.Optional(CONF_INVERT_POSITION, default=False): cv.boolean,
            cv.Optional(CONF_OPEN_DURATION, default=False): cv.int_range(0, 120),
            cv.Optional(CONF_CLOSE_DURATION, default=False): cv.int_range(0, 120),
            # cv.Optional(CONF_SLEEP_DURATION, default=False): cv.int_range(0, 864000),
            # cv.Required(CONF_TIME_ID): cv.use_id(time_.RealTimeClock),  # Require time component ID
        }
    )
    .extend(lora_client.LORA_CLIENT_SCHEMA)
    # .extend(TIME_SCHEMA)
    .extend(cv.COMPONENT_SCHEMA)
)





async def to_code(config):
    var = await cover.new_cover(config)
    # cg.add(var.set_pin(config[CONF_PIN]))
    # cg.add(var.set_short_address(config[CONF_SHORT_ADDRESS]))
    # cg.add(var.set_subnet_address(config[CONF_SUBNET_ADDRESS]))
    # cg.add(var.set_address(config[CONF_MAC_ADDRESS].as_hex))

    cg.add(var.set_invert_position(config[CONF_INVERT_POSITION]))
    cg.add(var.set_open_duration(config[CONF_OPEN_DURATION]))
    cg.add(var.set_close_duration(config[CONF_CLOSE_DURATION]))
    # cg.add(var.set_sleep_duration(config[CONF_SLEEP_DURATION]))

    # Get the time component variable and set it
    # timeInstance = await cg.get_variable(config[CONF_TIME_ID])
    # cg.add(var.set_time(timeInstance))

    # cg.add(var.set_time(cv.gethomeassistant.time.var))
    
    await cg.register_component(var, config)
    await lora_client.register_node(var, config)
