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
  assert(hidUsageToIso7(0x04, false, iso7) && iso7 == 'A');
  assert(hidUsageToIso7(0x04, true, iso7) && iso7 == 'a');
  assert(hidUsageToIso7(0x1E, false, iso7) && iso7 == '1');
  assert(hidUsageToIso7(0x1E, true, iso7) && iso7 == '!');
  assert(hidUsageToIso7(0x2C, false, iso7) && iso7 == ' ');
  assert(hidUsageToIso7(0x2C, true, iso7) && iso7 == ' ');
  assert(hidUsageToIso7(0x28, false, iso7) && iso7 == 0x0D);
  assert(!hidUsageToIso7(0, false, iso7));
}

void checkShiftCapturedAtKeyDown() {
  HidKeyboardTransitions tracker;
  HidKeyboardEvent events[HidKeyboardTransitions::MaxEvents];
  const HidKeyboardReport shiftedA{0x02, 0, {4, 0, 0, 0, 0, 0}};
  const HidKeyboardReport plainA{0, 0, {4, 0, 0, 0, 0, 0}};
  assert(tracker.update(shiftedA, events) == 1);
  assert(events[0].usage == 4 && events[0].iso7Code == 'a');
  // Shift released before A must not produce an additional uppercase A.
  assert(tracker.update(plainA, events) == 0);
  assert(tracker.update(plainA, events) == 0);
  assert(tracker.update(HidKeyboardReport{}, events) == 1);
  assert(events[0].usage == 0);
  // A fresh physical press uses the current Shift state.
  assert(tracker.update(plainA, events) == 1);
  assert(events[0].iso7Code == 'A');
  assert(tracker.update(shiftedA, events) == 0);
}

void checkOverlappingKeys() {
  HidKeyboardTransitions tracker;
  HidKeyboardEvent events[HidKeyboardTransitions::MaxEvents];
  const HidKeyboardReport b{0, 0, {5, 0, 0, 0, 0, 0}};
  const HidKeyboardReport ab{0, 0, {4, 5, 0, 0, 0, 0}};
  const HidKeyboardReport a{0, 0, {4, 0, 0, 0, 0, 0}};
  assert(tracker.update(b, events) == 1 && events[0].iso7Code == 'B');
  assert(tracker.update(ab, events) == 1 && events[0].iso7Code == 'A');
  // Releasing the newest key stops output, without re-pressing the older B.
  assert(tracker.update(b, events) == 1 && events[0].usage == 0);
  assert(tracker.update(b, events) == 0);
  assert(tracker.update(HidKeyboardReport{}, events) == 0);

  // Reverse release order: releasing the older B leaves the active A alone.
  assert(tracker.update(b, events) == 1);
  assert(tracker.update(ab, events) == 1);
  assert(tracker.update(a, events) == 0);
  assert(tracker.update(HidKeyboardReport{}, events) == 1);
  assert(events[0].usage == 0);

  // Array reordering is not a key-down; neither is adding an unsupported key.
  assert(tracker.update(ab, events) == 2);
  assert(events[0].iso7Code == 'A' && events[1].iso7Code == 'B');
  const HidKeyboardReport ba{0, 0, {5, 4, 0, 0, 0, 0}};
  assert(tracker.update(ba, events) == 0);
  const HidKeyboardReport extra{0, 0, {5, 4, 0x65, 0, 0, 0}};
  assert(tracker.update(extra, events) == 0);

  // A disconnect clears previous state so reconnecting can press the same key.
  tracker.reset();
  assert(tracker.update(b, events) == 1);
}

void checkTransitionCapacity() {
  HidKeyboardTransitions tracker;
  HidKeyboardEvent events[HidKeyboardTransitions::MaxEvents];
  const HidKeyboardReport a{0, 0, {4, 0, 0, 0, 0, 0}};
  assert(tracker.update(a, events) == 1);
  const HidKeyboardReport six{0x20, 0, {5, 6, 7, 8, 9, 10}};
  assert(tracker.update(six, events) == HidKeyboardTransitions::MaxEvents);
  assert(events[0].usage == 0);
  for (size_t index = 1; index < HidKeyboardTransitions::MaxEvents; ++index) {
    assert(events[index].usage == index + 4);
    assert(events[index].iso7Code == 'a' + index);
  }
  assert(tracker.update(six, events) == 0);
  tracker.reset();
  const HidKeyboardReport duplicates{0, 0, {4, 4, 5, 0, 0, 0}};
  assert(tracker.update(duplicates, events) == 2);
}
}  // namespace

int main() {
  checkIndependentReportStates();
  checkDistinctKeysAndCapacity();
  checkReportMapReplacement();
  checkTranslation();
  checkShiftCapturedAtKeyDown();
  checkOverlappingKeys();
  checkTransitionCapacity();
  std::cout << "Keyboard report checks passed\n";
}
