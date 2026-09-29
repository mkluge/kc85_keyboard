#include "hid_report_map.h"

#include <algorithm>
#include <cstring>

namespace
{
struct GlobalState
{
  uint16_t usagePage = 0;
  uint8_t reportSize = 0;
  uint16_t reportCount = 0;
  uint8_t reportId = 0;
};

struct LocalState
{
  std::vector<uint16_t> usages;
  uint16_t usageMinimum = 0;
  uint16_t usageMaximum = 0;
  bool haveUsageMinimum = false;
  bool haveUsageMaximum = false;

  void clear()
  {
    usages.clear();
    usageMinimum = 0;
    usageMaximum = 0;
    haveUsageMinimum = false;
    haveUsageMaximum = false;
  }
};

uint16_t normalizedUsage(uint32_t value)
{
  // Four-byte Usage items may carry their own usage page in the upper word.
  return static_cast<uint16_t>((value >> 16U) != 0 ? value & 0xFFFFU : value);
}
} // namespace

bool HidReportMap::parse(const uint8_t *data, size_t length)
{
  keyboardFields_.clear();
  if (data == nullptr || length == 0)
  {
    return false;
  }

  GlobalState global;
  std::vector<GlobalState> globalStack;
  LocalState local;
  uint32_t inputBitOffsets[256] = {};

  size_t offset = 0;
  while (offset < length)
  {
    const uint8_t prefix = data[offset++];
    if (prefix == 0xFE)
    {
      if (offset + 2 > length)
      {
        return false;
      }
      const uint8_t longSize = data[offset];
      offset += 2;
      if (offset + longSize > length)
      {
        return false;
      }
      offset += longSize;
      continue;
    }

    uint8_t itemSize = prefix & 0x03U;
    if (itemSize == 3)
    {
      itemSize = 4;
    }
    if (offset + itemSize > length)
    {
      return false;
    }

    const uint8_t type = (prefix >> 2U) & 0x03U;
    const uint8_t tag = (prefix >> 4U) & 0x0FU;
    const uint32_t value = unsignedValue(data + offset, itemSize);
    offset += itemSize;

    if (type == 1) // Global item.
    {
      switch (tag)
      {
      case 0x0: global.usagePage = static_cast<uint16_t>(value); break;
      case 0x7: global.reportSize = static_cast<uint8_t>(value); break;
      case 0x8: global.reportId = static_cast<uint8_t>(value); break;
      case 0x9: global.reportCount = static_cast<uint16_t>(value); break;
      case 0xA: globalStack.push_back(global); break;
      case 0xB:
        if (globalStack.empty())
        {
          return false;
        }
        global = globalStack.back();
        globalStack.pop_back();
        break;
      default: break;
      }
      continue;
    }

    if (type == 2) // Local item.
    {
      const uint16_t usage = normalizedUsage(value);
      switch (tag)
      {
      case 0x0: local.usages.push_back(usage); break;
      case 0x1:
        local.usageMinimum = usage;
        local.haveUsageMinimum = true;
        break;
      case 0x2:
        local.usageMaximum = usage;
        local.haveUsageMaximum = true;
        break;
      default: break;
      }
      continue;
    }

    if (type != 0) // Reserved item type.
    {
      continue;
    }

    if (tag == 0x8) // Input main item.
    {
      const uint32_t fieldBits =
          static_cast<uint32_t>(global.reportSize) * global.reportCount;
      const bool constant = (value & 0x01U) != 0;
      if (!constant && global.usagePage == 0x07 && global.reportSize != 0 &&
          global.reportSize <= 32 && global.reportCount != 0 &&
          inputBitOffsets[global.reportId] <= 0xFFFFU)
      {
        InputField field;
        field.reportId = global.reportId;
        field.bitOffset =
            static_cast<uint16_t>(inputBitOffsets[global.reportId]);
        field.bitSize = global.reportSize;
        field.count = global.reportCount;
        field.variable = (value & 0x02U) != 0;
        field.usageMinimum =
            local.haveUsageMinimum ? local.usageMinimum : 0;
        field.usages = local.usages;
        keyboardFields_.push_back(field);
      }
      inputBitOffsets[global.reportId] += fieldBits;
    }

    // Local items apply only to the next Main item.
    local.clear();
  }

  return !keyboardFields_.empty();
}

bool HidReportMap::hasKeyboardInput(uint8_t reportId) const
{
  return std::any_of(keyboardFields_.begin(), keyboardFields_.end(),
                     [reportId](const InputField &field) {
                       return field.reportId == reportId;
                     });
}

bool HidReportMap::decodeKeyboardInput(uint8_t reportId, const uint8_t *data,
                                       size_t length,
                                       HidKeyboardReport &report) const
{
  std::memset(&report, 0, sizeof(report));
  bool foundField = false;
  uint8_t keyCount = 0;

  const auto addUsage = [&report, &keyCount](uint16_t usage) {
    if (usage >= 0xE0 && usage <= 0xE7)
    {
      report.modifiers |= static_cast<uint8_t>(1U << (usage - 0xE0));
      return;
    }
    // Usages 1-3 are HID rollover/error values, not keys.
    if (usage >= 4 && usage <= 0xFF && keyCount < sizeof(report.keys))
    {
      report.keys[keyCount++] = static_cast<uint8_t>(usage);
    }
  };

  for (const InputField &field : keyboardFields_)
  {
    if (field.reportId != reportId)
    {
      continue;
    }
    const uint32_t fieldEnd =
        static_cast<uint32_t>(field.bitOffset) +
        static_cast<uint32_t>(field.bitSize) * field.count;
    if (fieldEnd > length * 8U)
    {
      return false;
    }
    foundField = true;

    for (uint16_t index = 0; index < field.count; ++index)
    {
      const uint32_t value = readBits(
          data, length,
          static_cast<uint32_t>(field.bitOffset) +
              static_cast<uint32_t>(index) * field.bitSize,
          field.bitSize);
      if (field.variable)
      {
        if (value != 0)
        {
          addUsage(usageForIndex(field, index));
        }
      }
      else
      {
        addUsage(static_cast<uint16_t>(value));
      }
    }
  }

  return foundField;
}

uint32_t HidReportMap::readBits(const uint8_t *data, size_t length,
                                uint32_t bitOffset, uint8_t bitSize)
{
  uint32_t value = 0;
  for (uint8_t bit = 0; bit < bitSize; ++bit)
  {
    const uint32_t sourceBit = bitOffset + bit;
    const size_t byteIndex = sourceBit / 8U;
    if (byteIndex >= length)
    {
      break;
    }
    if ((data[byteIndex] & (1U << (sourceBit % 8U))) != 0)
    {
      value |= 1UL << bit;
    }
  }
  return value;
}

uint32_t HidReportMap::unsignedValue(const uint8_t *data, uint8_t size)
{
  uint32_t value = 0;
  for (uint8_t index = 0; index < size; ++index)
  {
    value |= static_cast<uint32_t>(data[index]) << (index * 8U);
  }
  return value;
}

uint16_t HidReportMap::usageForIndex(const InputField &field, uint16_t index)
{
  if (index < field.usages.size())
  {
    return field.usages[index];
  }
  return static_cast<uint16_t>(field.usageMinimum + index);
}
