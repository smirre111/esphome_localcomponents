import esphome.codegen as cg
from esphome.components import text_sensor
from esphome.components import lora_client
import esphome.config_validation as cv
from esphome.const import ENTITY_CATEGORY_DIAGNOSTIC

# "Sleep Status": what the hub believes about the node's sleep, in words --
# "awake", "asleep until 05:00", "sleep unconfirmed" (or "auto mode"). Companion
# of the `asleep` binary sensor (loracover binary_sensor, type: asleep); both are
# published on change only. See lora_client/SleepModel.h.

AUTO_LOAD = ["loracover", "blindsproto"]
DEPENDENCIES = ["lora_tracker", "lora_client"]

CONFIG_SCHEMA = text_sensor.text_sensor_schema(
    entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
    icon="mdi:sleep",
).extend(lora_client.LORA_CLIENT_SCHEMA)


async def to_code(config):
    sens = await text_sensor.new_text_sensor(config)
    paren = await cg.get_variable(config[lora_client.CONF_LORA_CLIENT_ID])
    cg.add(paren.set_sleep_status_text_sensor(sens))
