import esphome.codegen as cg
from esphome.components import text, text_sensor
from esphome.components import lora_client
import esphome.config_validation as cv
from esphome.const import CONF_ID, ENTITY_CATEGORY_CONFIG

# The whole schedule in one field, as an alternative to the twenty per-slot
# entities. Both implementations are kept so the UX can be compared; only one
# should be referenced from YAML at a time.

AUTO_LOAD = ["loracover", "blindsproto", "text_sensor"]
DEPENDENCIES = ["lora_tracker", "lora_client"]

CONF_STATUS = "status"
CONF_HELP = "help"

loracov_ns = cg.esphome_ns.namespace("loracov")
ScheduleText = loracov_ns.class_("ScheduleText", text.Text, cg.Component)

CONFIG_SCHEMA = (
    # `mode` is required by the text platform and belongs on the SCHEMA, not on
    # new_text(); defaulting it here keeps the YAML a single clean block.
    text.text_schema(
        ScheduleText, entity_category=ENTITY_CATEGORY_CONFIG, mode="TEXT"
    )
    .extend(
        {
            # Companion read-only entity carrying "ok" or "error: ...".
            # Without it a rejected edit would silently snap back with no
            # explanation, which is the one thing this format cannot afford.
            cv.Optional(CONF_STATUS): text_sensor.text_sensor_schema(
                entity_category=ENTITY_CATEGORY_CONFIG,
                icon="mdi:alert-circle-outline",
            ),
            # The syntax, as a value HA can display. ESPHome has no way to
            # attach a description to an entity, so a text_sensor is the only
            # route to getting the help in front of whoever edits the field.
            cv.Optional(CONF_HELP): text_sensor.text_sensor_schema(
                entity_category=ENTITY_CATEGORY_CONFIG,
                icon="mdi:help-circle-outline",
            ),
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
    .extend(lora_client.LORA_CLIENT_SCHEMA)
)


async def to_code(config):
    var = await text.new_text(config, min_length=0, max_length=240)
    await cg.register_component(var, config)
    paren = await cg.get_variable(config[lora_client.CONF_LORA_CLIENT_ID])
    cg.add(var.set_lora_parent(paren))
    if CONF_STATUS in config:
        sens = await text_sensor.new_text_sensor(config[CONF_STATUS])
        cg.add(var.set_status(sens))
    if CONF_HELP in config:
        helper = await text_sensor.new_text_sensor(config[CONF_HELP])
        cg.add(var.set_help(helper))
