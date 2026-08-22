import esphome.codegen as cg
from esphome.components import binary_sensor
from esphome.components import lora_client
import esphome.config_validation as cv
from esphome.const import (
    CONF_TYPE,
    DEVICE_CLASS_PROBLEM,
    ENTITY_CATEGORY_DIAGNOSTIC,
)

# Two diagnostics share this platform, selected by `type:`:
#
#   command_failed  (F-4, default)  — a cover command was not acknowledged by
#                   the node after all retransmissions; cleared on the next
#                   successful (acked) command.
#   schedule_pending (P4b)          — the hub holds a schedule version the node
#                   has not applied yet. Expected to be briefly true after an
#                   edit; persistently true means pushes are not landing.
#
# `type:` defaults to command_failed so existing configurations keep working
# unchanged.

AUTO_LOAD = ["loracover", "blindsproto"]
CODEOWNERS = ["@buxtronix"]
DEPENDENCIES = ["lora_tracker", "lora_client"]

TYPE_COMMAND_FAILED = "command_failed"
TYPE_SCHEDULE_PENDING = "schedule_pending"
TYPES = [TYPE_COMMAND_FAILED, TYPE_SCHEDULE_PENDING]

CONFIG_SCHEMA = (
    binary_sensor.binary_sensor_schema(
        device_class=DEVICE_CLASS_PROBLEM,
        entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        icon="mdi:alert-circle",
    )
    .extend(
        {
            cv.Optional(CONF_TYPE, default=TYPE_COMMAND_FAILED): cv.one_of(
                *TYPES, lower=True
            ),
        }
    )
    .extend(lora_client.LORA_CLIENT_SCHEMA)
)


async def to_code(config):
    sens = await binary_sensor.new_binary_sensor(config)
    paren = await cg.get_variable(config[lora_client.CONF_LORA_CLIENT_ID])
    if config[CONF_TYPE] == TYPE_SCHEDULE_PENDING:
        cg.add(paren.set_schedule_pending_binary_sensor(sens))
    else:
        cg.add(paren.set_command_failed_binary_sensor(sens))
