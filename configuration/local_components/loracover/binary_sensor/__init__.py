import esphome.codegen as cg
from esphome.components import binary_sensor
from esphome.components import lora_client
import esphome.config_validation as cv
from esphome.const import (
    DEVICE_CLASS_PROBLEM,
    ENTITY_CATEGORY_DIAGNOSTIC,
)

# F-4: "command failed" indicator.  Set true when a cover command is not
# acknowledged by the node after all retransmissions, cleared on the next
# successful (acked) command.

AUTO_LOAD = ["loracover", "blindsproto"]
CODEOWNERS = ["@buxtronix"]
DEPENDENCIES = ["lora_tracker", "lora_client"]

CONFIG_SCHEMA = binary_sensor.binary_sensor_schema(
    device_class=DEVICE_CLASS_PROBLEM,
    entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
    icon="mdi:alert-circle",
).extend(lora_client.LORA_CLIENT_SCHEMA)


async def to_code(config):
    sens = await binary_sensor.new_binary_sensor(config)
    paren = await cg.get_variable(config[lora_client.CONF_LORA_CLIENT_ID])
    cg.add(paren.set_command_failed_binary_sensor(sens))
