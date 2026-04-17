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



# Haier buttons
CONF_TRIGGER_OTA = "trigger_ota"
# Additional icons
ICON_OTA_UPDATE = "mdi:update"

# CONFIG_SCHEMA = (
#     cv.Schema(
#     {
#         cv.GenerateID(): cv.declare_id(TriggerOtaButton),
#         # cv.GenerateID(lora_client.CONF_LORA_CLIENT_ID): cv.use_id(lora_client.LORAClient),
#         cv.Optional(CONF_TRIGGER_OTA): button.button_schema(
#             TriggerOtaButton,
#             icon=ICON_OTA_UPDATE,
#             entity_category=ENTITY_CATEGORY_CONFIG,
#         ),
#     }
#     )
#     .extend(cv.COMPONENT_SCHEMA)
#     .extend(lora_client.LORA_CLIENT_SCHEMA)
# )

CONFIG_SCHEMA = button.button_schema(
    TriggerOtaButton,
    icon=ICON_OTA_UPDATE,
    entity_category=ENTITY_CATEGORY_CONFIG,
).extend(cv.COMPONENT_SCHEMA).extend(lora_client.LORA_CLIENT_SCHEMA)

async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await button.register_button(var, config)
    await lora_client.register_node(var, config)

    # for button_type in [CONF_TRIGGER_OTA]:
    #     if conf := config.get(button_type):
    #         btn = await button.new_button(conf)
    #         await cg.register_parented(btn, config[lora_client.CONF_LORA_CLIENT_ID])
