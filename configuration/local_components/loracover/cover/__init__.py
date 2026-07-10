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
CONF_OPEN_SLACK = "open_slack"
CONF_CLOSE_SLACK = "close_slack"
CONF_SLEEP_DURATION = "sleep_duration"
CONF_BLIND_HEIGHT_MM = "blind_height_mm"
CONF_AXLE_DIAMETER_MM = "axle_diameter_mm"
CONF_BLIND_THICKNESS_MM = "blind_thickness_mm"
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

LORA_COVER_SCHEMA = (
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
            cv.Optional(CONF_OPEN_SLACK, default=0): cv.int_range(0, 60),
            cv.Optional(CONF_CLOSE_SLACK, default=0): cv.int_range(0, 60),
            cv.Optional(CONF_BLIND_HEIGHT_MM, default=2000.0): cv.float_range(min=1),
            cv.Optional(CONF_AXLE_DIAMETER_MM, default=60.0): cv.float_range(min=1),
            cv.Optional(CONF_BLIND_THICKNESS_MM, default=8.0): cv.float_range(min=0.1),
            # cv.Optional(CONF_SLEEP_DURATION, default=False): cv.int_range(0, 864000),
            # cv.Required(CONF_TIME_ID): cv.use_id(time_.RealTimeClock),  # Require time component ID
        }
    )
    .extend(lora_client.LORA_CLIENT_SCHEMA)
    # .extend(TIME_SCHEMA)
    .extend(cv.COMPONENT_SCHEMA)
)

CONFIG_SCHEMA = (
    cover.cover_schema(LoraCoverComponent)
    .extend(
        {
            cv.Optional(CONF_INVERT_POSITION, default=False): cv.boolean,
            cv.Optional(CONF_OPEN_DURATION, default=False): cv.int_range(0, 120),
            cv.Optional(CONF_CLOSE_DURATION, default=False): cv.int_range(0, 120),
            cv.Optional(CONF_OPEN_SLACK, default=0): cv.int_range(0, 60),
            cv.Optional(CONF_CLOSE_SLACK, default=0): cv.int_range(0, 60),
            cv.Optional(CONF_BLIND_HEIGHT_MM, default=2000.0): cv.float_range(min=1),
            cv.Optional(CONF_AXLE_DIAMETER_MM, default=60.0): cv.float_range(min=1),
            cv.Optional(CONF_BLIND_THICKNESS_MM, default=8.0): cv.float_range(min=0.1),
        }
    )
    .extend(lora_client.LORA_CLIENT_SCHEMA)
    .extend(cv.COMPONENT_SCHEMA)
)





async def to_code(config):
    var = await cover.new_cover(config)
    cg.add(var.set_invert_position(config[CONF_INVERT_POSITION]))
    cg.add(var.set_open_duration(config[CONF_OPEN_DURATION]))
    cg.add(var.set_close_duration(config[CONF_CLOSE_DURATION]))
    cg.add(var.set_open_slack(config[CONF_OPEN_SLACK]))
    cg.add(var.set_close_slack(config[CONF_CLOSE_SLACK]))
    cg.add(var.set_blind_height_mm(config[CONF_BLIND_HEIGHT_MM]))
    cg.add(var.set_axle_diameter_mm(config[CONF_AXLE_DIAMETER_MM]))
    cg.add(var.set_blind_thickness_mm(config[CONF_BLIND_THICKNESS_MM]))

    await cg.register_component(var, config)
    await lora_client.register_node(var, config)
