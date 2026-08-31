import esphome.codegen as cg
from esphome.components import text, text_sensor
from esphome.components import lora_client
import esphome.config_validation as cv
from esphome.const import CONF_ID, ENTITY_CATEGORY_CONFIG, ENTITY_CATEGORY_DIAGNOSTIC

# The whole schedule in one field, as an alternative to the twenty per-slot
# entities. Both implementations are kept so the UX can be compared; only one
# should be referenced from YAML at a time.

AUTO_LOAD = ["loracover", "blindsproto", "text_sensor"]
DEPENDENCIES = ["lora_tracker", "lora_client"]

CONF_STATUS = "status"

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
            #
            # DIAGNOSTIC, not CONFIG. HA's categories are semantic: config
            # means "changes a setting", diagnostic means "read-only info".
            # This sensor only ever reports "ok" or "error: ...", so marking
            # it config is wrong -- and HA does not merely mis-group it, it
            # shows the entity as UNAVAILABLE. Confirmed twice now: the same
            # mistake on the `help` text_sensor made that one unavailable too,
            # and it was deleted rather than diagnosed.
            #
            # The writable ScheduleText above stays CONFIG, correctly: typing
            # a schedule into it does change a device setting.
            cv.Optional(CONF_STATUS): text_sensor.text_sensor_schema(
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
                icon="mdi:alert-circle-outline",
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
