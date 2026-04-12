# ESPHome external component to read hid events from a ble client
The `ble_client_hid` external component foor ESPHome can be used to capture hid events like key presses from a hid device connected via Bluetooth LE.
**Boards without internal PSRAM seem to be not compatible**
#### Tested working with:
- FireTV Remote of [Fire TV Stick - 3rd Gen (2020)](https://developer.amazon.com/docs/fire-tv/device-specifications-fire-tv-streaming-media-player.html?v=ftvstickgen3)
- Nvidia Shield-Fernbedienung (2019)
- Ruwido Model 827A (Virgin Telco 4k Spain): [Manual](https://fcc.report/FCC-ID/XYN827A)

## How to Use
### Add as external component:
See [External Components](https://esphome.io/components/external_components.html):
```yaml
external_components:
  # use ble_client_hid from this master branch in GitHub
  - source: github://fsievers22/esphome-ble-remote@master
    components: [ ble_client_hid ]
```
### Component:
Multiple `ble_client_hid` components can be configured, but at max three. (See [BLE Client](https://esphome.io/components/ble_client.html) notes for more info).

The device has to use the `esp-idf` framework:
```yaml
esp32:
  board: az-delivery-devkit-v4  #modify to fit your board
  framework:                    #only works n esp-idf framework
    type: esp-idf
```
Each `ble_client_hid` component requires a `ble_client`.
```yaml
esp32_ble_tracker:            

ble_client:
  - id: ble_client_1
    mac_address: "FF:FF:20:00:0F:15"    #modify to fit your ble device

ble_client_hid:
  - id: ble_client_hid_1
    ble_client_id: ble_client_1
    homeassistant_event: false
    overrides:
      "7_82": "up"
      "7_81": "down"
    on_hid_event:
      - logger.log:
          format: "BLE HID event code=%s name=%s value=%d"
          args: [code.c_str(), name.c_str(), value]
```
#### Configuration variables:
- **id**(**Required**, ID): The ID to use for code generation, and for regerence by dependant components
- **ble_client_id**(**Required**, ID): The ID of the `ble_client` component associated with this component can be omitted if only one `ble_client` is registered
- **homeassistant_event**(**Optional**, boolean): Whether to fire the `esphome.hid_events` Home Assistant event. Defaults to `true`. Set this to `false` to skip the native API event overhead.
- **overrides**(**Optional**, mapping): Rename individual HID codes before they are published to automations, text sensors, and Home Assistant events.
  Use the HID code as the key and the friendly name as the value.
  Example:
  ```yaml
  overrides:
    "7_81": "down"
    "7_82": "up"
  ```
- **on_hid_event**(**Optional**, automation): Runs for each parsed HID event and exposes three variables:
  - **code**: Combined HID code in `{page}_{usage}` decimal format
  - **name**: Resolved name (after applying overrides)
  - **value**: Parsed HID value, typically `1` for press and `0` for release on button-like inputs
#### Events:
When `homeassistant_event` is enabled, the component sends an event through the Home Assistant native API when an HID event happens.
The event is named `esphome.hid_events` and contains `code`, `name`, and `value`.
Example:
```yaml
data:
  code: "7_81"
  name: "down"
  value: 1
```
This example assumes the override above is configured for `7_81`.
Without an override, the default name for `7_81` would be `Keyboard DownArrow`.

`code` is the raw HID identifier in `{page}_{usage}` decimal format. `name` is the resolved display name after applying any configured overrides.

### Battery sensor:
The `ble_client_hid` sensor lets you track the battery level of the BLE HID client.
```yaml
esp32_ble_tracker:            

ble_client:
  - id: ble_client_1
    mac_address: "48:B0:2D:52:29:C6"    #modify to fit your ble device

ble_client_hid:
  - id: ble_client_hid_1
    ble_client_id: ble_client_1
    homeassistant_event: false

sensor:
  - platform: ble_client_hid
    type: battery
    ble_client_hid_id: ble_client_hid_1
    name: "Battery"
```
#### Configuration variables:
- **ble_client_hid_id**(**Required**, ID): The ID of the `ble_client_hid` component associated with this component, can be omitted if only one `ble_client_hid` is registered.
- **id**(**Optional**, ID): Manuallyy specify the ID used for code generation
- All other options from [Sensor](https://esphome.io/components/sensor/index.html)

### last event sensors:
The component can expose the last received event through a combination of sensors and text sensors.

```yaml
sensor:
  - platform: ble_client_hid
    type: last_event_value
    name: "Last Event Value"

text_sensor:
  - platform: ble_client_hid
    type: last_event_usage
    name: "Last Event Usage"
  - platform: ble_client_hid
    type: last_event_code
    name: "Last Event Code"
```

#### Text sensor types:
- **last_event_usage**: The resolved event name after applying overrides. If no usage name is available for a code, this falls back to the raw code string.
- **last_event_code**: The raw HID code in `{page}_{usage}` decimal format (e.g., `12_233`)

#### Configuration variables:
- **type**(**Required**, string): The type of text sensor. Either `last_event_usage` or `last_event_code`.
- **ble_client_hid_id**(**Required**, ID): The ID of the `ble_client_hid` component associated with this component, can be omitted if only one `ble_client_hid` is registered.
- **id**(**Optional**, ID): Manually specify the ID used for code generation
- All other options from [Sensor](https://esphome.io/components/sensor/index.html) or [TextSensor](https://esphome.io/components/text_sensor/index.html)

# Example device configuration:
```yaml
esp32:
  board: az-delivery-devkit-v4  #modify to fit your board
  framework:                    #only works n esp-idf framework
    type: esp-idf

esphome:
  name: example-ble-hid         

external_components:
  # use ble_client_hid from this master branch in GitHub
  - source: github://fsievers22/esphome-ble-remote@master
    components: [ ble_client_hid ]

# Enable logging
logger:
  level: INFO

# Enable Home Assistant API
api:

ota:
  password: !secret ota_password

wifi:
  ssid: !secret wifi_ssid
  password: !secret wifi_password
  fast_connect: on

esp32_ble_tracker:            

ble_client:
  - id: ble_client_1
    mac_address: "48:B0:2D:52:29:C6"    #modify to fit your ble device

ble_client_hid:
  - id: ble_client_hid_1
    ble_client_id: ble_client_1
    overrides:
      "7_82": "up"
      "7_81": "down"
      "12_233": "volume_up"
      "12_234": "volume_down"
    on_hid_event:
      - if:
          condition:
            lambda: 'return value != 0;'
          then:
            - logger.log:
                format: "BLE HID press code=%s name=%s value=%d"
                args: [code.c_str(), name.c_str(), value]

sensor:
  - platform: ble_client_hid
    type: battery
    ble_client_hid_id: ble_client_hid_1
    name: "Battery"
  - platform: ble_client_hid
    type: last_event_value
    ble_client_hid_id: ble_client_hid_1
    name: "Last Event Value"

text_sensor:
  - platform: ble_client_hid
    type: last_event_usage
    ble_client_hid_id: ble_client_hid_1
    name: "Last Event Usage"
  - platform: ble_client_hid
    type: last_event_code
    ble_client_hid_id: ble_client_hid_1
    name: "Last Event Code"
```
