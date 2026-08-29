import esphome.codegen as cg
from esphome.components import select
from esphome.components import lora_client
import esphome.config_validation as cv
from esphome.const import CONF_TYPE, ENTITY_CATEGORY_CONFIG

# Action and days for one schedule slot.
#
# Days are a preset list rather than seven switches: 7 switches x 4 slots x 2
# nodes would be 56 extra entities for a choice that is nearly always one of the
# first three. Arbitrary masks (Mon+Thu) remain expressible in YAML — see the
# note in schedule_select.cpp.

AUTO_LOAD = ["loracover", "blindsproto"]
DEPENDENCIES = ["lora_tracker", "lora_client"]

CONF_SLOT = "slot"

loracov_ns = cg.esphome_ns.namespace("loracov")
ScheduleSlotSelect = loracov_ns.class_("ScheduleSlotSelect", select.Select, cg.Component)
SlotSelectKind = loracov_ns.enum("SlotSelectKind", is_class=True)

# Must match mask_for_option() / the action mapping in schedule_select.cpp.
ACTION_OPTIONS = ["open", "close", "position"]
DAYS_OPTIONS = ["daily", "weekdays", "weekend",
                "mon", "tue", "wed", "thu", "fri", "sat", "sun"]

TYPES = {
    "action": (SlotSelectKind.ACTION, ACTION_OPTIONS, "mdi:arrow-up-down"),
    "days": (SlotSelectKind.DAYS, DAYS_OPTIONS, "mdi:calendar-week"),
}

CONFIG_SCHEMA = (
    select.select_schema(ScheduleSlotSelect, entity_category=ENTITY_CATEGORY_CONFIG)
    .extend(
        {
            cv.Required(CONF_SLOT): cv.int_range(min=0, max=7),
            cv.Required(CONF_TYPE): cv.one_of(*TYPES, lower=True),
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
    .extend(lora_client.LORA_CLIENT_SCHEMA)
)


async def to_code(config):
    kind, options, icon = TYPES[config[CONF_TYPE]]
    var = await select.new_select(config, options=options)
    await cg.register_component(var, config)
    paren = await cg.get_variable(config[lora_client.CONF_LORA_CLIENT_ID])
    cg.add(var.set_lora_parent(paren))
    cg.add(var.set_slot(config[CONF_SLOT]))
    cg.add(var.set_kind(kind))
