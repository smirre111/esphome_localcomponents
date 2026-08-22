from __future__ import annotations

from dataclasses import dataclass
import logging

from esphome import automation
import esphome.codegen as cg
# from esphome.components import cover
from esphome.components import lora_tracker
from esphome.components import esp32_ble
from esphome.components import time as time_
from esphome.components import homeassistant
import esphome.components.homeassistant as ha

from esphome.components.esp32 import add_idf_sdkconfig_option
import esphome.config_validation as cv
from esphome.const import (
    CONF_NAME, 
    CONF_ID,
    CONF_MAC_ADDRESS,
    CONF_TIME,
    CONF_TRIGGER_ID,
)


from esphome.core import CORE, CoroPriority, coroutine_with_priority
from esphome.enum import StrEnum
from esphome.types import ConfigType
from esphome.core.entity_helpers import entity_duplicate_validator, setup_entity

# AUTO_LOAD = ["esp32_ble"]
CODEOWNERS = ["@buxtronix"]
AUTO_LOAD = ["lora_tracker", "blindsproto"]
DEPENDENCIES = ["lora_tracker", "time"]
MULTI_CONF = True

# CONF_INVERT_POSITION = "invert_position"
# CONF_LORA_TRACKER_ID = "lora_tracker_id"
CONF_LORA_LISTENER_ID = "lora_listener_id"
CONF_LORA_CLIENT_ID = "lora_client_id"
CONF_LORA_CLIENT_NODE_ID = "lora_client_node_id"
CONF_ON_SLEEP_START = "on_sleep_start"

CONF_SHORT_ADDRESS = "short_address"
CONF_SUBNET_ADDRESS = "subnet_address"
CONF_SLEEP_DURATION = "sleep_duration"
CONF_BATTERY_UPDATE_INTERVAL = "battery_update_interval"
CONF_TIME_ID = "time_id"  # New config key for time component

_LOGGER = logging.getLogger(__name__)


# Enum for LORA features
class LORAFeatures(StrEnum):
    LORA_DEVICE = "LORA_DEVICE"

# Dataclass for registration counts
@dataclass
class RegistrationCounts:
    listeners: int = 0
    clients: int = 0


lora_tracker_ns = cg.esphome_ns.namespace("lora_tracker")
# LORATracker = lora_tracker_ns.class_("LORATracker", cg.Component)


LORAListener = lora_tracker_ns.class_("LORAListener", cg.EntityBase,cg.Component)
LORAListenerConstRef = LORAListener.operator("ref").operator("const")


LORAClient = lora_tracker_ns.class_("LORAClient",  LORAListener, cg.EntityBase, cg.Component)
LORAClientNode = lora_tracker_ns.class_("LORAClientNode")


HomeassistantTime = ha.homeassistant_ns.class_("HomeassistantTime", time_.RealTimeClock)



# Triggers
SleepTrigger = lora_tracker_ns.class_(
    "SleepTrigger", automation.Trigger.template(LORAListenerConstRef)
)

SleepAction = lora_tracker_ns.class_("SleepAction", automation.Action)




TIME_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_TIME_ID): cv.use_id(HomeassistantTime),
    }
)

# CONFIG_SCHEMA = (
#     cover.cover_schema(LoraCoverComponent)
#     .extend(
#         {
#             cv.Required(CONF_MAC_ADDRESS): cv.mac_address,
#             cv.Optional(CONF_SHORT_ADDRESS, default=0x11): cv.int_range(min=0, max=0xFF),
#             cv.Optional(CONF_SUBNET_ADDRESS, default=0x11): cv.int_range(min=0, max=0xFF),
#             cv.Optional(CONF_PIN, default=8888): cv.int_range(min=0, max=0xFFFF),
#             cv.Optional(CONF_INVERT_POSITION, default=False): cv.boolean,
#             cv.Optional(CONF_OPEN_DURATION, default=False): cv.int_range(0, 120),
#             cv.Optional(CONF_CLOSE_DURATION, default=False): cv.int_range(0, 120),
#             cv.Optional(CONF_SLEEP_DURATION, default=False): cv.int_range(0, 864000),
#             cv.Required(CONF_TIME_ID): cv.use_id(time_.RealTimeClock),  # Require time component ID
#         }
#     )
#     .extend(lora_tracker.LORA_CLIENT_SCHEMA)
#     # .extend(TIME_SCHEMA)
#     .extend(cv.COMPONENT_SCHEMA)
# )

# CONFIG_SCHEMA = (
#     cv.Schema(
#         {
#             cv.GenerateID(): cv.declare_id(LORAListener),
#             cv.Optional(CONF_NAME): cv.string,
#             cv.Required(CONF_MAC_ADDRESS): cv.mac_address,
#             cv.Required(CONF_SHORT_ADDRESS): cv.int_range(min=0, max=0xFF),
#             cv.Required(CONF_SUBNET_ADDRESS): cv.int_range(min=0, max=0xFF),
#             cv.Required(CONF_TIME_ID): cv.use_id(time_.RealTimeClock),  # Require time component ID
#             cv.Optional(CONF_ON_SLEEP_START): automation.validate_automation(
#                 {
#                     cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(SleepTrigger),
#                     cv.Optional(CONF_MAC_ADDRESS): cv.ensure_list(cv.mac_address),
#                 }
#             ),
#         }
#     )
# )

# CONFIG_SCHEMA = (
#     cv.Schema(
#         {
#             cv.GenerateID(): cv.declare_id(LORAListener),
#             cv.Optional(CONF_NAME): cv.string,
#             cv.Required(CONF_MAC_ADDRESS): cv.mac_address,
#             cv.Required(CONF_SHORT_ADDRESS): cv.int_range(min=0, max=0xFF),
#             cv.Required(CONF_SUBNET_ADDRESS): cv.int_range(min=0, max=0xFF),
#             cv.Required(CONF_TIME_ID): cv.use_id(time_.RealTimeClock),  # Require time component ID
#             cv.Optional(CONF_ON_SLEEP_START): automation.validate_automation(
#                 {
#                     cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(SleepTrigger),
#                     cv.Optional(CONF_MAC_ADDRESS): cv.ensure_list(cv.mac_address),
#                 }
#             ),
#         }
#     )
# )

# # Triggers
# SleepTrigger = lora_tracker_ns.class_(
#     "SleepTrigger", automation.Trigger.template(LORAListenerConstRef)
# )

# SleepAction = lora_tracker_ns.class_("SleepAction", automation.Action)




# CONFIG_SCHEMA = (
#     cv.Schema(
#         {
#             cv.GenerateID(): cv.declare_id(LORATracker),
#             cv.Optional(CONF_ON_SLEEP_START): automation.validate_automation(
#                 {
#                     cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(SleepTrigger),
#                     cv.Optional(CONF_MAC_ADDRESS): cv.ensure_list(cv.mac_address),
#                 }
#             ),
#         }
#     )
# )






# CONFIG_SCHEMA = (
#     cv.Schema(
#         {
#             cv.GenerateID(): cv.declare_id(LORAListener),
#             cv.Required(CONF_MAC_ADDRESS): cv.mac_address,
#             cv.Optional(CONF_SHORT_ADDRESS, default=0x11): cv.int_range(min=0, max=0xFF),
#             cv.Optional(CONF_SUBNET_ADDRESS, default=0x11): cv.int_range(min=0, max=0xFF),
#             cv.Optional(CONF_PIN, default=8888): cv.int_range(min=0, max=0xFFFF),
#             cv.Optional(CONF_ON_SLEEP_START): automation.validate_automation(
#                 {
#                     cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(SleepTrigger),
#                     cv.Optional(CONF_MAC_ADDRESS): cv.ensure_list(cv.mac_address),
#                 }
#             ),
#         }
#     )

# )

# CONFIG_SCHEMA = (
#     cv.Schema(
#         {
#             cv.GenerateID(): cv.declare_id(LORAClient),
#             cv.Required(CONF_MAC_ADDRESS): cv.mac_address,
#             cv.Optional(CONF_SHORT_ADDRESS, default=0x11): cv.int_range(min=0, max=0xFF),
#             cv.Optional(CONF_SUBNET_ADDRESS, default=0x11): cv.int_range(min=0, max=0xFF),
#             cv.Optional(CONF_PIN, default=8888): cv.int_range(min=0, max=0xFFFF),
#             cv.Optional(CONF_ON_SLEEP_START): automation.validate_automation(
#                 {
#                     cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(SleepTrigger),
#                     cv.Optional(CONF_MAC_ADDRESS): cv.ensure_list(cv.mac_address),
#                 }
#             ),
#         }
#     )

# )

# CONFIG_SCHEMA = cv.All(
#     cv.Schema(
#         {
#             cv.GenerateID(): cv.declare_id(LORAClient),
#             cv.Optional(CONF_NAME): cv.string,

#             cv.Required(CONF_MAC_ADDRESS): cv.mac_address,
#             cv.Required(CONF_SHORT_ADDRESS): cv.int_range(min=0, max=0xFF),
#             cv.Required(CONF_SUBNET_ADDRESS): cv.int_range(min=0, max=0xFF),
#             cv.Required(CONF_TIME_ID): cv.use_id(time_.RealTimeClock),  # Require time component ID
#             cv.Optional(CONF_SLEEP_DURATION, default=False): cv.int_range(0, 864000),
            
#             cv.Optional(CONF_ON_SLEEP_START): automation.validate_automation(
#                 {
#                     cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(SleepTrigger),
#                     cv.Optional(CONF_MAC_ADDRESS): cv.ensure_list(cv.mac_address),
#                 }
#             ),
#         }

#     )
#     # .extend(TIME_SCHEMA)
#     .extend(lora_tracker.LORA_TRACKER_SCHEMA)
#     .extend(cv.COMPONENT_SCHEMA)
# )


# ---- P4: automatic (scheduled) mode ----
CONF_AUTO_MODE           = "auto_mode"
CONF_INTERACTIVE_TIMEOUT = "interactive_timeout"
CONF_CHECKIN_INTERVAL    = "checkin_interval"
CONF_BEACON_LEAD         = "beacon_lead"
CONF_POST_EVENT_WINDOW   = "post_event_window"
CONF_CATCHUP_WINDOW      = "catchup_window"
CONF_SCHEDULE            = "schedule"
CONF_DAYS                = "days"
CONF_ACTION              = "action"
CONF_POSITION            = "position"

# bit0 = MON .. bit6 = SUN, matching sched::DayBit on the node. Monday-first is
# deliberate and NOT the C tm_wday convention.
DAY_BITS = {
    "mon": 1 << 0, "tue": 1 << 1, "wed": 1 << 2, "thu": 1 << 3,
    "fri": 1 << 4, "sat": 1 << 5, "sun": 1 << 6,
}
DAY_PRESETS = {
    "daily":    0x7F,
    "weekdays": 0x1F,                  # MON-FRI
    "weekend":  (1 << 5) | (1 << 6),
    "mon-sat":  0x3F,
}
SCHED_ACTIONS = {"open": 0, "close": 1, "stop": 2, "position": 3}

# Mirrors sched::kMaxEntries on the node AND the one-frame budget: 8 entries is
# ~152 B on air against the 255 B limit.
MAX_SCHEDULE_ENTRIES = 8


def _validate_days(value):
    """Accept a preset ("daily", "weekdays", ...) or a list of day names."""
    if isinstance(value, str):
        key = value.lower()
        if key in DAY_PRESETS:
            return DAY_PRESETS[key]
        value = [value]
    if not isinstance(value, list):
        raise cv.Invalid(
            f"days must be one of {sorted(DAY_PRESETS)} or a list of day names"
        )
    mask = 0
    for day in value:
        key = str(day).lower()[:3]
        if key not in DAY_BITS:
            raise cv.Invalid(f"unknown day '{day}'; expected one of {sorted(DAY_BITS)}")
        mask |= DAY_BITS[key]
    if mask == 0:
        raise cv.Invalid("days must select at least one day, otherwise the entry never fires")
    return mask


def _validate_entry(config):
    """A position action without a position would silently drive to 0%."""
    if config[CONF_ACTION] == "position" and CONF_POSITION not in config:
        raise cv.Invalid("action: position requires a 'position:' percentage")
    if config[CONF_ACTION] != "position" and CONF_POSITION in config:
        raise cv.Invalid("'position:' is only meaningful with action: position")
    return config


SCHEDULE_ENTRY_SCHEMA = cv.All(
    cv.Schema(
        {
            # Local wall-clock time. The node stores minutes-of-day and the hub
            # resolves DST, so this stays 07:30 across a DST change.
            cv.Required(CONF_TIME): cv.time_of_day,
            cv.Optional(CONF_DAYS, default="daily"): _validate_days,
            # NOTE: cv.one_of, deliberately NOT cv.enum. cv.enum returns an
            # EStr (str subclass) whose .enum_value holds the int; comparing it
            # to an int is silently always False, and codegen does not emit the
            # mapped value either. Both failures are invisible. Keep the plain
            # string here and map it once, explicitly, in to_code.
            cv.Optional(CONF_ACTION, default="open"): cv.one_of(*SCHED_ACTIONS, lower=True),
            cv.Optional(CONF_POSITION): cv.percentage_int,
        }
    ),
    _validate_entry,
)


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(LORAClient),
            # cv.Optional(CONF_NAME): cv.string,

            cv.Required(CONF_MAC_ADDRESS): cv.mac_address,
            # F-9: short_address is now optional.  When omitted it is derived
            # from the MAC (low byte) and made collision-free across all
            # lora_client entries at code-generation time.  0x00/0xFE/0xFF are
            # reserved (broadcast / subnet-broadcast).
            cv.Optional(CONF_SHORT_ADDRESS): cv.int_range(min=1, max=0xFD),
            cv.Required(CONF_SUBNET_ADDRESS): cv.int_range(min=0, max=0xFF),
            cv.Required(CONF_TIME_ID): cv.use_id(time_.RealTimeClock),  # Require time component ID
            cv.Optional(CONF_SLEEP_DURATION, default=False): cv.int_range(0, 864000),
            # How often the node force-sends its battery status.  Accepts a time
            # period (e.g. "15min", "900s"); pushed to the node via ClientConfig.
            cv.Optional(CONF_BATTERY_UPDATE_INTERVAL, default="15min"): cv.All(
                cv.positive_time_period_seconds,
                cv.Range(min=cv.TimePeriod(seconds=10), max=cv.TimePeriod(seconds=86400)),
            ),

            # ---- P4: automatic (scheduled) mode ----
            # auto_mode is the DEFAULT the hub pushes; the node still refuses to
            # enter it without a valid clock and a schedule that can fire.
            cv.Optional(CONF_AUTO_MODE, default=False): cv.boolean,
            # 0 = stay interactive until told otherwise. A documented, meaningful
            # zero (blinds.proto) — the node does NOT treat it as "unset".
            cv.Optional(CONF_INTERACTIVE_TIMEOUT, default="30min"): cv.All(
                cv.positive_time_period_seconds,
                cv.Range(max=cv.TimePeriod(seconds=86400)),
            ),
            # 0 = no periodic check-in. Bounds how long a hub-side config change
            # can sit unseen by a sleeping node.
            cv.Optional(CONF_CHECKIN_INTERVAL, default="6h"): cv.All(
                cv.positive_time_period_seconds,
                cv.Range(max=cv.TimePeriod(seconds=604800)),
            ),
            # Wake this early so the beacon exchange and any pending config land
            # BEFORE the event runs — an edit made an hour ago then takes effect
            # on this event, cancellation included.
            cv.Optional(CONF_BEACON_LEAD, default="30s"): cv.All(
                cv.positive_time_period_seconds,
                cv.Range(min=cv.TimePeriod(seconds=5), max=cv.TimePeriod(seconds=300)),
            ),
            cv.Optional(CONF_POST_EVENT_WINDOW, default="20s"): cv.All(
                cv.positive_time_period_seconds,
                cv.Range(min=cv.TimePeriod(seconds=5), max=cv.TimePeriod(seconds=300)),
            ),
            # 0 = never replay a missed event.
            cv.Optional(CONF_CATCHUP_WINDOW, default="30min"): cv.All(
                cv.positive_time_period_seconds,
                cv.Range(max=cv.TimePeriod(seconds=86400)),
            ),
            cv.Optional(CONF_SCHEDULE): cv.All(
                cv.ensure_list(SCHEDULE_ENTRY_SCHEMA),
                cv.Length(max=MAX_SCHEDULE_ENTRIES),
            ),

            cv.Optional(CONF_ON_SLEEP_START): automation.validate_automation(
                {
                    cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(SleepTrigger),
                    cv.Optional(CONF_MAC_ADDRESS): cv.ensure_list(cv.mac_address),
                }
            ),
        }
    )
    .extend(cv.ENTITY_BASE_SCHEMA)
    .extend(cv.MQTT_COMMAND_COMPONENT_SCHEMA)
    # .extend(TIME_SCHEMA)
    .extend(lora_tracker.LORA_TRACKER_SCHEMA)
    .extend(cv.COMPONENT_SCHEMA)
)

# LORA_TRACKER_SCHEMA = cv.Schema(
#     {
#         cv.GenerateID(CONF_LORA_TRACKER_ID): cv.use_id(LORATracker),
#     }
# )

LORA_LISTENER_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_LORA_LISTENER_ID): cv.use_id(LORAListener),
    }
)

LORA_CLIENT_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_LORA_CLIENT_ID): cv.use_id(LORAClient),
    }
)

LORA_CLIENT_NODE_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_LORA_CLIENT_NODE_ID): cv.use_id(LORAClientNode),
    }
)

SLEEP_ACTION_SCHEMA = automation.maybe_simple_id(
    {
        cv.Required(CONF_ID): cv.use_id(LORAListener),
        # cv.Required(CONF_STATE): cv.templatable(cv.boolean),
    }
)


SLEEP_CONTROL_ACTION_SCHEMA = automation.maybe_simple_id(
    {
        cv.Required(CONF_ID): cv.use_id(LORAListener),
        # cv.Required(CONF_STATE): cv.templatable(cv.boolean),
    }
)


@automation.register_action(
    "lora_tracker.sleep_control", SleepAction, SLEEP_CONTROL_ACTION_SCHEMA
)
async def sleep_control_to_code(config, action_id, template_arg, args):
    paren = await cg.get_variable(config[CONF_ID])
    var = cg.new_Pvariable(action_id, template_arg, paren)
    # template_ = await cg.templatable(config[CONF_STATE], args, bool)
    # cg.add(var.set_state(template_))
    return var

@automation.register_action("loracover.on_sleep_start", SleepAction, SLEEP_ACTION_SCHEMA)
async def tracker_sleep_start_to_code(config, action_id, template_arg, args):
    paren = await cg.get_variable(config[CONF_ID])
    return cg.new_Pvariable(action_id, template_arg, paren)

# CORE.data keys for state management
LORA_TRACKER_REQUIRED_FEATURES_KEY = "lora_tracker_required_features"
LORA_TRACKER_REGISTRATION_COUNTS_KEY = "lora_tracker_registration_counts"


def _get_required_features() -> set[LORAFeatures]:
    """Get the set of required LORA features from CORE.data."""
    return CORE.data.setdefault(LORA_TRACKER_REQUIRED_FEATURES_KEY, set())


def _get_registration_counts() -> RegistrationCounts:
    """Get the registration counts from CORE.data."""
    return CORE.data.setdefault(
        LORA_TRACKER_REGISTRATION_COUNTS_KEY, RegistrationCounts()
    )


def register_lora_features(features: set[LORAFeatures]) -> None:
    """Register LORA features that a component needs.

    Args:
        features: Set of LORAFeatures enum members
    """
    _get_required_features().update(features)


# F-9: short-address auto-assignment ------------------------------------------
RESERVED_ADDRESSES = {0x00, 0xFE, 0xFF}
LORA_ADDR_ASSIGNMENTS_KEY = "lora_client_addr_assignments"


def _compute_address_assignments() -> dict[str, int]:
    """Resolve a collision-free short_address for every lora_client.

    Entries with an explicit short_address keep it; the rest are derived from
    the MAC low byte and linear-probed (skipping reserved/used values) so the
    result is deterministic and collision-free.  Computed once and cached in
    CORE.data because each lora_client is code-generated independently.
    """
    cache = CORE.data.get(LORA_ADDR_ASSIGNMENTS_KEY)
    if cache is not None:
        return cache

    assignments: dict[str, int] = {}
    used: set[int] = set()
    clients = CORE.config.get("lora_client", [])

    # First pass: honour explicitly configured addresses.
    for conf in clients:
        if CONF_SHORT_ADDRESS in conf:
            addr = conf[CONF_SHORT_ADDRESS]
            assignments[conf[CONF_ID].id] = addr
            used.add(addr)

    # Second pass: derive from MAC low byte, probing for a free slot.
    for conf in clients:
        if CONF_SHORT_ADDRESS in conf:
            continue
        addr = conf[CONF_MAC_ADDRESS].parts[5]  # low byte of the MAC
        for _ in range(0xFD):
            if addr not in used and addr not in RESERVED_ADDRESSES:
                break
            addr += 1
            if addr > 0xFD:
                addr = 0x01
        else:
            raise cv.Invalid("Unable to auto-assign a free short_address — address space exhausted")
        assignments[conf[CONF_ID].id] = addr
        used.add(addr)

    CORE.data[LORA_ADDR_ASSIGNMENTS_KEY] = assignments
    return assignments







async def to_code(config):

    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await setup_entity(var, config, "lora_client")
    add_idf_sdkconfig_option("CONFIG_BT_ENABLED", True)

    # cg.add(var.set_pin(config[CONF_PIN]))
    # F-9: use the explicit short_address or the auto-derived collision-free one.
    short_address = config.get(CONF_SHORT_ADDRESS)
    if short_address is None:
        short_address = _compute_address_assignments()[config[CONF_ID].id]
        _LOGGER.info(
            "lora_client '%s': auto-assigned short_address %d from MAC",
            config[CONF_ID].id,
            short_address,
        )
    cg.add(var.set_short_address(short_address))
    cg.add(var.set_subnet_address(config[CONF_SUBNET_ADDRESS]))
    cg.add(var.set_address(config[CONF_MAC_ADDRESS].as_hex))

    # cg.add(var.set_invert_position(config[CONF_INVERT_POSITION]))
    # cg.add(var.set_open_duration(config[CONF_OPEN_DURATION]))
    # cg.add(var.set_close_duration(config[CONF_CLOSE_DURATION]))
    cg.add(var.set_sleep_duration(config[CONF_SLEEP_DURATION]))
    cg.add(var.set_battery_update_interval(config[CONF_BATTERY_UPDATE_INTERVAL].total_seconds))

    # ---- P4: automatic (scheduled) mode ----
    # These become the hub's PENDING schedule. It is pushed to the node at its
    # next beacon whenever the node reports a different version, so a node that
    # is asleep right now picks the change up when it next wakes.
    cg.add(var.set_auto_mode_default(config[CONF_AUTO_MODE]))
    cg.add(var.set_interactive_timeout(config[CONF_INTERACTIVE_TIMEOUT].total_seconds))
    cg.add(var.set_checkin_interval(config[CONF_CHECKIN_INTERVAL].total_seconds))
    cg.add(var.set_beacon_lead(config[CONF_BEACON_LEAD].total_seconds))
    cg.add(var.set_post_event_window(config[CONF_POST_EVENT_WINDOW].total_seconds))
    cg.add(var.set_catchup_window(config[CONF_CATCHUP_WINDOW].total_seconds))

    for entry in config.get(CONF_SCHEDULE, []):
        # cv.time_of_day yields a dict, not a datetime.
        tod = entry[CONF_TIME]
        cg.add(
            var.add_schedule_entry(
                tod["hour"] * 60 + tod["minute"],
                entry[CONF_DAYS],
                SCHED_ACTIONS[entry[CONF_ACTION]],
                entry.get(CONF_POSITION, 0),
            )
        )


    # Get the time component variable and set it
    timeInstance = await cg.get_variable(config[CONF_TIME_ID])
    cg.add(var.set_time(timeInstance))

    # Register LORA listener feature if any of the automation triggers are used
    if config.get(CONF_ON_SLEEP_START):
        register_lora_features({LORAFeatures.LORA_DEVICE})

    registration_counts = _get_registration_counts()

    # await cg.register_component(var, config)
    await lora_tracker.register_client(var, config)

    for conf in config.get(CONF_ON_SLEEP_START, []):
        registration_counts.listeners += 1
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        if CONF_MAC_ADDRESS in conf:
            addr_list = [it.as_hex for it in conf[CONF_MAC_ADDRESS]]
            cg.add(trigger.set_addresses(addr_list))
        await automation.build_automation(trigger, [(LORAListenerConstRef, "x")], conf)


async def register_node(var: cg.SafeExpType, config: ConfigType) -> cg.SafeExpType:
    # register_lora_features({LORAFeatures.LORA_DEVICE})
    # _get_registration_counts().clients += 1
    paren = await cg.get_variable(config[CONF_LORA_CLIENT_ID])
    cg.add(paren.register_lora_node(var))
    return var














