import esphome.codegen as cg
from esphome.components import sensor
from esphome.components import lora_client
import esphome.config_validation as cv
from esphome.const import (
    CONF_BATTERY_LEVEL,
    CONF_ID,
    CONF_BATTERY_VOLTAGE,
    DEVICE_CLASS_BATTERY,
    DEVICE_CLASS_CURRENT,
    DEVICE_CLASS_SIGNAL_STRENGTH,
    ENTITY_CATEGORY_DIAGNOSTIC,
    STATE_CLASS_MEASUREMENT,
    UNIT_DECIBEL_MILLIWATT,
    UNIT_PERCENT,
    UNIT_VOLT,
)

CONF_LINK_RSSI = "link_rssi"
CONF_MOTOR_CURRENT = "motor_current"

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
            # F-11: hub-side received-signal strength for this node's packets.
            cv.Optional(CONF_LINK_RSSI): sensor.sensor_schema(
                unit_of_measurement=UNIT_DECIBEL_MILLIWATT,
                device_class=DEVICE_CLASS_SIGNAL_STRENGTH,
                state_class=STATE_CLASS_MEASUREMENT,
                accuracy_decimals=0,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ),
            # F-11: motor current reported in the CoverPosition frame (raw ADC
            # counts — apply a calibration filter in YAML to convert to amps).
            cv.Optional(CONF_MOTOR_CURRENT): sensor.sensor_schema(
                device_class=DEVICE_CLASS_CURRENT,
                state_class=STATE_CLASS_MEASUREMENT,
                accuracy_decimals=0,
                icon="mdi:current-dc",
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

    if rssi_config := config.get(CONF_LINK_RSSI):
        sens = await sensor.new_sensor(rssi_config)
        cg.add(var.set_rssi(sens))

    if current_config := config.get(CONF_MOTOR_CURRENT):
        sens = await sensor.new_sensor(current_config)
        cg.add(var.set_motor_current(sens))
