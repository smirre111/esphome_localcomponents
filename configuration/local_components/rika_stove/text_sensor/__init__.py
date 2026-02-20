import esphome.codegen as cg
from esphome.components import text_sensor
import esphome.config_validation as cv
from esphome.const import ENTITY_CATEGORY_DIAGNOSTIC, ENTITY_CATEGORY_NONE

from ..climate import CONF_STOVE_ID, RikaStove

CODEOWNERS = ["rsc"]
TextSensorTypeEnum = RikaStove.enum("SubTextSensorType", True)

# Haier text sensors
CONF_STATUS = "stove_status"


# Additional icons
ICON_TEXT_BOX = "mdi:text-box-outline"

TEXT_SENSOR_TYPES = {
    CONF_STATUS: text_sensor.text_sensor_schema(
        icon=ICON_TEXT_BOX,
        entity_category=ENTITY_CATEGORY_NONE,
    ),
}

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_STOVE_ID): cv.use_id(RikaStove),
    }
).extend({cv.Optional(type): schema for type, schema in TEXT_SENSOR_TYPES.items()})


async def to_code(config):
    paren = await cg.get_variable(config[CONF_STOVE_ID])

    for type_ in TEXT_SENSOR_TYPES:
        if conf := config.get(type_):
            sens = await text_sensor.new_text_sensor(conf)
            text_sensor_type = getattr(TextSensorTypeEnum, type_.upper())
            cg.add(paren.set_sub_text_sensor(text_sensor_type, sens))