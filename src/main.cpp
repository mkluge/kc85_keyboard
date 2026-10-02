#include <Arduino.h>
#include <BLEAdvertisedDevice.h>
#include <BLEClient.h>
#include <BLEDevice.h>
#include <BLERemoteCharacteristic.h>
#include <BLERemoteService.h>
#include <BLEScan.h>
#include <BLESecurity.h>
#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstring>
#include <string>
#include <vector>

#include "hid_keyboard.h"
#include "hid_report_map.h"
#include "kc85_keyboard.h"
#include "kc85_output.h"

namespace {
constexpr uint16_t kHidServiceUuid = 0x1812;
constexpr uint16_t kReportUuid = 0x2A4D;
constexpr uint16_t kReportMapUuid = 0x2A4B;
constexpr uint16_t kReportReferenceUuid = 0x2908;
constexpr uint8_t kInputReportType = 0x01;
constexpr char kKeyboardName[] = "MX Keys Mini";
constexpr char kKeyboardNameMatch[] = "mx keys mini";
constexpr uint32_t kScanSeconds = 5;
constexpr uint32_t kRetryDelayMs = 1500;
constexpr uint32_t kKeyFlashMs = 100;
constexpr uint32_t kConsoleHeartbeatMs = 3000;
constexpr uint8_t kLedBrightness = 24;
constexpr uint8_t kKcDataPin = A0;
constexpr size_t kMaxInputReports = 16;
constexpr size_t kMaxReportBytes = 512;
constexpr uint32_t kAuthenticationTimeoutMs = 15000;
constexpr uint16_t kKeyboardAppearance = 0x03C1;
constexpr uint8_t kShiftModifierMask = 0x22;

BLEClient *client = nullptr;
BLEAddress keyboardAddress("00:00:00:00:00:00");
esp_ble_addr_type_t keyboardAddressType = BLE_ADDR_TYPE_PUBLIC;
std::vector<esp_ble_bond_dev_t> bondedDevices;
Preferences preferences;
Kc85Output kcOutput(kKcDataPin);
HidReportMap hidReportMap;

// Callbacks copy data into queues. Only loop() owns connection state, the
// report map, subscriptions and bonds. Kc85Output owns the transmitter.
enum class BleEventType { Authentication, Disconnected, Passkey, RejectPairing,
                          NotificationRegistration };
struct BleEvent {
  explicit BleEvent(BleEventType eventType = BleEventType::Disconnected)
      : type(eventType) {}
  BleEventType type;
  esp_ble_auth_cmpl_t authentication{};
  uint32_t passkey = 0;
  uint16_t handle = 0;
  esp_gatt_status_t status = ESP_GATT_ERROR;
  esp_bd_addr_t address{};
  bool numericComparison = false;
};
struct AdvertisementCandidate {
  esp_bd_addr_t address;
  esp_ble_addr_type_t addressType;
  bool matchesName;
  bool advertisesHid;
  bool keyboardAppearance;
};
struct RawInputReport {
  uint32_t generation;
  size_t length;
  uint8_t data[kMaxReportBytes];
};
QueueHandle_t bleEventQueue = nullptr;
QueueHandle_t candidateQueue = nullptr;
QueueHandle_t inputMailboxes[kMaxInputReports]{};
std::atomic<bool> bleEventOverflow{false};
uint32_t connectionGeneration = 0;

struct KeyboardInputSubscription {
  KeyboardInputSubscription(BLERemoteCharacteristic *input, uint8_t id,
                            QueueHandle_t queue)
      : characteristic(input), reportId(id), mailbox(queue) {}
  BLERemoteCharacteristic *characteristic;
  uint8_t reportId;
  QueueHandle_t mailbox;
  HidKeyboardReport state{};
  bool registered = false;
};
std::vector<KeyboardInputSubscription> keyboardInputSubscriptions;

bool connectRequested = false;
bool connected = false;
bool authenticated = false;
bool authenticationFinished = false;
bool pairingRejected = false;
bool linkDisconnected = false;
esp_ble_auth_cmpl_t authenticatedPeer{};

bool deadlineReached(uint32_t now, uint32_t deadline) {
  return static_cast<int32_t>(now - deadline) >= 0;
}

void queueBleEvent(const BleEvent &event) {
  if (xQueueSend(bleEventQueue, &event, 0) != pdTRUE) {
    bleEventOverflow.store(true);
  }
}

void releaseOutput() {
  kcOutput.release();
}

uint32_t nextScanAt = 0;
uint32_t ledOffAt = 0;
uint32_t nextConsoleHeartbeatAt = 0;
bool serialConsoleAttached = false;

void setLed(bool on) {
  neopixelWrite(PIN_NEOPIXEL, on ? kLedBrightness : 0,
                on ? kLedBrightness : 0, on ? kLedBrightness : 0);
}

void printConsoleBanner() {
  Serial.println();
  Serial.println("[STARTUP] Bluetooth-to-KC85 adapter is running");
  Serial.println("[PAIRING] hold an MX Keys Mini Easy-Switch key until it blinks");
  Serial.println("[PAIRING] type the displayed six-digit code and press Enter");
}

void printConsoleHeartbeat() {
  if (connected) {
    Serial.println("[STATUS] adapter running; keyboard connected");
  } else if (connectRequested) {
    Serial.println("[STATUS] adapter running; keyboard found, connection pending");
  } else {
    Serial.println("[STATUS] adapter running; searching for keyboard");
  }
}

bool containsIgnoringCase(std::string value, const char *needle) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  return value.find(needle) != std::string::npos;
}

bool isBondedAddress(const BLEAddress &address) {
  for (const auto &device : bondedDevices) {
    if (BLEAddress(const_cast<uint8_t *>(device.bd_addr)) == address ||
        ((device.bond_key.key_mask & ESP_BLE_ID_KEY_MASK) != 0 &&
         BLEAddress(const_cast<uint8_t *>(device.bond_key.pid_key.static_addr)) == address)) {
      return true;
    }
  }
  return false;
}

void loadBondedAddresses() {
  bondedDevices.clear();
  int count = esp_ble_get_bond_device_num();
  if (count <= 0) {
    Serial.println("No stored keyboard bond; initial pairing is required");
    return;
  }

  std::vector<esp_ble_bond_dev_t> devices(static_cast<size_t>(count));
  if (esp_ble_get_bond_device_list(&count, devices.data()) != ESP_OK) {
    Serial.println("Could not read stored BLE bonds");
    return;
  }

  devices.resize(count);
  bondedDevices = std::move(devices);
  Serial.printf("Loaded %u stored BLE bond(s) for automatic reconnection\n",
                static_cast<unsigned>(bondedDevices.size()));
}

void rememberPreferredIdentity(const esp_ble_auth_cmpl_t &result) {
  // Store identity metadata, never the bonding keys. Bluedroid owns its IRKs
  // and resolves private addresses when the current advertisement is used.
  const uint8_t *identity = result.bd_addr;
  esp_ble_addr_type_t type = result.addr_type;
  for (const auto &device : bondedDevices) {
    if (std::memcmp(device.bd_addr, result.bd_addr, sizeof(esp_bd_addr_t)) == 0 &&
        (device.bond_key.key_mask & ESP_BLE_ID_KEY_MASK) != 0) {
      identity = device.bond_key.pid_key.static_addr;
      type = device.bond_key.pid_key.addr_type;
      break;
    }
  }
  preferences.putBytes("identity", identity, sizeof(esp_bd_addr_t));
  preferences.putUChar("addr-type", static_cast<uint8_t>(type));
}

bool reportHasPressedKey(const HidKeyboardReport &report) {
  if (report.modifiers != 0) {
    return true;
  }
  for (uint8_t usage : report.keys) {
    if (usage != 0) {
      return true;
    }
  }
  return false;
}

void printHidUsage(uint8_t usage) {
  if (usage >= 0x04 && usage <= 0x1D) {
    Serial.printf("%c(0x%02X)", 'A' + usage - 0x04, usage);
    return;
  }
  if (usage >= 0x1E && usage <= 0x26) {
    Serial.printf("%c(0x%02X)", '1' + usage - 0x1E, usage);
    return;
  }
  if (usage == 0x27) {
    Serial.printf("0(0x%02X)", usage);
    return;
  }
  if (usage >= 0x3A && usage <= 0x45) {
    Serial.printf("F%u(0x%02X)", usage - 0x3A + 1, usage);
    return;
  }

  const char *name = nullptr;
  switch (usage) {
    case 0x28: name = "Enter"; break;
    case 0x29: name = "Escape"; break;
    case 0x2A: name = "Backspace"; break;
    case 0x2B: name = "Tab"; break;
    case 0x2C: name = "Space"; break;
    case 0x39: name = "CapsLock"; break;
    case 0x48: name = "Pause"; break;
    case 0x49: name = "Insert"; break;
    case 0x4A: name = "Home"; break;
    case 0x4C: name = "Delete"; break;
    case 0x4F: name = "Right"; break;
    case 0x50: name = "Left"; break;
    case 0x51: name = "Down"; break;
    case 0x52: name = "Up"; break;
    default: break;
  }

  if (name != nullptr) {
    Serial.printf("%s(0x%02X)", name, usage);
  } else {
    Serial.printf("0x%02X", usage);
  }
}

void printPressedKeyboardReport(const HidKeyboardReport &report) {
  Serial.printf("[DEBUG] BT keyboard key pressed: modifiers=0x%02X keys=[",
                report.modifiers);
  bool first = true;
  for (uint8_t usage : report.keys) {
    if (usage == 0) {
      continue;
    }
    if (!first) {
      Serial.print(", ");
    }
    printHidUsage(usage);
    first = false;
  }
  Serial.println("]");
}

void applyKeyboardReport(const HidKeyboardReport &report) {
  if (reportHasPressedKey(report)) {
    printPressedKeyboardReport(report);
    setLed(true);
    ledOffAt = millis() + kKeyFlashMs;
  }

  const bool shifted = (report.modifiers & kShiftModifierMask) != 0;
  for (uint8_t usage : report.keys) {
    uint8_t iso7Code;
    KcKey kcKey;
    bool kcShifted;
    if (usage != 0 && hidUsageToIso7(usage, shifted, iso7Code) &&
        Kc85Keyboard::keyForIso7(iso7Code, kcKey, kcShifted)) {
      kcOutput.press(kcKey, kcShifted);
      Serial.printf("[KEY] HID 0x%02X -> ISO-7 0x%02X -> KC85 IBUS 0x%02X",
                    usage, iso7Code,
                    Kc85Keyboard::ibusForKey(kcKey, kcShifted));
      Serial.println();
      return;
    }
  }

  // A release, modifier-only report, or unsupported key stops KC85 repeats.
  releaseOutput();
  Serial.println(reportHasPressedKey(report)
                     ? "[KEY] no supported KC85 key in report"
                     : "[KEY] all keys released");
}

class KeyboardAdvertisementCallbacks : public BLEAdvertisedDeviceCallbacks {
 public:
  void onResult(BLEAdvertisedDevice device) override {
    AdvertisementCandidate candidate{};
    std::memcpy(candidate.address, device.getAddress().getNative(),
                sizeof(candidate.address));
    candidate.addressType = device.getAddressType();
    candidate.matchesName =
        device.haveName() &&
        containsIgnoringCase(device.getName(), kKeyboardNameMatch);
    candidate.advertisesHid =
        device.haveServiceUUID() &&
        device.isAdvertisingService(BLEUUID(kHidServiceUuid));

    candidate.keyboardAppearance =
        device.haveAppearance() && device.getAppearance() == kKeyboardAppearance;
    xQueueSend(candidateQueue, &candidate, 0);
  }
};

class KeyboardClientCallbacks : public BLEClientCallbacks {
 public:
  void onConnect(BLEClient *) override {}

  void onDisconnect(BLEClient *) override {
    queueBleEvent(BleEvent{BleEventType::Disconnected});
  }
};

// The Arduino passkey-request callback always sends an affirmative reply.
// Use the raw GAP hook so the owner task can explicitly reject that path.
void onGapEvent(esp_gap_ble_cb_event_t type, esp_ble_gap_cb_param_t *parameters) {
  BleEvent event{};
  switch (type) {
    case ESP_GAP_BLE_AUTH_CMPL_EVT:
      event.type = BleEventType::Authentication;
      event.authentication = parameters->ble_security.auth_cmpl;
      break;
    case ESP_GAP_BLE_PASSKEY_NOTIF_EVT:
      event.type = BleEventType::Passkey;
      event.passkey = parameters->ble_security.key_notif.passkey;
      break;
    case ESP_GAP_BLE_PASSKEY_REQ_EVT:
    case ESP_GAP_BLE_NC_REQ_EVT:
      event.type = BleEventType::RejectPairing;
      event.numericComparison = type == ESP_GAP_BLE_NC_REQ_EVT;
      std::memcpy(event.address, parameters->ble_security.ble_req.bd_addr,
                  sizeof(event.address));
      break;
    default:
      return;
  }
  queueBleEvent(event);
}

void onGattEvent(esp_gattc_cb_event_t type, esp_gatt_if_t,
                 esp_ble_gattc_cb_param_t *parameters) {
  if (type == ESP_GATTC_REG_FOR_NOTIFY_EVT) {
    BleEvent event{BleEventType::NotificationRegistration};
    event.handle = parameters->reg_for_notify.handle;
    event.status = parameters->reg_for_notify.status;
    queueBleEvent(event);
  }
}

KeyboardAdvertisementCallbacks advertisementCallbacks;
KeyboardClientCallbacks clientCallbacks;
BLESecurity security;

void processBleEvents() {
  BleEvent event;
  while (xQueueReceive(bleEventQueue, &event, 0) == pdTRUE) {
    switch (event.type) {
      case BleEventType::Disconnected:
        connected = false;
        authenticated = false;
        linkDisconnected = true;
        keyboardInputSubscriptions.clear();
        releaseOutput();
        nextScanAt = millis() + kRetryDelayMs;
        Serial.println("[STATUS] keyboard disconnected; scanning will resume");
        break;
      case BleEventType::Authentication: {
        if (linkDisconnected || client == nullptr || !client->isConnected()) {
          break;
        }
        const auto &result = event.authentication;
        authenticationFinished = true;
        authenticated = result.success &&
            (result.auth_mode & ESP_LE_AUTH_REQ_SC_MITM_BOND) ==
                ESP_LE_AUTH_REQ_SC_MITM_BOND;
        if (authenticated) {
          authenticatedPeer = result;
          loadBondedAddresses();
          Serial.println("[STATUS] keyboard authenticated and bonded");
        } else {
          pairingRejected = true;
          Serial.printf("[STATUS] authentication rejected (reason 0x%02x, mode 0x%02x)\n",
                        result.fail_reason, result.auth_mode);
        }
        break;
      }
      case BleEventType::Passkey:
        Serial.printf("[PAIRING] code: %06lu; type it on the keyboard and press Enter\n",
                      static_cast<unsigned long>(event.passkey));
        break;
      case BleEventType::RejectPairing:
        if (event.numericComparison) {
          esp_ble_confirm_reply(event.address, false);
        } else {
          esp_ble_passkey_reply(event.address, false, 0);
        }
        pairingRejected = true;
        Serial.println("[PAIRING] rejected unexpected pairing path for display-only host");
        break;
      case BleEventType::NotificationRegistration:
        for (auto &subscription : keyboardInputSubscriptions) {
          if (subscription.characteristic->getHandle() == event.handle) {
            subscription.registered = event.status == ESP_GATT_OK;
          }
        }
        break;
    }
  }
  // Dropping a security/disconnect event cannot leave a keyboard ready.
  if (bleEventOverflow.exchange(false)) {
    pairingRejected = true;
    Serial.println("[STATUS] BLE event queue overflow; reconnecting");
  }
}

size_t subscribeToInputReports(BLERemoteService *hidService) {
  keyboardInputSubscriptions.clear();

  BLERemoteCharacteristic *reportMapCharacteristic =
      hidService->getCharacteristic(BLEUUID(kReportMapUuid));
  if (reportMapCharacteristic == nullptr || !reportMapCharacteristic->canRead()) {
    Serial.println("HID service has no readable Report Map");
    return 0;
  }

  const std::string reportMapValue = reportMapCharacteristic->readValue();
  if (!hidReportMap.parse(
          reinterpret_cast<const uint8_t *>(reportMapValue.data()),
          reportMapValue.size())) {
    Serial.println("Could not parse keyboard fields from HID Report Map");
    return 0;
  }
  Serial.printf("[STATUS] parsed HID Report Map (%u bytes)\n",
                static_cast<unsigned>(reportMapValue.size()));

  size_t subscribed = 0;
  auto *characteristics = hidService->getCharacteristicsByHandle();

  for (const auto &entry : *characteristics) {
    BLERemoteCharacteristic *characteristic = entry.second;
    if (!characteristic->getUUID().equals(BLEUUID(kReportUuid)) ||
        !characteristic->canNotify()) {
      continue;
    }

    BLERemoteDescriptor *reportReference =
        characteristic->getDescriptor(BLEUUID(kReportReferenceUuid));
    if (reportReference == nullptr) {
      continue;
    }

    const std::string referenceValue = reportReference->readValue();
    if (referenceValue.size() < 2 ||
        static_cast<uint8_t>(referenceValue[1]) != kInputReportType) {
      continue;
    }

    const uint8_t reportId = static_cast<uint8_t>(referenceValue[0]);
    if (!hidReportMap.hasKeyboardInput(reportId)) {
      continue;
    }

    if (keyboardInputSubscriptions.size() == kMaxInputReports) {
      Serial.println("Too many keyboard input reports");
      return 0;
    }
    for (const auto &subscription : keyboardInputSubscriptions) {
      if (subscription.reportId == reportId) {
        Serial.println("Duplicate keyboard report ID");
        return 0;
      }
    }
    BLERemoteDescriptor *configuration =
        characteristic->getDescriptor(BLEUUID(uint16_t{0x2902}));
    if (configuration == nullptr) {
      return 0;
    }

    const QueueHandle_t mailbox = inputMailboxes[keyboardInputSubscriptions.size()];
    xQueueReset(mailbox);
    keyboardInputSubscriptions.push_back({characteristic, reportId, mailbox});
    const uint32_t generation = connectionGeneration;
    characteristic->registerForNotify(
        [mailbox, generation](BLERemoteCharacteristic *, uint8_t *data,
                              size_t length, bool) {
          RawInputReport report{};
          report.generation = generation;
          report.length = length;
          if (length <= sizeof(report.data)) {
            std::memcpy(report.data, data, length);
          }
          xQueueOverwrite(mailbox, &report);
        });

    // registerForNotify() returns void in this Arduino version. Verify both
    // its GATT completion event and the remote notification-enable bit.
    const std::string configurationValue = configuration->readValue();
    processBleEvents();
    if (linkDisconnected || pairingRejected ||
        keyboardInputSubscriptions.empty() ||
        !keyboardInputSubscriptions.back().registered ||
        configurationValue.size() != 2 ||
        (static_cast<uint8_t>(configurationValue[0]) & 1) == 0) {
      Serial.println("Could not enable HID notifications");
      return 0;
    }
    Serial.printf("[STATUS] subscribed to keyboard HID report %u\n", reportId);
    ++subscribed;
  }
  return subscribed;
}

bool connectToKeyboard() {
  if (client == nullptr) {
    client = BLEDevice::createClient();
    client->setClientCallbacks(&clientCallbacks);
  }

  authenticated = false;
  authenticationFinished = false;
  pairingRejected = false;
  linkDisconnected = false;
  ++connectionGeneration;
  Serial.printf("[STATUS] connecting to %s...\n", keyboardAddress.toString().c_str());

  if (!client->connect(keyboardAddress, keyboardAddressType)) {
    Serial.println("[STATUS] connection failed");
    return false;
  }

  // Encryption starts automatically because setup() configures an encryption
  // level. Give the pairing/bonding exchange time to complete.
  Serial.println("[STATUS] BLE link connected; authenticating");
  const uint32_t authenticationDeadline = millis() + kAuthenticationTimeoutMs;
  while (!authenticationFinished && !pairingRejected && !linkDisconnected &&
         client->isConnected() && !deadlineReached(millis(), authenticationDeadline)) {
    processBleEvents();
    delay(5);
  }
  processBleEvents();

  if (!authenticationFinished || !authenticated || pairingRejected ||
      linkDisconnected || !client->isConnected()) {
    Serial.println("[STATUS] authentication incomplete or failed; retrying");
    client->disconnect();
    return false;
  }

  // getServices() clears Arduino's cached remote objects and performs service
  // discovery again. Never use old characteristic pointers after this call.
  keyboardInputSubscriptions.clear();
  client->getServices();
  BLERemoteService *hidService = client->getService(BLEUUID(kHidServiceUuid));
  if (hidService == nullptr) {
    Serial.println("Connected device has no BLE HID service");
    client->disconnect();
    return false;
  }

  const size_t reportCount = subscribeToInputReports(hidService);
  if (reportCount == 0) {
    Serial.println("No notifiable keyboard input reports found");
    client->disconnect();
    return false;
  }

  processBleEvents();
  if (!authenticated || pairingRejected || linkDisconnected || !client->isConnected()) {
    client->disconnect();
    return false;
  }
  connected = true;
  rememberPreferredIdentity(authenticatedPeer);
  Serial.printf("[STATUS] connected; listening to %u HID input report(s)\n",
                static_cast<unsigned>(reportCount));
  return true;
}

void onScanComplete(BLEScanResults) {}

void scanForKeyboard() {
  Serial.printf("[STATUS] searching for %s...\n", kKeyboardName);
  BLEScan *scan = BLEDevice::getScan();
  scan->setAdvertisedDeviceCallbacks(&advertisementCallbacks);
  scan->setActiveScan(true);
  scan->setInterval(100);
  scan->setWindow(80);
  scan->clearResults();
  xQueueReset(candidateQueue);
  // The asynchronous scan lets loop() consume candidates and BLE events.
  scan->start(kScanSeconds, onScanComplete, false);
  nextScanAt = millis() + kScanSeconds * 1000 + kRetryDelayMs;
}

void selectConnectionCandidate() {
  if (pairingRejected || (client != nullptr && client->isConnected())) {
    return;
  }
  AdvertisementCandidate candidate;
  while (!connected && !connectRequested &&
         xQueueReceive(candidateQueue, &candidate, 0) == pdTRUE) {
    const BLEAddress address(candidate.address);
    esp_bd_addr_t preferred{};
    const bool preferredIdentity =
        preferences.getBytes("identity", preferred, sizeof(preferred)) == sizeof(preferred) &&
        std::memcmp(preferred, candidate.address, sizeof(preferred)) == 0 &&
        preferences.getUChar("addr-type", 0xFF) == candidate.addressType;
    const bool knownIdentity = preferredIdentity || isBondedAddress(address);
    // A nameless private address cannot be matched by raw address. With bonds
    // present, HID + keyboard appearance is a candidate; authentication lets
    // Bluedroid resolve it against its stored identity keys.
    const bool privateKeyboardCandidate = !bondedDevices.empty() &&
        candidate.advertisesHid && candidate.keyboardAppearance;
    if (!candidate.matchesName && !knownIdentity && !privateKeyboardCandidate) {
      continue;
    }
    keyboardAddress = address;
    keyboardAddressType = candidate.addressType;
    connectRequested = true;
    BLEDevice::getScan()->stop();
    xQueueReset(candidateQueue);
    Serial.println("[STATUS] keyboard candidate found; authenticating before use");
  }
}

void consumeKeyboardReports() {
  bool changed = false;
  for (auto &subscription : keyboardInputSubscriptions) {
    RawInputReport raw;
    if (xQueueReceive(subscription.mailbox, &raw, 0) != pdTRUE ||
        raw.generation != connectionGeneration) {
      continue;
    }
    if (raw.length > sizeof(raw.data) ||
        !hidReportMap.decodeKeyboardInput(subscription.reportId, raw.data,
                                         raw.length, subscription.state)) {
      // A malformed release must not leave a previously pressed key repeating.
      pairingRejected = true;
      releaseOutput();
      Serial.println("[STATUS] invalid HID input; reconnecting");
      return;
    }
    changed = true;
  }
  if (!changed) {
    return;
  }

  HidKeyboardReport aggregate{};
  for (const auto &subscription : keyboardInputSubscriptions) {
    mergeKeyboardReport(aggregate, subscription.state);
  }
  applyKeyboardReport(aggregate);
}
}  // namespace

void setup() {
  Serial.begin(115200);

  // Native USB can take a moment to enumerate after reset. Waiting briefly
  // keeps the startup diagnostics from disappearing before the monitor opens.
  const uint32_t serialDeadline = millis() + 5000;
  while (!Serial && static_cast<int32_t>(serialDeadline - millis()) > 0) {
    delay(10);
  }
  serialConsoleAttached = static_cast<bool>(Serial);
  printConsoleBanner();
  nextConsoleHeartbeatAt = millis() + 1000;

  pinMode(NEOPIXEL_POWER, OUTPUT);
  digitalWrite(NEOPIXEL_POWER, NEOPIXEL_POWER_ON);
  setLed(false);

  bleEventQueue = xQueueCreate(32, sizeof(BleEvent));
  candidateQueue = xQueueCreate(32, sizeof(AdvertisementCandidate));
  bool queuesReady = bleEventQueue && candidateQueue;
  for (auto &mailbox : inputMailboxes) {
    mailbox = xQueueCreate(1, sizeof(RawInputReport));
    queuesReady = queuesReady && mailbox;
  }
  if (!queuesReady || !kcOutput.begin()) {
    Serial.println("Could not allocate keyboard queues/output task; halted");
    for (;;) {
      delay(1000);
    }
  }
  preferences.begin("kc85-keyboard", false);

  Serial.println("Initializing BLE...");
  BLEDevice::init("QT Py keyboard host");
  Serial.println("BLE initialized");
  BLEDevice::setCustomGapHandler(onGapEvent);
  BLEDevice::setCustomGattcHandler(onGattEvent);
  BLEDevice::setEncryptionLevel(ESP_BLE_SEC_ENCRYPT_MITM);
  security.setAuthenticationMode(ESP_LE_AUTH_REQ_SC_MITM_BOND);
  security.setKeySize(16);
  // The MX Keys Mini enters a passkey displayed by its host. Advertise a
  // display-only capability so GAP delivers a passkey notification.
  security.setCapability(ESP_IO_CAP_OUT);
  security.setInitEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);
  security.setRespEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);
  loadBondedAddresses();

  Serial.println("[PAIRING] select a Bluetooth slot and hold its Easy-Switch key");
  Serial.println("[PAIRING] when a six-digit code appears, type it and press Enter");
}

void loop() {
  const bool consoleAttached = static_cast<bool>(Serial);
  if (consoleAttached && !serialConsoleAttached) {
    printConsoleBanner();
    printConsoleHeartbeat();
    nextConsoleHeartbeatAt = millis() + kConsoleHeartbeatMs;
  }
  serialConsoleAttached = consoleAttached;

  const uint32_t now = millis();
  if (static_cast<int32_t>(now - nextConsoleHeartbeatAt) >= 0) {
    printConsoleHeartbeat();
    nextConsoleHeartbeatAt = now + kConsoleHeartbeatMs;
  }

  processBleEvents();
  if (pairingRejected) {
    connected = false;
    releaseOutput();
    keyboardInputSubscriptions.clear();
    if (client != nullptr && client->isConnected()) {
      client->disconnect();
    }
    // Wait for the disconnect event before trying another connection.
    if (client == nullptr || !client->isConnected()) {
      pairingRejected = false;
      nextScanAt = millis() + kRetryDelayMs;
    }
  }
  if (connected) {
    consumeKeyboardReports();
  }

  if (ledOffAt != 0 && static_cast<int32_t>(millis() - ledOffAt) >= 0) {
    setLed(false);
    ledOffAt = 0;
  }

  selectConnectionCandidate();
  if (connectRequested) {
    connectRequested = false;
    if (!connectToKeyboard()) {
      nextScanAt = millis() + kRetryDelayMs;
    }
  } else if (!connected && !pairingRejected &&
             (client == nullptr || !client->isConnected()) &&
             static_cast<int32_t>(millis() - nextScanAt) >= 0) {
    scanForKeyboard();
  }

  delay(5);
}
