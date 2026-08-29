import esphome.codegen as cg
from esphome.components import datetime
from esphome.components import lora_client
import esphome.config_validation as cv
from esphome.const import ENTITY_CATEGORY_CONFIG

# One editable time per schedule slot.
#
# The schedule used to live only in YAML, so changing when a blind moves meant
# recompiling and reflashing the hub. These entities put it in Home Assistant.
#
# The hub persists its own slot table (see LORAListener::save_schedule_), which
# is what actually survives a reboot — the YAML schedule is a first-boot seed.

AUTO_LOAD = ["loracover", "blindsproto"]
DEPENDENCIES = ["lora_tracker", "lora_client"]

CONF_SLOT = "slot"

loracov_ns = cg.esphome_ns.namespace("loracov")
ScheduleSlotTime = loracov_ns.class_(
    "ScheduleSlotTime", datetime.TimeEntity, cg.Component
)

CONFIG_SCHEMA = (
    datetime.time_schema(ScheduleSlotTime)
    .extend(
        {
            # Which slot this entity edits. Stable index, because HA edits one
            # field of one slot at a time.
            cv.Required(CONF_SLOT): cv.int_range(min=0, max=7),
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
    .extend(lora_client.LORA_CLIENT_SCHEMA)
)


async def to_code(config):
    var = await datetime.new_datetime(config)
    await cg.register_component(var, config)
    paren = await cg.get_variable(config[lora_client.CONF_LORA_CLIENT_ID])
    cg.add(var.set_lora_parent(paren))
    cg.add(var.set_slot(config[CONF_SLOT]))
