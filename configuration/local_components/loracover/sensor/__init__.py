import esphome.codegen as cg
from esphome.components import sensor
from esphome.components import lora_client
import esphome.config_validation as cv
from esphome.const import (
    CONF_BATTERY_LEVEL,
    CONF_ID,
    CONF_BATTERY_VOLTAGE,
    DEVICE_CLASS_BATTERY,
    ENTITY_CATEGORY_DIAGNOSTIC,
    UNIT_PERCENT,
    UNIT_VOLT
)

AUTO_LOAD = ["loracover", "blindsproto"]
CODEOWNERS = ["@buxtronix"]
DEPENDENCIES = ["lora_tracker", "lora_client"]



loracov_ns = cg.esphome_ns.namespace("loracov")

LoraCover = loracov_ns.class_("LoraCover", lora_client.LORAClientNode, cg.PollingComponent)

CONFIG_SCHEMA = (
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(LoraCover),
            cv.Optional(CONF_BATTERY_LEVEL): sensor.sensor_schema(
                unit_of_measurement=UNIT_PERCENT,
                device_class=DEVICE_CLASS_BATTERY,
                accuracy_decimals=0,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ),
            cv.Optional(CONF_BATTERY_VOLTAGE): sensor.sensor_schema(
                unit_of_measurement=UNIT_VOLT,
                icon="mdi:flash",
                accuracy_decimals=1,
            ),
        }
    )
    .extend(lora_client.LORA_CLIENT_SCHEMA)
    .extend(cv.polling_component_schema("120s"))
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await lora_client.register_node(var, config)

    if battery_level_config := config.get(CONF_BATTERY_LEVEL):
        sens = await sensor.new_sensor(battery_level_config)
        cg.add(var.set_battery(sens))

    if voltage_config := config.get(CONF_BATTERY_VOLTAGE):
        sens = await sensor.new_sensor(voltage_config)
        cg.add(var.set_voltage(sens))
