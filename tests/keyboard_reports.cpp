#include <cassert>
#include <cstring>
#include <iostream>

#include "hid_keyboard.h"
#include "hid_report_map.h"

namespace {
// A keyboard with separate modifier and key-array report IDs. Report IDs
// come from the GATT Report Reference, not from a prefix in the payload.
constexpr uint8_t ReportMap[] = {
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01,
    0x85, 0x01, 0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7,
    0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
    0x85, 0x02, 0x19, 0x00, 0x29, 0x65, 0x15, 0x00, 0x25, 0x65,
    0x75, 0x08, 0x95, 0x06, 0x81, 0x00, 0xC0,
};

HidKeyboardReport combine(const HidKeyboardReport &first,
                          const HidKeyboardReport &second) {
  HidKeyboardReport aggregate{};
  mergeKeyboardReport(aggregate, first);
  mergeKeyboardReport(aggregate, second);
  return aggregate;
}

void checkIndependentReportStates() {
  HidReportMap map;
  assert(map.parse(ReportMap, sizeof(ReportMap)));
  assert(map.hasKeyboardInput(1));
  assert(map.hasKeyboardInput(2));
  assert(!map.hasKeyboardInput(3));

  HidKeyboardReport modifiers{}, keys{};
  const uint8_t shift[] = {0x02};
  const uint8_t letter[] = {0x04, 0, 0, 0, 0, 0};
  assert(map.decodeKeyboardInput(1, shift, sizeof(shift), modifiers));
  assert(map.decodeKeyboardInput(2, letter, sizeof(letter), keys));
  auto aggregate = combine(modifiers, keys);
  assert(aggregate.modifiers == 0x02 && aggregate.keys[0] == 0x04);

  // Releasing Shift retains the key from the other report ID.
  const uint8_t noShift[] = {0};
  assert(map.decodeKeyboardInput(1, noShift, sizeof(noShift), modifiers));
  aggregate = combine(modifiers, keys);
  assert(aggregate.modifiers == 0 && aggregate.keys[0] == 0x04);

  // A later release replaces the key report's state completely.
  const uint8_t release[6]{};
  assert(map.decodeKeyboardInput(2, release, sizeof(release), keys));
  aggregate = combine(modifiers, keys);
  const HidKeyboardReport empty{};
  assert(std::memcmp(&aggregate, &empty, sizeof(empty)) == 0);
  assert(!map.decodeKeyboardInput(2, letter, 1, keys));
}

void checkDistinctKeysAndCapacity() {
  const HidKeyboardReport first{0x02, 0, {4, 5, 6, 7, 8, 9}};
  const HidKeyboardReport second{0x20, 0, {4, 10, 11, 0, 0, 0}};
  const auto aggregate = combine(first, second);
  assert(aggregate.modifiers == 0x22);
  assert(std::memcmp(aggregate.keys, first.keys, sizeof(first.keys)) == 0);

  const HidKeyboardReport duplicate{0, 0, {4, 4, 5, 0, 0, 0}};
  const auto unique = combine(duplicate, HidKeyboardReport{});
  assert(unique.keys[0] == 4 && unique.keys[1] == 5 && unique.keys[2] == 0);

  HidKeyboardReport firstId{0, 0, {4, 0, 0, 0, 0, 0}};
  const HidKeyboardReport secondId{0, 0, {5, 0, 0, 0, 0, 0}};
  auto held = combine(firstId, secondId);
  assert(held.keys[0] == 4 && held.keys[1] == 5);
  firstId = HidKeyboardReport{};
  held = combine(firstId, secondId);
  assert(held.keys[0] == 5 && held.keys[1] == 0);
}

void checkReportMapReplacement() {
  HidReportMap map;
  assert(map.parse(ReportMap, sizeof(ReportMap)));
  uint8_t replacement[sizeof(ReportMap)];
  std::memcpy(replacement, ReportMap, sizeof(replacement));
  // A reconnected keyboard uses IDs 3 and 4 instead of IDs 1 and 2.
  replacement[7] = 3;
  replacement[25] = 4;
  assert(map.parse(replacement, sizeof(replacement)));
  assert(!map.hasKeyboardInput(1) && !map.hasKeyboardInput(2));
  assert(map.hasKeyboardInput(3) && map.hasKeyboardInput(4));
}

void checkTranslation() {
  uint8_t iso7 = 0;
  assert(hidUsageToIso7(0x04, false, iso7) && iso7 == 'a');
  assert(hidUsageToIso7(0x04, true, iso7) && iso7 == 'A');
  assert(hidUsageToIso7(0x28, false, iso7) && iso7 == 0x0D);
  assert(!hidUsageToIso7(0, false, iso7));
}
}  // namespace

int main() {
  checkIndependentReportStates();
  checkDistinctKeysAndCapacity();
  checkReportMapReplacement();
  checkTranslation();
  std::cout << "Keyboard report checks passed\n";
}
