import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import ble_client
from esphome.const import CONF_ID


DEPENDENCIES = ['ble_client']
AUTO_LOAD = ["sensor", "text_sensor"]
CODE_OWNERS=["@fsievers22"]

MULTI_CONF=3

CONF_HOMEASSISTANT_EVENT = "homeassistant_event"

ble_client_hid_ns = cg.esphome_ns.namespace("ble_client_hid")

BLEClientHID = ble_client_hid_ns.class_(
    "BLEClientHID",
    cg.Component,
    ble_client.BLEClientNode,
)

CONFIG_SCHEMA = (
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(BLEClientHID),
            cv.Optional(CONF_HOMEASSISTANT_EVENT, default=True): cv.boolean,
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
    await cg.register_component(var, config)
    await ble_client.register_ble_node(var, config)
