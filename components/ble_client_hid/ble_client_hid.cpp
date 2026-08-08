#include "ble_client_hid.h"

#include "usages.h"

#include <algorithm>

#ifdef USE_ESP32

// BLEClientBase::services_ is protected with no public get_services() in current ESPHome.
// This accessor exposes it via a derived-class cast; safe because no data members are added.
namespace {
struct BLEClientServicesAccessor : esphome::esp32_ble_client::BLEClientBase {
  const std::vector<esphome::esp32_ble_client::BLEService *> &get_services() {
    return this->services_;
  }
};
inline const std::vector<esphome::esp32_ble_client::BLEService *> &get_ble_client_services(
    esphome::ble_client::BLEClient *client) {
  // Upcast to BLEClientBase (valid), then reinterpret as the accessor subclass
  // (safe: accessor adds no data members, get_services() is non-virtual).
  auto *base = static_cast<esphome::esp32_ble_client::BLEClientBase *>(client);
  return reinterpret_cast<BLEClientServicesAccessor *>(base)->get_services();
}
}  // namespace

namespace esphome {
namespace ble_client_hid {

static const char *const TAG = "ble_client_hid";

static const std::string EMPTY = "";

static constexpr uint8_t MAX_READ_RETRIES = 2;
static constexpr uint32_t READ_RETRY_DELAY_MS = 100;

HIDEventTrigger::HIDEventTrigger(BLEClientHID *parent) {
  parent->add_on_event_callback(
  [this](const std::string &code, const std::string &name, int32_t value) { this->trigger(code, name, value); });
}

void BLEClientHID::loop() {
  switch (this->hid_state) {
    case HIDState::BLE_CONNECTED:
      this->read_client_characteristics();  // not instant, finished when
                                            // hid_state = HIDState::READ_CHARS
      this->hid_state = HIDState::READING_CHARS;
      break;
    case HIDState::READING_CHARS:
      this->start_next_read_();
      break;
    case HIDState::READ_CHARS:
      if (this->configure_hid_client()) {
        this->hid_state = this->handles_waiting_for_notify_registration == 0
                              ? HIDState::NOTIFICATIONS_REGISTERED
                              : HIDState::NOTIFICATIONS_REGISTERING;
      } else {
        this->hid_state = HIDState::NO_HID_SERVICE;
        this->node_state = espbt::ClientState::ESTABLISHED;
      }
      break;
    case HIDState::NOTIFICATIONS_REGISTERED:
      ESP_LOGD(TAG, "HID client configured");
      this->hid_state = HIDState::HID_CONFIGURED;
      this->node_state = espbt::ClientState::ESTABLISHED;
      this->status_clear_warning();
      break;
    default:
      break;
  }
}

void BLEClientHID::dump_config() {
  ESP_LOGCONFIG(TAG, "BLE Client HID:");
  ESP_LOGCONFIG(TAG, "  MAC address        : %s",
                this->parent()->address_str());
  ESP_LOGCONFIG(TAG, "  Home Assistant Event: %s",
                this->homeassistant_event_enabled_ ? "YES" : "NO");
  ESP_LOGCONFIG(TAG, "  Debug unmapped characteristics: %s",
                this->debug_unmapped_characteristics_ ? "YES" : "NO");
#if !defined(USE_API)
  if (this->homeassistant_event_enabled_) {
    ESP_LOGW(TAG, "Home Assistant event enabled, but the native API is not configured");
  }
#elif !defined(USE_API_HOMEASSISTANT_SERVICES)
  if (this->homeassistant_event_enabled_) {
    ESP_LOGW(TAG,
             "Home Assistant event enabled, but 'api.homeassistant_services' is disabled");
  }
#endif
}

void BLEClientHID::read_client_characteristics() {
  ESP_LOGD(TAG, "Reading client characteristics");
  using namespace ble_client;
  this->reset_read_state_();
  this->handle_report_reference_.clear();
  this->debug_characteristics_.clear();
  this->handles_registered_for_notify.clear();
  this->handles_waiting_for_notify_registration = 0;
  if (this->hid_report_map != nullptr) {
    delete this->hid_report_map;
    this->hid_report_map = nullptr;
  }
  BLEService *battery_service =
      this->parent()->get_service(ESP_GATT_UUID_BATTERY_SERVICE_SVC);
  BLEService *device_info_service =
      this->parent()->get_service(ESP_GATT_UUID_DEVICE_INFO_SVC);

  BLEService *hid_service = this->parent()->get_service(ESP_GATT_UUID_HID_SVC);
  BLEService *generic_access_service = this->parent()->get_service(0x1800);

  if (this->debug_unmapped_characteristics_) {
    this->log_gatt_services_();
  }

  if (generic_access_service != nullptr) {
    BLECharacteristic *device_name_char =
        generic_access_service->get_characteristic(
            ESP_GATT_UUID_GAP_DEVICE_NAME);
    this->schedule_read_char(device_name_char);
  }
  if (device_info_service != nullptr) {
    BLECharacteristic *pnp_id_char =
        device_info_service->get_characteristic(ESP_GATT_UUID_PNP_ID);
    this->schedule_read_char(pnp_id_char);
    BLECharacteristic *manufacturer_char =
        device_info_service->get_characteristic(ESP_GATT_UUID_MANU_NAME);
    this->schedule_read_char(manufacturer_char);
    BLECharacteristic *serial_number_char =
        device_info_service->get_characteristic(
            ESP_GATT_UUID_SERIAL_NUMBER_STR);
    this->schedule_read_char(serial_number_char);
  }
  if (this->battery_sensor != nullptr && battery_service != nullptr) {
    BLECharacteristic *battery_level_char =
        battery_service->get_characteristic(ESP_GATT_UUID_BATTERY_LEVEL);
    if (battery_level_char != nullptr) {
      this->battery_handle = battery_level_char->handle;
      if ((battery_level_char->properties & ESP_GATT_CHAR_PROP_BIT_READ) != 0) {
        this->schedule_read_char(battery_level_char);
      }
    }
  }
  if (hid_service != nullptr) {
    BLECharacteristic *hid_report_map_char =
        hid_service->get_characteristic(ESP_GATT_UUID_HID_REPORT_MAP);
    this->schedule_read_char(hid_report_map_char);
    ESP_LOGD(TAG, "Found %d characteristics",
             hid_service->characteristics.size());
    for (auto *chr : hid_service->characteristics) {
      if (chr->uuid.get_uuid().uuid.uuid16 != ESP_GATT_UUID_HID_REPORT) {
        continue;
      }

      if ((chr->properties & ESP_GATT_CHAR_PROP_BIT_READ) != 0) {
        this->schedule_read_char(chr);
      }

      BLEDescriptor *rpt_ref_desc =
          chr->get_descriptor(ESP_GATT_UUID_RPT_REF_DESCR);
      if (rpt_ref_desc != nullptr) {
        this->schedule_read_descriptor_(rpt_ref_desc);
      }
    }
  }
  if (this->debug_unmapped_characteristics_) {
    for (auto *service : get_ble_client_services(this->parent())) {
      if (service == nullptr) {
        continue;
      }
      for (auto *characteristic : service->characteristics) {
        if (characteristic == nullptr ||
            this->is_component_managed_characteristic_(characteristic)) {
          continue;
        }
        this->track_debug_characteristic_(service, characteristic);
        this->schedule_read_char(characteristic);
      }
    }
  }
  ESP_LOGD(TAG, "Queued %u GATT reads", static_cast<unsigned>(this->read_queue_.size()));
}
void BLEClientHID::on_gatt_read_finished(GATTReadData *data) {
  auto itr = this->handles_to_read.find(data->handle_);
  if (itr != this->handles_to_read.end()) {
    delete itr->second;
    itr->second = data;
  } else {
    delete data;
  }
}

void BLEClientHID::gattc_event_handler(esp_gattc_cb_event_t event,
                                       esp_gatt_if_t gattc_if,
                                       esp_ble_gattc_cb_param_t *param) {
  esp_ble_gattc_cb_param_t *p_data = param;
  switch (event) {
    case ESP_GATTC_CONNECT_EVT: {
      auto ret = esp_ble_set_encryption(param->connect.remote_bda,
                                        ESP_BLE_SEC_ENCRYPT);
      if (ret) {
        ESP_LOGE(TAG, "[%d] [%s] esp_ble_set_encryption error, status=%d",
                 this->parent()->get_connection_index(),
                 this->parent()->address_str(), ret);
      }
      esp_gap_conn_params_t params;
      ret = esp_ble_get_current_conn_params(
          this->parent()->get_remote_bda(), &params);
      if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to get conn params");
      }
      ESP_LOGI(TAG, "conn params: interval=%u, latency=%u, timeout=%u",
               params.interval, params.latency, params.timeout);
      break;
    }
    case ESP_GATTC_DISCONNECT_EVT: {
      ESP_LOGW(TAG, "[%s] Disconnected!",
               this->parent()->address_str());
      this->reset_read_state_();
      this->handles_registered_for_notify.clear();
      this->handles_waiting_for_notify_registration = 0;
      this->handle_report_reference_.clear();
      this->battery_handle = 0;
      if (this->hid_report_map != nullptr) {
        delete this->hid_report_map;
        this->hid_report_map = nullptr;
      }
      this->hid_state = HIDState::INIT;
      this->status_set_warning("Diconnected");
      break;
    }
    case ESP_GATTC_SEARCH_RES_EVT: {
      if (p_data->search_res.srvc_id.uuid.uuid.uuid16 ==
          ESP_GATT_UUID_HID_SVC) {
        this->hid_state = HIDState::HID_SERVICE_FOUND;
        ESP_LOGD(TAG, "GATT HID service found on device %s",
                 this->parent()->address_str());
      }
      break;
    }
    case ESP_GATTC_SEARCH_CMPL_EVT: {
      if (this->hid_state != HIDState::HID_SERVICE_FOUND) {
        // service not found
        ESP_LOGW(TAG, "No GATT HID service found on device %s",
                 this->parent()->address_str());
        this->hid_state = HIDState::NO_HID_SERVICE;
        this->status_set_warning("Invalid device config");
        break;
      }
      ESP_LOGD(TAG, "GATTC search finished with status code %d",
               p_data->search_cmpl.status);
      this->hid_state = HIDState::BLE_CONNECTED;
      esp_gap_conn_params_t params;
      esp_err_t ret = esp_ble_get_current_conn_params(
          this->parent()->get_remote_bda(), &params);
      if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to get conn params");
      }
      ESP_LOGI(TAG, "conn params: interval=%u, latency=%u, timeout=%u",
               params.interval, params.latency, params.timeout);
      break;
    }
    case ESP_GATTC_READ_CHAR_EVT:
    case ESP_GATTC_READ_DESCR_EVT: {
      if (param->read.conn_id != this->parent()->get_conn_id()) break;
      if (!this->read_in_flight_ || this->read_queue_index_ >= this->read_queue_.size() ||
          param->read.handle != this->read_queue_[this->read_queue_index_].handle) {
        break;
      }
      this->read_in_flight_ = false;
      if (param->read.status != ESP_OK) {
        this->handle_read_failure_(param->read.status);
        break;
      }
      if (param->read.handle == this->battery_handle) {
        this->publish_battery_level_(param->read.value, param->read.value_len);
      }
      this->log_debug_characteristic_value_(param->read.handle, param->read.value,
                                            param->read.value_len, "read");
      GATTReadData *data = new GATTReadData(
          param->read.handle, param->read.value, param->read.value_len);
      this->on_gatt_read_finished(data);
      this->read_queue_index_++;
      this->read_retry_at_ = 0;
      break;
    }
    case ESP_GATTC_NOTIFY_EVT: {
      if (param->notify.conn_id != this->parent()->get_conn_id()) break;
      if (p_data->notify.handle == this->battery_handle) {
        this->publish_battery_level_(p_data->notify.value,
                                     p_data->notify.value_len);
      } else if (this->handle_report_reference_.count(p_data->notify.handle) != 0) {
        this->send_input_report_event(p_data);
      } else if (this->debug_characteristics_.count(p_data->notify.handle) != 0) {
        this->log_debug_characteristic_value_(p_data->notify.handle,
                                              p_data->notify.value,
                                              p_data->notify.value_len,
                                              "notify");
      } else {
        ESP_LOGV(TAG, "Ignoring notify from unknown handle %d", p_data->notify.handle);
      }
      break;
    }
    case ESP_GATTC_REG_FOR_NOTIFY_EVT: {
      auto pending = std::find(this->handles_registered_for_notify.begin(),
                               this->handles_registered_for_notify.end(),
                               param->reg_for_notify.handle);
      if (pending == this->handles_registered_for_notify.end()) break;
      if (param->reg_for_notify.status != ESP_GATT_OK) {
        ESP_LOGW(TAG, "Notification registration failed for handle %u with status=%d",
                 param->reg_for_notify.handle, param->reg_for_notify.status);
      }
      this->handles_registered_for_notify.erase(pending);
      if (this->handles_waiting_for_notify_registration > 0) {
        this->handles_waiting_for_notify_registration--;
      }
      if (this->handles_waiting_for_notify_registration == 0) {
        this->hid_state = HIDState::NOTIFICATIONS_REGISTERED;
      }
      break;
    }
    default: {
      break;
    }
  }
}

std::string BLEClientHID::format_usage_code_(const HIDUsage &usage) const {
  return std::to_string(usage.page) + "_" + std::to_string(usage.usage);
}

std::string BLEClientHID::format_uuid_(const espbt::ESPBTUUID &uuid) const {
  char uuid_str[esphome::esp32_ble::UUID_STR_LEN];
  uuid.to_str(uuid_str);
  return std::string(uuid_str);
}

std::string BLEClientHID::lookup_usage_name_(const HIDUsage &usage) const {
  auto page_it = USAGE_PAGES.find(usage.page);
  if (page_it == USAGE_PAGES.end()) {
    return this->format_usage_code_(usage);
  }

  auto usage_it = page_it->second.usages_.find(usage.usage);
  if (usage_it == page_it->second.usages_.end()) {
    return this->format_usage_code_(usage);
  }

  return usage_it->second;
}

std::string BLEClientHID::resolve_usage_name_(const std::string &event_code, const HIDUsage &usage) const {
  auto override_it = this->overrides_.find(event_code);
  if (override_it != this->overrides_.end()) {
    return override_it->second;
  }

  return this->lookup_usage_name_(usage);
}

void BLEClientHID::log_gatt_services_() {
  const auto &services = get_ble_client_services(this->parent());
  ESP_LOGV(TAG, "Discovered %u GATT services",
           static_cast<unsigned>(services.size()));
  for (auto *service : services) {
    if (service == nullptr) {
      continue;
    }

    const std::string service_uuid = this->format_uuid_(service->uuid);
    ESP_LOGV(TAG,
             "GATT service uuid=%s start_handle=%u end_handle=%u characteristics=%u",
             service_uuid.c_str(), service->start_handle, service->end_handle,
             static_cast<unsigned>(service->characteristics.size()));

    for (auto *characteristic : service->characteristics) {
      if (characteristic == nullptr) {
        continue;
      }

      const std::string characteristic_uuid =
          this->format_uuid_(characteristic->uuid);
      ESP_LOGV(
          TAG,
          "GATT characteristic service=%s handle=%u uuid=%s properties=0x%X descriptors=%u",
          service_uuid.c_str(), characteristic->handle,
          characteristic_uuid.c_str(), characteristic->properties,
          static_cast<unsigned>(characteristic->descriptors.size()));

      for (auto *descriptor : characteristic->descriptors) {
        if (descriptor == nullptr) {
          continue;
        }

        const std::string descriptor_uuid = this->format_uuid_(descriptor->uuid);
        ESP_LOGV(TAG,
                 "GATT descriptor characteristic_handle=%u handle=%u uuid=%s",
                 characteristic->handle, descriptor->handle,
                 descriptor_uuid.c_str());
      }
    }
  }
}

void BLEClientHID::publish_battery_level_(const uint8_t *value,
                                          uint16_t value_len) {
  if (this->battery_sensor == nullptr) {
    return;
  }
  if (value == nullptr || value_len == 0) {
    ESP_LOGW(TAG, "Ignoring empty battery update");
    return;
  }
  this->battery_sensor->publish_state(value[0]);
}

bool BLEClientHID::is_component_managed_characteristic_(
    const ble_client::BLECharacteristic *characteristic) const {
  if (characteristic == nullptr) {
    return false;
  }

  const auto uuid = characteristic->uuid.get_uuid();
  if (uuid.len != ESP_UUID_LEN_16) {
    return false;
  }

  switch (uuid.uuid.uuid16) {
    case ESP_GATT_UUID_GAP_DEVICE_NAME:
    case ESP_GATT_UUID_GAP_PREF_CONN_PARAM:
    case ESP_GATT_UUID_PNP_ID:
    case ESP_GATT_UUID_MANU_NAME:
    case ESP_GATT_UUID_SERIAL_NUMBER_STR:
    case ESP_GATT_UUID_BATTERY_LEVEL:
    case ESP_GATT_UUID_HID_REPORT_MAP:
    case ESP_GATT_UUID_HID_REPORT:
      return true;
    default:
      return false;
  }
}

void BLEClientHID::track_debug_characteristic_(
    const ble_client::BLEService *service,
    const ble_client::BLECharacteristic *characteristic) {
  if (service == nullptr || characteristic == nullptr) {
    return;
  }

  DebugCharacteristicInfo info;
  info.service_uuid = this->format_uuid_(service->uuid);
  info.characteristic_uuid = this->format_uuid_(characteristic->uuid);
  info.service_start_handle = service->start_handle;
  info.service_end_handle = service->end_handle;
  info.properties = characteristic->properties;
  this->debug_characteristics_[characteristic->handle] = info;
}

void BLEClientHID::register_for_notify_(ble_client::BLECharacteristic *characteristic,
                                        const char *purpose) {
  if (characteristic == nullptr) {
    return;
  }

  const uint8_t notify_properties =
      ESP_GATT_CHAR_PROP_BIT_NOTIFY | ESP_GATT_CHAR_PROP_BIT_INDICATE;
  if ((characteristic->properties & notify_properties) == 0) {
    return;
  }

  if (std::find(this->handles_registered_for_notify.begin(),
                this->handles_registered_for_notify.end(),
                characteristic->handle) !=
      this->handles_registered_for_notify.end()) {
    return;
  }

  auto status = esp_ble_gattc_register_for_notify(
      this->parent()->get_gattc_if(), this->parent()->get_remote_bda(),
      characteristic->handle);

  if (status != ESP_OK) {
    ESP_LOGW(TAG, "Register for notify failed for %s handle %d with status=%d",
             purpose, characteristic->handle, status);
    return;
  }

  this->handles_registered_for_notify.push_back(characteristic->handle);
  this->handles_waiting_for_notify_registration++;
  ESP_LOGV(TAG, "Registered notify debug for %s handle=%u", purpose,
           characteristic->handle);
}

void BLEClientHID::log_debug_characteristic_value_(uint16_t handle,
                                                   const uint8_t *value,
                                                   uint16_t value_len,
                                                   const char *source) const {
  auto debug_it = this->debug_characteristics_.find(handle);
  if (debug_it == this->debug_characteristics_.end()) {
    return;
  }

  if (value == nullptr || value_len == 0) {
    ESP_LOGV(TAG, "Unmapped characteristic %s handle=%u empty", source, handle);
    return;
  }

  std::vector<char> raw_value(format_hex_pretty_size(value_len));
  format_hex_pretty_to(raw_value.data(), raw_value.size(), value, value_len,
                       ' ');

  const DebugCharacteristicInfo &info = debug_it->second;
  ESP_LOGV(
      TAG,
      "Unmapped characteristic %s handle=%u service=%s range=%u-%u uuid=%s properties=0x%X len=%u raw=%s",
      source, handle, info.service_uuid.c_str(), info.service_start_handle,
      info.service_end_handle, info.characteristic_uuid.c_str(),
      info.properties, value_len, raw_value.data());
}

void BLEClientHID::handle_hid_report_(uint16_t handle, const uint8_t *value,
                                      uint16_t value_len, const char *source,
                                      bool publish_input_events) {
  auto report_ref_it = this->handle_report_reference_.find(handle);
  if (report_ref_it == this->handle_report_reference_.end()) {
    ESP_LOGV(TAG, "No HID report reference known for handle %d", handle);
    return;
  }
  if (this->hid_report_map == nullptr) {
    ESP_LOGV(TAG, "Cannot parse HID %s report for handle %d without a report map",
             source, handle);
    return;
  }
  if (value == nullptr || value_len == 0) {
    ESP_LOGV(TAG, "Ignoring empty HID %s report for handle %d", source, handle);
    return;
  }

  const HIDReportReference &report_ref = report_ref_it->second;
  std::vector<char> raw_report(format_hex_pretty_size(value_len));
  format_hex_pretty_to(raw_report.data(), raw_report.size(), value, value_len, ' ');
  ESP_LOGV(TAG,
           "HID %s report handle=%d report_id=%u type=%s len=%u raw=%s",
           source, handle, report_ref.report_id,
           hid_report_type_to_string(report_ref.report_type), value_len,
           raw_report.data());

  uint8_t *data = new uint8_t[value_len + 1];
  memcpy(data + 1, value, value_len);
  data[0] = report_ref.report_id;
  std::vector<HIDReportItemValue> hid_report_values =
      this->hid_report_map->parse(report_ref.report_type, data);
  if (hid_report_values.empty()) {
    ESP_LOGV(TAG,
             "No parsed HID items for handle=%d report_id=%u type=%s via %s",
             handle, report_ref.report_id,
             hid_report_type_to_string(report_ref.report_type), source);
    delete[] data;
    return;
  }

  for (const HIDReportItemValue &value_item : hid_report_values) {
    const std::string event_code = this->format_usage_code_(value_item.usage);
    const std::string usage_name =
        this->resolve_usage_name_(event_code, value_item.usage);
    ESP_LOGV(TAG,
             "Parsed HID %s report handle=%d report_id=%u type=%s code=%s name=%s value=%ld raw_value=%ld",
             source, handle, report_ref.report_id,
             hid_report_type_to_string(report_ref.report_type),
             event_code.c_str(), usage_name.c_str(), value_item.value,
             value_item.raw_value);

    if (report_ref.report_type != HID_REPORT_TYPE_INPUT ||
        !publish_input_events) {
      continue;
    }

    #if defined(USE_API) && defined(USE_API_HOMEASSISTANT_SERVICES) && defined(USE_BLE_CLIENT_HID_HOMEASSISTANT_EVENT)
    if (this->homeassistant_event_enabled_) {
      this->fire_homeassistant_event("esphome.hid_events",
                                     {{"code", event_code},
                                      {"name", usage_name},
                                      {"value", std::to_string(value_item.value)}});
      ESP_LOGD(TAG, "Sent HID event to Home Assistant: code: %s, name: %s, value: %ld",
               event_code.c_str(), usage_name.c_str(), value_item.value);
    }
    #endif
    if(this->last_event_usage_text_sensor != nullptr){
      this->last_event_usage_text_sensor->publish_state(usage_name);
    }
    if(this->last_event_code_text_sensor != nullptr){
      this->last_event_code_text_sensor->publish_state(event_code);
    }
    if (this->last_event_value_sensor != nullptr) {
      this->last_event_value_sensor->publish_state(value_item.value);
    }
    this->event_callback_.call(event_code, usage_name, value_item.value);
    ESP_LOGI(TAG, "Received HID event: code: %s, name: %s, value: %ld",
             event_code.c_str(), usage_name.c_str(), value_item.value);
  }

  delete[] data;
}

void BLEClientHID::send_input_report_event(esp_ble_gattc_cb_param_t *p_data) {
  this->handle_hid_report_(p_data->notify.handle, p_data->notify.value,
                           p_data->notify.value_len, "notify", true);
}

void BLEClientHID::register_last_event_value_sensor(
    sensor::Sensor *last_event_value_sensor) {
  this->last_event_value_sensor = last_event_value_sensor;
}

void BLEClientHID::register_battery_sensor(sensor::Sensor *battery_sensor) {
  this->battery_sensor = battery_sensor;
}

void BLEClientHID::set_homeassistant_event_enabled(
    bool homeassistant_event_enabled) {
  this->homeassistant_event_enabled_ = homeassistant_event_enabled;
}

void BLEClientHID::set_debug_unmapped_characteristics(
    bool debug_unmapped_characteristics) {
  this->debug_unmapped_characteristics_ = debug_unmapped_characteristics;
}

void BLEClientHID::register_last_event_usage_text_sensor(
    text_sensor::TextSensor *last_event_usage_text_sensor) {
  this->last_event_usage_text_sensor = last_event_usage_text_sensor;
}

void BLEClientHID::register_last_event_code_text_sensor(
    text_sensor::TextSensor *last_event_code_text_sensor) {
  this->last_event_code_text_sensor = last_event_code_text_sensor;
}

void BLEClientHID::schedule_read_char(
    ble_client::BLECharacteristic *characteristic) {
  if (characteristic == nullptr) {
    ESP_LOGW(TAG, "characteristic not found");
    return;
  }
  if ((characteristic->properties & ESP_GATT_CHAR_PROP_BIT_READ) == 0) {
    ESP_LOGV(TAG, "Skipping non-readable characteristic handle=%u",
             characteristic->handle);
    return;
  }
  if (this->handles_to_read.count(characteristic->handle) != 0) {
    return;
  }
  this->handles_to_read.insert(std::make_pair(characteristic->handle, nullptr));
  this->read_queue_.push_back(
      {characteristic->handle, GATTReadType::CHARACTERISTIC, 0});
}

void BLEClientHID::schedule_read_descriptor_(
    ble_client::BLEDescriptor *descriptor) {
  if (descriptor == nullptr ||
      this->handles_to_read.count(descriptor->handle) != 0) {
    return;
  }
  this->handles_to_read.insert(std::make_pair(descriptor->handle, nullptr));
  this->read_queue_.push_back(
      {descriptor->handle, GATTReadType::DESCRIPTOR, 0});
}

void BLEClientHID::start_next_read_() {
  if (this->read_in_flight_) {
    return;
  }
  if (this->read_queue_index_ >= this->read_queue_.size()) {
    ESP_LOGD(TAG, "Finished %u queued GATT reads",
             static_cast<unsigned>(this->read_queue_.size()));
    this->hid_state = HIDState::READ_CHARS;
    return;
  }
  if (this->read_retry_at_ != 0 &&
      static_cast<int32_t>(millis() - this->read_retry_at_) < 0) {
    return;
  }

  auto &request = this->read_queue_[this->read_queue_index_];
  esp_err_t status;
  if (request.type == GATTReadType::CHARACTERISTIC) {
    status = esp_ble_gattc_read_char(
        this->parent()->get_gattc_if(), this->parent()->get_conn_id(),
        request.handle, ESP_GATT_AUTH_REQ_NO_MITM);
  } else {
    status = esp_ble_gattc_read_char_descr(
        this->parent()->get_gattc_if(), this->parent()->get_conn_id(),
        request.handle, ESP_GATT_AUTH_REQ_NO_MITM);
  }

  if (status == ESP_OK) {
    this->read_in_flight_ = true;
    this->read_retry_at_ = 0;
    ESP_LOGV(TAG, "Started GATT %s read for handle %u",
             request.type == GATTReadType::CHARACTERISTIC ? "characteristic"
                                                          : "descriptor",
             request.handle);
    return;
  }

  if (request.retries < MAX_READ_RETRIES) {
    request.retries++;
    this->read_retry_at_ = millis() + READ_RETRY_DELAY_MS;
    ESP_LOGW(TAG,
             "Starting GATT read for handle %u failed with status=%d; retry %u/%u",
             request.handle, status, static_cast<unsigned>(request.retries),
             static_cast<unsigned>(MAX_READ_RETRIES));
    return;
  }

  ESP_LOGW(TAG,
           "Starting GATT read for handle %u failed with status=%d; skipping",
           request.handle, status);
  this->read_queue_index_++;
  this->read_retry_at_ = 0;
}

void BLEClientHID::handle_read_failure_(esp_gatt_status_t status) {
  auto &request = this->read_queue_[this->read_queue_index_];
  if (this->is_transient_read_status_(status) &&
      request.retries < MAX_READ_RETRIES) {
    request.retries++;
    this->read_retry_at_ = millis() + READ_RETRY_DELAY_MS;
    ESP_LOGW(TAG,
             "GATT read failed for handle %u with status=%d; retry %u/%u",
             request.handle, status, static_cast<unsigned>(request.retries),
             static_cast<unsigned>(MAX_READ_RETRIES));
    return;
  }

  ESP_LOGW(TAG, "GATT read failed for handle %u with status=%d; skipping",
           request.handle, status);
  this->read_queue_index_++;
  this->read_retry_at_ = 0;
}

bool BLEClientHID::is_transient_read_status_(esp_gatt_status_t status) const {
  return status == ESP_GATT_INSUF_RESOURCE || status == ESP_GATT_NO_RESOURCES ||
         status == ESP_GATT_BUSY || status == ESP_GATT_PENDING ||
         status == ESP_GATT_CONGESTED;
}

void BLEClientHID::reset_read_state_() {
  for (auto &entry : this->handles_to_read) {
    delete entry.second;
  }
  this->handles_to_read.clear();
  this->read_queue_.clear();
  this->read_queue_index_ = 0;
  this->read_in_flight_ = false;
  this->read_retry_at_ = 0;
}

GATTReadData *BLEClientHID::get_read_data_(uint16_t handle) const {
  auto read = this->handles_to_read.find(handle);
  if (read == this->handles_to_read.end()) {
    return nullptr;
  }
  return read->second;
}

uint8_t *BLEClientHID::parse_characteristic_data(
    ble_client::BLEService *service, uint16_t uuid) {
  using namespace ble_client;
  BLECharacteristic *characteristic = service->get_characteristic(uuid);
  if (characteristic == nullptr) {
    ESP_LOGD(TAG, "No characteristic with uuid %#X found on device", uuid);
    return nullptr;
  }
  GATTReadData *data = this->get_read_data_(characteristic->handle);
  if (data != nullptr && data->value_len_ > 0) {
    ESP_LOGD(
        TAG,
        "Characteristic parsed for uuid %#X and handle %#X starts with %#X",
        uuid, characteristic->handle, *(data->value_));
    return data->value_;
  }
  ESP_LOGD(TAG,
           "Characteristic with uuid %#X and handle %#X not stored in "
           "handles_to_read",
           uuid, characteristic->handle);
  return nullptr;
}

bool BLEClientHID::configure_hid_client() {
  using namespace ble_client;
  BLEService *battery_service =
      this->parent()->get_service(ESP_GATT_UUID_BATTERY_SERVICE_SVC);
  BLEService *device_info_service =
      this->parent()->get_service(ESP_GATT_UUID_DEVICE_INFO_SVC);
  BLEService *hid_service = this->parent()->get_service(ESP_GATT_UUID_HID_SVC);
  BLEService *generic_access_service = this->parent()->get_service(0x1800);

  BLECharacteristic *hid_report_map_char =
      hid_service == nullptr
          ? nullptr
          : hid_service->get_characteristic(ESP_GATT_UUID_HID_REPORT_MAP);
  GATTReadData *hid_report_map_data =
      hid_report_map_char == nullptr
          ? nullptr
          : this->get_read_data_(hid_report_map_char->handle);
  if (hid_report_map_data == nullptr || hid_report_map_data->value_len_ == 0) {
    ESP_LOGE(TAG, "Required HID Report Map could not be read");
    this->status_set_warning("HID Report Map unavailable");
    this->reset_read_state_();
    return false;
  }

  ESP_LOGD(TAG, "Parse HID Report Map");
  HIDReportMap::esp_logd_report_map(hid_report_map_data->value_,
                                    hid_report_map_data->value_len_);
  this->hid_report_map = HIDReportMap::parse_report_map_data(
      hid_report_map_data->value_, hid_report_map_data->value_len_);
  if (this->hid_report_map == nullptr) {
    ESP_LOGE(TAG, "Required HID Report Map could not be parsed");
    this->status_set_warning("Invalid HID Report Map");
    this->reset_read_state_();
    return false;
  }
  ESP_LOGD(TAG, "Parse HID Report Map Done");

  if (generic_access_service != nullptr) {
    uint8_t *t_device_name = this->parse_characteristic_data(
        generic_access_service, ESP_GATT_UUID_GAP_DEVICE_NAME);
    if (t_device_name != nullptr) {
      this->device_name = (const char *)t_device_name;
    } else {
      this->device_name = "Generic";
    }
  }
  if (this->battery_sensor != nullptr && battery_service != nullptr) {
    BLECharacteristic *battery_level_char =
        battery_service->get_characteristic(ESP_GATT_UUID_BATTERY_LEVEL);
    if (battery_level_char != nullptr) {
      this->battery_handle = battery_level_char->handle;
      this->register_for_notify_(battery_level_char, "battery characteristic");
    }
  }
  if (device_info_service != nullptr) {
    BLECharacteristic *pnp_id_char =
        device_info_service->get_characteristic(ESP_GATT_UUID_PNP_ID);
    GATTReadData *pnp_id_data =
        pnp_id_char == nullptr ? nullptr
                               : this->get_read_data_(pnp_id_char->handle);
    if (pnp_id_data != nullptr && pnp_id_data->value_len_ >= 7) {
      const uint8_t *rdata = pnp_id_data->value_;
      this->vendor_id = rdata[1] | (rdata[2] << 8);
      this->product_id = rdata[3] | (rdata[4] << 8);
      this->version = rdata[5] | (rdata[6] << 8);
    } else {
      ESP_LOGV(TAG, "No valid PnP ID value available");
    }

    uint8_t *t_manufacturer = this->parse_characteristic_data(
        device_info_service, ESP_GATT_UUID_MANU_NAME);
    if (t_manufacturer != nullptr) {
      this->manufacturer = (const char *)t_manufacturer;
    } else {
      this->manufacturer = "Generic";
    }

    uint8_t *t_serial = this->parse_characteristic_data(
        device_info_service, ESP_GATT_UUID_SERIAL_NUMBER_STR);
    if (t_serial != nullptr) {
      this->serial_number = (const char *)t_serial;
    } else {
      this->serial_number = "000000";
    }
  }
  if (hid_service != nullptr) {
    std::vector<BLECharacteristic *> chars = hid_service->characteristics;
    for (BLECharacteristic *hid_char : chars) {
      if (hid_char->uuid.get_uuid().uuid.uuid16 == ESP_GATT_UUID_HID_REPORT) {
        HIDReportReference report_ref;
        this->register_for_notify_(hid_char, "hid report");
        BLEDescriptor *rpt_ref_desc =
            hid_char->get_descriptor(ESP_GATT_UUID_RPT_REF_DESCR);
        bool has_report_ref = false;
        if (rpt_ref_desc != nullptr &&
            this->handles_to_read.count(rpt_ref_desc->handle) != 0 &&
            this->handles_to_read[rpt_ref_desc->handle] != nullptr &&
            this->handles_to_read[rpt_ref_desc->handle]->value_len_ >= 2) {
          report_ref.report_id =
              this->handles_to_read[rpt_ref_desc->handle]->value_[0];
          report_ref.report_type =
              this->handles_to_read[rpt_ref_desc->handle]->value_[1];
          has_report_ref = true;
        }
        if (!has_report_ref) {
          ESP_LOGV(TAG,
                   "No valid report reference descriptor for handle %d, defaulting to input report id 0",
                   hid_char->handle);
          report_ref.report_id = 0;
          report_ref.report_type = HID_REPORT_TYPE_INPUT;
        }
        this->handle_report_reference_[hid_char->handle] = report_ref;
        ESP_LOGV(TAG,
                 "HID report characteristic handle=%d report_id=%u type=%s properties=0x%X",
                 hid_char->handle, report_ref.report_id,
                 hid_report_type_to_string(report_ref.report_type),
                 hid_char->properties);
        if (this->handles_to_read.count(hid_char->handle) != 0 &&
            this->handles_to_read[hid_char->handle] != nullptr) {
          this->handle_hid_report_(
              hid_char->handle, this->handles_to_read[hid_char->handle]->value_,
              this->handles_to_read[hid_char->handle]->value_len_, "read",
              false);
        }
      }
    }
  }
  if (this->debug_unmapped_characteristics_) {
    for (auto *service : get_ble_client_services(this->parent())) {
      if (service == nullptr) {
        continue;
      }
      for (auto *characteristic : service->characteristics) {
        if (characteristic == nullptr ||
            this->debug_characteristics_.count(characteristic->handle) == 0) {
          continue;
        }
        this->register_for_notify_(characteristic,
                                   "unmapped characteristic");
      }
    }
  }
  this->reset_read_state_();
  return true;
}

}  // namespace ble_client_hid
}  // namespace esphome
#endif
