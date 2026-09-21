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
    UNIT_AMPERE,
    UNIT_DECIBEL_MILLIWATT,
    UNIT_PERCENT,
    UNIT_SECOND,
    UNIT_VOLT,
)

CONF_LINK_RSSI = "link_rssi"
CONF_MOTOR_CURRENT = "motor_current"
CONF_CLOCK_OFFSET = "clock_offset"

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
            # Motor current from the CoverPosition frame, in AMPS.
            #
            # BOTH HALVES OR NEITHER, and this is the "both" state. The node
            # converts the VNH5019 CS reading on-node (I_OUT = V_CS * K / R_CS,
            # K ~7100, R_CS 1 kOhm measured on the board) and puts amps in the
            # field; this declares the unit to match. Declaring amps while the
            # node still sent raw counts would be a wrong number wearing a
            # confident unit — worse than an honest unitless one — and it is the
            # current the endstop in MotorPolicy.h is judged against.
            #
            # The node's FSM still consumes RAW counts for that endstop, which
            # is deliberate: the threshold was tuned against counts and changing
            # its units would change stop behaviour.
            cv.Optional(CONF_MOTOR_CURRENT): sensor.sensor_schema(
                unit_of_measurement=UNIT_AMPERE,
                device_class=DEVICE_CLASS_CURRENT,
                state_class=STATE_CLASS_MEASUREMENT,
                accuracy_decimals=2,
                icon="mdi:current-dc",
            ),
            # P2: node clock minus hub clock, reported in the wake beacon.
            # Positive = the node runs ahead. This is how the 32.768 kHz
            # crystal's real drift becomes visible without a serial cable, and
            # it is the measurement the "no sleep cap" decision rests on.
            cv.Optional(CONF_CLOCK_OFFSET): sensor.sensor_schema(
                unit_of_measurement=UNIT_SECOND,
                state_class=STATE_CLASS_MEASUREMENT,
                accuracy_decimals=0,
                icon="mdi:clock-alert-outline",
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
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

    if clock_offset_config := config.get(CONF_CLOCK_OFFSET):
        sens = await sensor.new_sensor(clock_offset_config)
        cg.add(var.set_clock_offset(sens))

    if current_config := config.get(CONF_MOTOR_CURRENT):
        sens = await sensor.new_sensor(current_config)
        cg.add(var.set_motor_current(sens))
