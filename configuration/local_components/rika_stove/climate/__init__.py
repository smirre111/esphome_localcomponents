import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import climate
from esphome.const import CONF_ID
from esphome.components import uart


DEPENDENCIES = ['uart']
AUTO_LOAD = ["climate"]

rikastove_ns = cg.esphome_ns.namespace('rikastove')
RikaStove = rikastove_ns.class_('RikaStove', climate.Climate, cg.Component)


CONF_STOVE_ID = 'rikastove_id'
CONF_SUPPORTS_HEAT = 'supports_heat'
CONF_SUPPORTS_COOL = 'supports_cool'
CONF_PIN_CODE = "pin_code"
CONF_PHONE_NUMBER = "phone_number"

# CONFIG_SCHEMA = cv.All(climate.CLIMATE_SCHEMA.extend({
#     cv.GenerateID(): cv.declare_id(RikaStove),
#     cv.Optional(CONF_SUPPORTS_COOL, default=False): cv.boolean,
#     cv.Optional(CONF_SUPPORTS_HEAT, default=True): cv.boolean,
#     cv.Optional(CONF_PIN_CODE, default="1211"): cv.string,
#     cv.Optional(CONF_PHONE_NUMBER, default="+436508012415"): cv.string,
# }).extend(cv.COMPONENT_SCHEMA).extend(uart.UART_DEVICE_SCHEMA))


# CONFIG_SCHEMA = (
#     cv.Schema(
#         {
#             cv.GenerateID(): cv.declare_id(RikaStove),
#             cv.Optional(CONF_SUPPORTS_COOL, default=False): cv.boolean,
#             cv.Optional(CONF_SUPPORTS_HEAT, default=True): cv.boolean,
#             cv.Optional(CONF_PIN_CODE, default="1211"): cv.string,
#             cv.Optional(CONF_PHONE_NUMBER, default="+436508012415"): cv.string,
#         }
#     )
#     .extend(climate.CLIMATE_SCHEMA)
#     .extend(cv.COMPONENT_SCHEMA)
#     .extend(uart.UART_DEVICE_SCHEMA)
# )

CONFIG_SCHEMA = cv.All(
    climate.climate_schema(RikaStove)
    .extend(
        {
            cv.GenerateID(): cv.declare_id(RikaStove),
            cv.Optional(CONF_SUPPORTS_COOL, default=False): cv.boolean,
            cv.Optional(CONF_SUPPORTS_HEAT, default=True): cv.boolean,
            cv.Optional(CONF_PIN_CODE, default="1211"): cv.string,
            cv.Optional(CONF_PHONE_NUMBER, default="+436508012415"): cv.string,
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
    .extend(uart.UART_DEVICE_SCHEMA)
   
)



def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    yield cg.register_component(var, config)
    yield climate.register_climate(var, config)
    yield uart.register_uart_device(var, config)

    cg.add(var.set_supports_cool(config[CONF_SUPPORTS_COOL]))
    cg.add(var.set_supports_heat(config[CONF_SUPPORTS_HEAT]))
    cg.add(var.set_pin_code(config[CONF_PIN_CODE]))
    cg.add(var.set_phone_number(config[CONF_PHONE_NUMBER]))