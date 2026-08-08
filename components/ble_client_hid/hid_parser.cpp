#include <stack>
#include <map>
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

#include "hid_report_data.h"
#include "hid_parser.h"
#include "usages.h"

namespace esphome
{
  namespace ble_client_hid
  {

    static const char *const TAG = "hid_parser";

    const char *hid_report_type_to_string(uint8_t report_type)
    {
      switch (report_type)
      {
      case HID_REPORT_TYPE_INPUT:
        return "input";
      case HID_REPORT_TYPE_OUTPUT:
        return "output";
      case HID_REPORT_TYPE_FEATURE:
        return "feature";
      default:
        return "other";
      }
    }

    static const char *hid_collection_type_to_string(uint8_t collection_type)
    {
      switch (collection_type)
      {
      case 0x00:
        return "Physical";
      case 0x01:
        return "Application";
      case 0x02:
        return "Logical";
      case 0x03:
        return "Report";
      case 0x04:
        return "NamedArray";
      case 0x05:
        return "UsageSwitch";
      case 0x06:
        return "UsageModifier";
      default:
        return "Reserved";
      }
    }

    static std::string format_usage_debug(const HIDUsage &usage)
    {
      auto page_it = USAGE_PAGES.find(usage.page);
      if (page_it == USAGE_PAGES.end())
      {
        return std::to_string(usage.page) + "_" + std::to_string(usage.usage);
      }

      auto usage_it = page_it->second.usages_.find(usage.usage);
      if (usage_it == page_it->second.usages_.end())
      {
        return std::string(page_it->second.name_) + ":" + std::to_string(usage.usage);
      }

      return std::string(page_it->second.name_) + ":" + usage_it->second;
    }

    static bool hid_item_value_is_signed(uint8_t report_item_info, const HIDStateTable &state_table)
    {
      switch (report_item_info & (HID_ITEM_TYPE_MASK | HID_ITEM_TAG_MASK))
      {
      case HID_ITEM_TYPE_TAG_LOGICAL_MINIMUM:
      case HID_ITEM_TYPE_TAG_PHYSICAL_MINIMUM:
      case HID_ITEM_TYPE_TAG_PHYSICAL_MAXIMUM:
        return true;
      case HID_ITEM_TYPE_TAG_LOGICAL_MAXIMUM:
        return state_table.logical_range.minimum < 0;
      default:
        return false;
      }
    }

    static int32_t sign_extend_hid_value(uint32_t value, uint8_t bit_width)
    {
      if (bit_width == 0 || bit_width >= 32)
      {
        return static_cast<int32_t>(value);
      }

      const uint32_t sign_bit = 1UL << (bit_width - 1);
      if ((value & sign_bit) == 0)
      {
        return static_cast<int32_t>(value);
      }

      const uint32_t extend_mask = ~((1UL << bit_width) - 1UL);
      return static_cast<int32_t>(value | extend_mask);
    }

    // requires at least C++11
    const std::string vformat(const char *const zcFormat, ...)
    {

      // initialize use of the variable argument array
      va_list vaArgs;
      va_start(vaArgs, zcFormat);

      // reliably acquire the size
      // from a copy of the variable argument array
      // and a functionally reliable call to mock the formatting
      va_list vaArgsCopy;
      va_copy(vaArgsCopy, vaArgs);
      const int iLen = std::vsnprintf(NULL, 0, zcFormat, vaArgsCopy);
      va_end(vaArgsCopy);

      // return a formatted string without risking memory mismanagement
      // and without assuming any compiler or platform specific behavior
      std::vector<char> zc(iLen + 1);
      std::vsnprintf(zc.data(), zc.size(), zcFormat, vaArgs);
      va_end(vaArgs);
      return std::string(zc.data(), iLen);
    }
    void HIDReportMap::esp_logd_report_map(const uint8_t *report_map_data, uint16_t report_map_size)
    {
      ESP_LOGD(TAG, "Report Map:");
      while (report_map_size > 0)
      {
        uint8_t report_item_info = report_map_data[0];
        report_map_size--;
        report_map_data++;
        switch (report_item_info & HID_ITEM_SIZE_MASK)
        {
        case HID_ITEM_SIZE_32:
          ESP_LOGD(TAG, "%X, %X, %X, %X, %X", report_item_info, report_map_data[0], report_map_data[1], report_map_data[2], report_map_data[3]);
          report_map_data += 4;
          report_map_size -= 4;
          break;
        case HID_ITEM_SIZE_16:
          ESP_LOGD(TAG, "%X, %X, %X", report_item_info, report_map_data[0], report_map_data[1]);
          report_map_data += 2;
          report_map_size -= 2;
          break;
        case HID_ITEM_SIZE_8:
          ESP_LOGD(TAG, "%X, %X", report_item_info, report_map_data[0]);
          report_map_data += 1;
          report_map_size -= 1;
          break;
        case HID_ITEM_SIZE_0:
          ESP_LOGD(TAG, "%X", report_item_info);
          break;
        }
      }
    }

    int32_t HIDReportMap::parse_item(const uint8_t **p_report_map_data, uint16_t *report_map_size,
                                     uint8_t report_item_info, bool signed_value)
    {
      uint32_t report_item_data = 0;
      uint8_t item_size_bits = 0;

      switch (report_item_info & HID_ITEM_SIZE_MASK)
      {
      case HID_ITEM_SIZE_32:
        item_size_bits = 32;
        report_item_data =
            (((uint32_t)(*p_report_map_data)[3] << 24) |
             ((uint32_t)(*p_report_map_data)[2] << 16) |
             ((uint16_t)(*p_report_map_data)[1] << 8) | (*p_report_map_data)[0]);
        (*report_map_size) -= 4;
        (*p_report_map_data) += 4;
        break;

      case HID_ITEM_SIZE_16:
        item_size_bits = 16;
        report_item_data =
            (((uint16_t)(*p_report_map_data)[1] << 8) | ((*p_report_map_data)[0]));
        (*report_map_size) -= 2;
        (*p_report_map_data) += 2;
        break;

      case HID_ITEM_SIZE_8:
        item_size_bits = 8;
        report_item_data = (*p_report_map_data)[0];
        (*report_map_size) -= 1;
        (*p_report_map_data) += 1;
        break;

      default:
        return 0;
      }

      if (signed_value)
      {
        return sign_extend_hid_value(report_item_data, item_size_bits);
      }

      return static_cast<int32_t>(report_item_data);
    }

    static const HIDUsage parse_usage(uint8_t item_info, uint32_t data, uint16_t usage_page)
    {
      if ((item_info & HID_ITEM_SIZE_MASK) == HID_ITEM_SIZE_32)
      {
        ESP_LOGD(TAG,"Parsing extedned usage: %X, %X", (uint16_t)(data >> 16), (uint16_t)data);
        return HIDUsage((uint16_t)data, (uint16_t)(data >> 16));
      }
      ESP_LOGD(TAG,"Parsing simple usage: %X, %X", usage_page, (uint16_t)data);
      return HIDUsage((uint16_t)data, usage_page);
    }

    const HIDUsage HIDUsageRange::get_usage(uint16_t index) const
    {
      if (index > this->usage_max.usage - this->usage_min.usage)
      {
        ESP_LOGW(TAG,
                 "Usage index %u out of range for page %u usage range [%u, %u]",
                 static_cast<unsigned>(index),
                 static_cast<unsigned>(this->usage_page),
                 static_cast<unsigned>(this->usage_min.usage),
                 static_cast<unsigned>(this->usage_max.usage));
        return HIDUsage(index,0);
      }
      return HIDUsage(this->usage_min.usage + index, this->usage_page);
    }

    const HIDUsage HIDUsageList::get_usage(uint16_t index) const
    {
      ESP_LOGD(TAG, "get usage for index %d with list size %d", index, this->usages.size());
      if (index >= this->usages.size())
      {
        ESP_LOGW(TAG, "Usage index %u out of range for usage list size %u",
                 static_cast<unsigned>(index),
                 static_cast<unsigned>(this->usages.size()));
        return HIDUsage(index,0);;
      }
      return this->usages[index];
    }

    HIDReportMap *HIDReportMap::parse_report_map_data(
        const uint8_t *report_map_data, uint16_t report_map_size)
    {
      HIDStateTable state_table = {};
      std::stack<HIDStateTable> parser_states;
      HIDUsageRangeLimits usage_range = {};
      std::vector<HIDUsage> usages;
      std::map<uint16_t, HIDInputReport *> reports;
      std::vector<std::pair<HIDUsage, uint8_t>> collection_stack;

      while (report_map_size)
      {
        uint8_t report_item_info = report_map_data[0];

        report_map_data++;
        report_map_size--;

        bool signed_value = hid_item_value_is_signed(report_item_info, state_table);
        uint32_t report_item_data = HIDReportMap::parse_item(
            &report_map_data, &report_map_size, report_item_info, signed_value);
        switch (report_item_info & (HID_ITEM_TYPE_MASK | HID_ITEM_TAG_MASK))
        {
        case HID_ITEM_TYPE_TAG_PUSH:
        {

          parser_states.push(state_table);
          break;
        }
        case HID_ITEM_TYPE_TAG_POP:
        {
          if (parser_states.size() <= 0)
          {
            ESP_LOGW(TAG,
                     "No parser state in HID parser states stack, error in HID "
                     "report map");
            return nullptr;
          }
          state_table = parser_states.top();
          parser_states.pop();
          break;
        }

        case HID_ITEM_TYPE_TAG_USAGE_PAGE:
        {
          ESP_LOGD(TAG, "Usage page: %lX", report_item_data);
          state_table.usage_page = report_item_data;
          break;
        }

        case HID_ITEM_TYPE_TAG_LOGICAL_MINIMUM:
        {
          state_table.logical_range.minimum = report_item_data;
          break;
        }

        case HID_ITEM_TYPE_TAG_LOGICAL_MAXIMUM:
        {
          state_table.logical_range.maximum = report_item_data;
          break;
        }

        case HID_ITEM_TYPE_TAG_PHYSICAL_MINIMUM:
          // Ignore for now
          break;

        case HID_ITEM_TYPE_TAG_PHYSICAL_MAXIMUM:
          // Ignore for now
          break;

        case HID_ITEM_TYPE_TAG_UNIT_EXPONENT:
          // Ignore for now
          break;

        case HID_ITEM_TYPE_TAG_UNIT:
          // Ignore for now
          break;

        case HID_ITEM_TYPE_TAG_REPORT_SIZE:
        {
          state_table.report_size = report_item_data;
          break;
        }

        case HID_ITEM_TYPE_TAG_REPORT_COUNT:
        {
          state_table.report_count = report_item_data;
          break;
        }

        case HID_ITEM_TYPE_TAG_REPORT_ID:
        {
          state_table.report_id = report_item_data;
          ESP_LOGV(TAG, "Active report ID changed to %u", state_table.report_id);
          break;
        }

        case HID_ITEM_TYPE_TAG_USAGE:
        {
          usages.push_back(parse_usage(report_item_info, report_item_data, state_table.usage_page));
          break;
        }

        case HID_ITEM_TYPE_TAG_USAGE_MINIMUM:
        {
          usage_range.minimum = parse_usage(report_item_info, report_item_data, state_table.usage_page);
          break;
        }

        case HID_ITEM_TYPE_TAG_USAGE_MAXIMUM:
        {
          usage_range.maximum = parse_usage(report_item_info, report_item_data, state_table.usage_page);
          break;
        }

        case HID_ITEM_TYPE_TAG_COLLECTION:
        {
          HIDUsage collection_usage = usages.empty() ? HIDUsage(0, state_table.usage_page) : usages.back();
          collection_stack.emplace_back(collection_usage, static_cast<uint8_t>(report_item_data));
          std::string indent(collection_stack.size() * 2, ' ');
          ESP_LOGV(TAG, "%sCollection start: type=%s usage=%s", indent.c_str(),
                   hid_collection_type_to_string(static_cast<uint8_t>(report_item_data)),
                   format_usage_debug(collection_usage).c_str());
          break;
        }

        case HID_ITEM_TYPE_TAG_END_COLLECTION:
        {
          if (collection_stack.empty())
          {
            ESP_LOGW(TAG, "End collection without a matching collection start");
            break;
          }
          std::string indent(collection_stack.size() * 2, ' ');
          ESP_LOGV(TAG, "%sCollection end: type=%s usage=%s", indent.c_str(),
                   hid_collection_type_to_string(collection_stack.back().second),
                   format_usage_debug(collection_stack.back().first).c_str());
          collection_stack.pop_back();
          break;
        }

        case HID_ITEM_TYPE_TAG_INPUT:
        case HID_ITEM_TYPE_TAG_OUTPUT:
        case HID_ITEM_TYPE_TAG_FEATURE:
        {
          uint8_t report_type = HID_REPORT_TYPE_INPUT;
          if ((report_item_info & (HID_ITEM_TYPE_MASK | HID_ITEM_TAG_MASK)) == HID_ITEM_TYPE_TAG_OUTPUT)
          {
            report_type = HID_REPORT_TYPE_OUTPUT;
          }
          else if ((report_item_info & (HID_ITEM_TYPE_MASK | HID_ITEM_TAG_MASK)) == HID_ITEM_TYPE_TAG_FEATURE)
          {
            report_type = HID_REPORT_TYPE_FEATURE;
          }

          ESP_LOGD(TAG, "Found %s main item", hid_report_type_to_string(report_type));
          uint16_t item_flags = report_item_data;

          uint16_t report_key = HIDReportMap::make_report_key_(state_table.report_id, report_type);
          if (reports.count(report_key) == 0)
          {
            reports.emplace(report_key, new HIDInputReport(state_table.report_id, report_type));
          }

          HIDInputReport *input_report = reports.at(report_key);
          std::string indent(collection_stack.size() * 2, ' ');
          ESP_LOGV(TAG,
                   "%sRegistering %s report id=%u count=%u size=%u usage_page=%u collection_depth=%u",
                   indent.c_str(), hid_report_type_to_string(report_type), state_table.report_id,
                   state_table.report_count, state_table.report_size, state_table.usage_page,
                   collection_stack.size());
          if (item_flags & HID_IOF_CONSTANT)
          {
            ESP_LOGD(TAG, "Parsed %s report item of type: constant", hid_report_type_to_string(report_type));
            input_report->add_padding(state_table.report_size);
            break;
          }

          HIDUsageCollection *usage_collection;
          if (usages.size() > 0)
          {
            usage_collection = new HIDUsageList(usages);
          }
          else
          {
            ESP_LOGD(TAG, "Creating usage range with min: %d, max: %d, page: %d", usage_range.minimum.usage, usage_range.maximum.usage, usage_range.minimum.page);
            usage_collection = new HIDUsageRange(usage_range.minimum, usage_range.maximum, usage_range.minimum.page);
          }
          if (item_flags & HID_IOF_VARIABLE)
          {
            input_report->push_back(new HIDInputReportVariable(usage_collection, state_table.report_count, state_table.report_id, state_table.logical_range, state_table.report_size, input_report->get_next_offset()));
            ESP_LOGD(TAG, "Parsed %s report item of type: variable, report size: %d, report count: %d, report id: %d", hid_report_type_to_string(report_type), state_table.report_size, state_table.report_count, state_table.report_id);
          }
          else
          {
            input_report->push_back(new HIDInputReportArray(usage_collection, state_table.report_count, state_table.report_id, state_table.logical_range, state_table.report_size, input_report->get_next_offset()));
            ESP_LOGD(TAG, "Parsed %s report item of type: array, report size: %d, report count: %d, report id: %d", hid_report_type_to_string(report_type), state_table.report_size, state_table.report_count, state_table.report_id);
          }
          break;
        }

        default:
          break;
        }
        if ((report_item_info & HID_ITEM_TYPE_MASK) == HID_ITEM_TYPE_MAIN)
        {
          usages.clear();
          usage_range.maximum = HIDUsage(0, 0);
          usage_range.minimum = HIDUsage(0, 0);
        }
      }
      HIDReportMap *report_map = new HIDReportMap(reports);
      ESP_LOGD(TAG, "Parsed report map with %d reports", reports.size());
      return report_map;
    }

    uint16_t HIDReportMap::make_report_key_(uint8_t report_id, uint8_t report_type)
    {
      return (static_cast<uint16_t>(report_type) << 8) | report_id;
    }

    uint8_t HIDInputReport::get_next_offset()
    {
      return this->report_size;
    }

    void HIDInputReport::add_padding(uint8_t padding_size)
    {
      this->report_size += padding_size;
    }

    void HIDInputReport::push_back(HIDInputReportItem *item)
    {
      this->items.push_back(item);
      this->report_size += item->get_total_size();
    }

    std::vector<HIDReportItemValue> HIDReportMap::parse(uint8_t *hid_report_data)
    {
      return this->parse(HID_REPORT_TYPE_INPUT, hid_report_data);
    }

    std::vector<HIDReportItemValue> HIDReportMap::parse(uint8_t report_type, uint8_t *hid_report_data)
    {
      if (this->reports.empty())
      {
        ESP_LOGW(TAG, "No HID reports found");
        return std::vector<HIDReportItemValue>();
      }

      uint16_t report_key = HIDReportMap::make_report_key_(0, report_type);
      if (this->reports.count(report_key) == 0)
      {
        uint8_t report_id = hid_report_data[0];
        report_key = HIDReportMap::make_report_key_(report_id, report_type);
        auto report_it = this->reports.find(report_key);
        if (report_it == this->reports.end())
        {
          ESP_LOGW(TAG, "No %s report found for report ID %d", hid_report_type_to_string(report_type), report_id);
          return std::vector<HIDReportItemValue>();
        }
        ESP_LOGD(TAG, "Parsing HID %s report with report ID (%d)", hid_report_type_to_string(report_type), report_id);
        hid_report_data++;
        return report_it->second->parse(hid_report_data);
      }
      ESP_LOGD(TAG, "Parsing HID %s report without report ID", hid_report_type_to_string(report_type));
      return this->reports.at(report_key)->parse(hid_report_data);
    }

    std::vector<HIDReportItemValue> HIDInputReport::parse(uint8_t *report_data)
    {
      std::vector<HIDReportItemValue> report_values;
      for (HIDInputReportItem *report_item : this->items)
      {
        std::vector<HIDReportItemValue> item_values = report_item->parse(report_data);
        for (HIDReportItemValue item_value : item_values)
        {
          report_values.push_back(item_value);
        }
      }
      return report_values;
    }

    size_t HIDInputReportItem::get_total_size()
    {
      return this->report_size * this->report_count;
    }

    int32_t HIDInputReportItem::parse_input_report_item(uint8_t *report_data, uint16_t bit_offset, uint16_t report_size, HIDLogicalRange logical_range)
    {
      int32_t value = 0;
      uint16_t data_bits_remaining = report_size;
      uint16_t current_bit = bit_offset;
      uint32_t bit_mask = (1 << 0);
      bool negative_range = logical_range.minimum < 0 || logical_range.maximum < 0;
      // scan through report data
      while (data_bits_remaining--)
      {
        if (report_data[current_bit / 8] & (1 << (current_bit % 8)))
        {
          if (negative_range && data_bits_remaining == 0)
          {
            value -= 1 << (current_bit - bit_offset);
          }
          else
          {
            value |= bit_mask;
          }
        }
        bit_mask <<= 1;
        current_bit++;
      }
      return value;
    }

    std::string HIDReportItemValue::to_string() const
    {
      return vformat("HIDReportItemValue(usage_page: %d, usage: %d, value: %d)",this->usage.page, this->usage.usage, this->value);
    }

    std::vector<HIDReportItemValue> HIDInputReportVariable::parse(uint8_t *report_data)
    {
      std::vector<HIDReportItemValue> values;
      for (uint8_t i = 0; i < this->report_count; i++)
      {
        int32_t value = parse_input_report_item(report_data, this->report_offset + i * this->report_size, this->report_size, this->logical_range);
        if (value > this->logical_range.maximum || value < this->logical_range.minimum)
        {
          ESP_LOGD(TAG,
                   "Report ID %u variable field %u (bit offset=%u, size=%u): value=%ld (0x%lX) outside logical range [%ld, %ld]",
                   static_cast<unsigned>(this->report_id),
                   static_cast<unsigned>(i),
                   static_cast<unsigned>(this->report_offset +
                                         i * this->report_size),
                   static_cast<unsigned>(this->report_size),
                   static_cast<long>(value),
                   static_cast<unsigned long>(static_cast<uint32_t>(value)),
                   static_cast<long>(this->logical_range.minimum),
                   static_cast<long>(this->logical_range.maximum));
          continue;
        }
        if (this->last_values[i].raw_value == value)
          continue;
        values.push_back(HIDReportItemValue(this->usage_collection->get_usage(i), value, value));
        ESP_LOGD(TAG, values.back().to_string().c_str());

        this->last_values[i] = values.back();
      }
      return values;
    }

    std::vector<HIDReportItemValue> HIDInputReportArray::parse(uint8_t *report_data)
    {
      std::vector<HIDReportItemValue> values = {};

      for (uint8_t i = 0; i < this->report_count; i++)
      {
        int32_t value = parse_input_report_item(report_data, this->report_offset + i * this->report_size, this->report_size, this->logical_range);
        if (value > this->logical_range.maximum || value < this->logical_range.minimum)
        {
          ESP_LOGD(TAG,
                   "Report ID %u array field %u (bit offset=%u, size=%u): value=%ld (0x%lX) outside logical range [%ld, %ld]; treating as null/no selection",
                   static_cast<unsigned>(this->report_id),
                   static_cast<unsigned>(i),
                   static_cast<unsigned>(this->report_offset +
                                         i * this->report_size),
                   static_cast<unsigned>(this->report_size),
                   static_cast<long>(value),
                   static_cast<unsigned long>(static_cast<uint32_t>(value)),
                   static_cast<long>(this->logical_range.minimum),
                   static_cast<long>(this->logical_range.maximum));
          value = 0;
        }
        if(value == 0){
          if(this->last_values[i].value){
            values.push_back(HIDReportItemValue(this->last_values[i].usage, 0, value));
            last_values[i] = values.back();
            ESP_LOGD(TAG, values.back().to_string().c_str());
          }
        } else {
          if(this->last_values[i].value == 0){
            values.push_back(HIDReportItemValue(this->usage_collection->get_usage(value), 1, value));
            last_values[i] = values.back();
            ESP_LOGD(TAG, last_values[i].to_string().c_str());
          }
        }
      }
      return values;
    }
  } // namespace ble_client_hid
} // namespace esphome
