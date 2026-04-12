import re

from esphome import automation
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import ble_client
from esphome.const import CONF_ID, CONF_TRIGGER_ID


DEPENDENCIES = ['ble_client']
AUTO_LOAD = ["sensor", "text_sensor"]
CODE_OWNERS=["@fsievers22"]

MULTI_CONF=3

CONF_HOMEASSISTANT_EVENT = "homeassistant_event"
CONF_CODE = "code"
CONF_ON_HID_EVENT = "on_hid_event"
CONF_OVERRIDES = "overrides"

OVERRIDE_ID_PATTERN = re.compile(r"^(\d+)_(\d+)$")

ble_client_hid_ns = cg.esphome_ns.namespace("ble_client_hid")

BLEClientHID = ble_client_hid_ns.class_(
    "BLEClientHID",
    cg.Component,
    ble_client.BLEClientNode,
)
HIDEventTrigger = ble_client_hid_ns.class_(
    "HIDEventTrigger",
    automation.Trigger.template(cg.std_string, cg.std_string, cg.int32),
)


def validate_override_code(value):
    if isinstance(value, int):
        raise cv.Invalid(
            "override codes must be quoted strings in 'usage_page_usage' format, for example '7_81' (Keyboard DownArrow)"
        )

    value = cv.string_strict(value).strip()
    match = OVERRIDE_ID_PATTERN.fullmatch(value)
    if match is None:
        raise cv.Invalid(
            "override codes must use 'usage_page_usage' format with decimal integers, for example '7_81' (Keyboard DownArrow)"
        )

    usage_page, usage = (int(part) for part in match.groups())
    if usage_page > 0xFFFF or usage > 0xFFFF:
        raise cv.Invalid("override codes must use 16-bit usage_page and usage values")

    return value


def validate_overrides(value):
    value = cv.Schema({}, extra=cv.ALLOW_EXTRA)(value)
    if not isinstance(value, dict):
        raise cv.Invalid(
            "overrides must be a mapping of HID code strings to friendly names"
        )

    return {
        validate_override_code(code): cv.string_strict(name)
        for code, name in value.items()
    }

CONFIG_SCHEMA = (
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(BLEClientHID),
            cv.Optional(CONF_HOMEASSISTANT_EVENT, default=True): cv.boolean,
            cv.Optional(CONF_OVERRIDES, default={}): validate_overrides,
            cv.Optional(CONF_ON_HID_EVENT): automation.validate_automation(
                {
                    cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(HIDEventTrigger),
                }
            ),
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
    .extend(ble_client.BLE_CLIENT_SCHEMA)
)

CONF_BLE_CLIENT_HID_ID = "ble_client_hid_id"

BLE_CLIENT_HID_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_BLE_CLIENT_HID_ID): cv.use_id(BLEClientHID),
    }
)

async def register_last_event_usage_text_sensor(var, config):
    parent = await cg.get_variable(config[CONF_BLE_CLIENT_HID_ID])
    cg.add(parent.register_last_event_usage_text_sensor(var))

async def register_last_event_code_text_sensor(var, config):
    parent = await cg.get_variable(config[CONF_BLE_CLIENT_HID_ID])
    cg.add(parent.register_last_event_code_text_sensor(var))

async def register_last_event_value_sensor(var, config):
    parent = await cg.get_variable(config[CONF_BLE_CLIENT_HID_ID])
    cg.add(parent.register_last_event_value_sensor(var))

async def register_battery_sensor(var, config):
    parent = await cg.get_variable(config[CONF_BLE_CLIENT_HID_ID])
    cg.add(parent.register_battery_sensor(var))

async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    if config[CONF_HOMEASSISTANT_EVENT]:
        cg.add_define("USE_BLE_CLIENT_HID_HOMEASSISTANT_EVENT")
    cg.add(var.set_homeassistant_event_enabled(config[CONF_HOMEASSISTANT_EVENT]))
    for override_code, override_name in config[CONF_OVERRIDES].items():
        cg.add(var.add_override(override_code, override_name))
    await cg.register_component(var, config)
    await ble_client.register_ble_node(var, config)
    for automation_conf in config.get(CONF_ON_HID_EVENT, []):
        trigger = cg.new_Pvariable(automation_conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(
            trigger,
            [
                (cg.std_string, "code"),
                (cg.std_string, "name"),
                (cg.int32, "value"),
            ],
            automation_conf,
        )
