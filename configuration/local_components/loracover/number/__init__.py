import esphome.codegen as cg
from esphome.components import number
from esphome.components import lora_client
import esphome.config_validation as cv
from esphome.const import ENTITY_CATEGORY_CONFIG, UNIT_PERCENT

# Target position for a slot whose action is `position`.
#
# This was in the original proposal and was missed in the first implementation,
# so choosing `position` in the action select had nowhere to enter a percentage.

AUTO_LOAD = ["loracover", "blindsproto"]
DEPENDENCIES = ["lora_tracker", "lora_client"]

CONF_SLOT = "slot"

loracov_ns = cg.esphome_ns.namespace("loracov")
ScheduleSlotPosition = loracov_ns.class_(
    "ScheduleSlotPosition", number.Number, cg.Component
)

CONFIG_SCHEMA = (
    number.number_schema(
        ScheduleSlotPosition,
        unit_of_measurement=UNIT_PERCENT,
        icon="mdi:percent",
        entity_category=ENTITY_CATEGORY_CONFIG,
    )
    .extend({cv.Required(CONF_SLOT): cv.int_range(min=0, max=7)})
    .extend(cv.COMPONENT_SCHEMA)
    .extend(lora_client.LORA_CLIENT_SCHEMA)
)


async def to_code(config):
    var = await number.new_number(config, min_value=0, max_value=100, step=1)
    await cg.register_component(var, config)
    paren = await cg.get_variable(config[lora_client.CONF_LORA_CLIENT_ID])
    cg.add(var.set_lora_parent(paren))
    cg.add(var.set_slot(config[CONF_SLOT]))
