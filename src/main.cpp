#include <Arduino.h>
#include <BLEAdvertisedDevice.h>
#include <BLEClient.h>
#include <BLEDevice.h>
#include <BLERemoteCharacteristic.h>
#include <BLERemoteService.h>
#include <BLEScan.h>
#include <BLESecurity.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>
#include <vector>

#include "hid_keyboard.h"
#include "kc85_keyboard.h"

namespace {
constexpr uint16_t kHidServiceUuid = 0x1812;
constexpr uint16_t kReportUuid = 0x2A4D;
constexpr uint16_t kBootKeyboardInputUuid = 0x2A22;
constexpr char kKeyboardName[] = "MX Keys Mini";
constexpr char kKeyboardNameMatch[] = "mx keys mini";
constexpr uint32_t kScanSeconds = 5;
constexpr uint32_t kRetryDelayMs = 1500;
constexpr uint32_t kKeyFlashMs = 100;
constexpr uint32_t kConsoleHeartbeatMs = 3000;
constexpr uint8_t kLedBrightness = 24;
constexpr uint8_t kKcDataPin = A0;
constexpr uint8_t kKeyboardReportQueueLength = 16;
constexpr uint8_t kShiftModifierMask = 0x22;

BLEClient *client = nullptr;
BLEAddress *keyboardAddress = nullptr;
esp_ble_addr_type_t keyboardAddressType = BLE_ADDR_TYPE_PUBLIC;
std::vector<BLEAddress> bondedAddresses;
QueueHandle_t keyboardReportQueue = nullptr;
Kc85Keyboard kcKeyboard(kKcDataPin);

volatile bool connectRequested = false;
volatile bool connected = false;
volatile bool authenticated = false;
volatile bool authenticationFinished = false;

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
  return std::find(bondedAddresses.begin(), bondedAddresses.end(), address) !=
         bondedAddresses.end();
}

void rememberBondedAddress(const BLEAddress &address) {
  if (!isBondedAddress(address)) {
    bondedAddresses.push_back(address);
  }
}

void loadBondedAddresses() {
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

  for (int i = 0; i < count; ++i) {
    rememberBondedAddress(BLEAddress(devices[i].bd_addr));
  }
  Serial.printf("Loaded %u stored BLE bond(s) for automatic reconnection\n",
                static_cast<unsigned>(bondedAddresses.size()));
}

void enqueueKeyboardReport(const HidKeyboardReport &report) {
  if (keyboardReportQueue == nullptr) {
    return;
  }

  if (xQueueSend(keyboardReportQueue, &report, 0) != pdTRUE) {
    // Retain the most recent transitions if an unusual burst fills the queue.
    HidKeyboardReport discarded;
    xQueueReceive(keyboardReportQueue, &discarded, 0);
    xQueueSend(keyboardReportQueue, &report, 0);
  }
}

void onHidReport(BLERemoteCharacteristic *, uint8_t *data, size_t length,
                 bool) {
  // Keyboard input reports use the standard boot-keyboard shape. Other
  // notifiable HID reports (consumer/media controls, for example) are ignored.
  if (length != sizeof(HidKeyboardReport)) {
    return;
  }

  HidKeyboardReport report;
  std::memcpy(&report, data, sizeof(report));
  enqueueKeyboardReport(report);
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

void printKeyboardReport(const HidKeyboardReport &report) {
  Serial.printf("[KEY] modifiers=0x%02X keys=[", report.modifiers);
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
  printKeyboardReport(report);
  if (reportHasPressedKey(report)) {
    setLed(true);
    ledOffAt = millis() + kKeyFlashMs;
  }

  const bool shifted = (report.modifiers & kShiftModifierMask) != 0;
  for (uint8_t usage : report.keys) {
    uint8_t iso7Code;
    if (usage != 0 && hidUsageToIso7(usage, shifted, iso7Code) &&
        kcKeyboard.pressIso7(iso7Code)) {
      KcKey kcKey;
      bool kcShifted;
      Kc85Keyboard::keyForIso7(iso7Code, kcKey, kcShifted);
      Serial.printf("[KEY] HID 0x%02X -> ISO-7 0x%02X -> KC85 IBUS 0x%02X",
                    usage, iso7Code,
                    Kc85Keyboard::ibusForKey(kcKey, kcShifted));
      Serial.println();
      return;
    }
  }

  // A release, modifier-only report, or unsupported key stops KC85 repeats.
  kcKeyboard.releaseKey();
  Serial.println(reportHasPressedKey(report)
                     ? "[KEY] no supported KC85 key in report"
                     : "[KEY] all keys released");
}

class KeyboardAdvertisementCallbacks : public BLEAdvertisedDeviceCallbacks {
 public:
  void onResult(BLEAdvertisedDevice device) override {
    const BLEAddress advertisedAddress = device.getAddress();
    const bool namedMxKeysMini =
        device.haveName() &&
        containsIgnoringCase(device.getName(), kKeyboardNameMatch);
    const bool knownBond = isBondedAddress(advertisedAddress);
    const bool advertisesHid =
        device.haveServiceUUID() &&
        device.isAdvertisingService(BLEUUID(kHidServiceUuid));

    Serial.printf("  %s | %s | RSSI %d dBm%s\n",
                  device.haveName() ? device.getName().c_str() : "<unnamed>",
                  device.getAddress().toString().c_str(),
                  device.haveRSSI() ? device.getRSSI() : -999,
                  advertisesHid ? " | HID" : "");

    if (!namedMxKeysMini && !knownBond) {
      return;
    }

    Serial.println(knownBond ? "[STATUS] bonded keyboard found; reconnecting"
                             : "[STATUS] MX Keys Mini found; pairing required");

    delete keyboardAddress;
    keyboardAddress = new BLEAddress(advertisedAddress);
    keyboardAddressType = device.getAddressType();
    connectRequested = true;
    BLEDevice::getScan()->stop();
  }
};

class KeyboardClientCallbacks : public BLEClientCallbacks {
 public:
  void onConnect(BLEClient *) override {
    Serial.println("[STATUS] BLE link connected; authenticating");
  }

  void onDisconnect(BLEClient *) override {
    connected = false;
    authenticated = false;
    authenticationFinished = false;
    nextScanAt = millis() + kRetryDelayMs;
    enqueueKeyboardReport(HidKeyboardReport{});
    Serial.println("[STATUS] keyboard disconnected; scanning will resume");
  }
};

class KeyboardSecurityCallbacks : public BLESecurityCallbacks {
 public:
  uint32_t onPassKeyRequest() override {
    Serial.println("[PAIRING] keyboard requested a passkey from the ESP32");
    return 0;
  }

  void onPassKeyNotify(uint32_t passkey) override {
    Serial.printf("[PAIRING] code: %06lu\n",
                  static_cast<unsigned long>(passkey));
    Serial.println("[PAIRING] type this code on the keyboard, then press Enter");
  }

  bool onSecurityRequest() override { return true; }

  void onAuthenticationComplete(esp_ble_auth_cmpl_t result) override {
    authenticated = result.success;
    authenticationFinished = true;
    if (result.success) {
      rememberBondedAddress(BLEAddress(result.bd_addr));
      Serial.println("[STATUS] keyboard paired and bonded");
    } else {
      Serial.printf("[STATUS] pairing failed (reason 0x%02x)\n", result.fail_reason);
    }
  }

  bool onConfirmPIN(uint32_t pin) override {
    Serial.printf("Confirming pairing code %06lu\n",
                  static_cast<unsigned long>(pin));
    return true;
  }
};

KeyboardAdvertisementCallbacks advertisementCallbacks;
KeyboardClientCallbacks clientCallbacks;
KeyboardSecurityCallbacks securityCallbacks;
BLESecurity security;

size_t subscribeToInputReports(BLERemoteService *hidService) {
  size_t subscribed = 0;
  auto *characteristics = hidService->getCharacteristicsByHandle();

  for (const auto &entry : *characteristics) {
    BLERemoteCharacteristic *characteristic = entry.second;
    BLEUUID uuid = characteristic->getUUID();
    const bool isInputReport = uuid.equals(BLEUUID(kReportUuid)) ||
                               uuid.equals(BLEUUID(kBootKeyboardInputUuid));
    if (isInputReport && characteristic->canNotify()) {
      characteristic->registerForNotify(onHidReport);
      ++subscribed;
    }
  }
  return subscribed;
}

bool connectToKeyboard() {
  if (keyboardAddress == nullptr) {
    return false;
  }

  if (client == nullptr) {
    client = BLEDevice::createClient();
    client->setClientCallbacks(&clientCallbacks);
  }

  authenticated = false;
  authenticationFinished = false;
  Serial.printf("[STATUS] connecting to %s...\n", keyboardAddress->toString().c_str());

  if (!client->connect(*keyboardAddress, keyboardAddressType)) {
    Serial.println("[STATUS] connection failed");
    return false;
  }

  // Encryption starts automatically because setup() configures an encryption
  // level. Give the pairing/bonding exchange time to complete.
  const uint32_t authenticationDeadline = millis() + 15000;
  while (!authenticationFinished && client->isConnected() &&
         static_cast<int32_t>(authenticationDeadline - millis()) > 0) {
    delay(20);
  }

  if (authenticationFinished && !authenticated) {
    client->disconnect();
    return false;
  }

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

  connected = true;
  Serial.printf("[STATUS] connected; listening to %u HID input report(s)\n",
                static_cast<unsigned>(reportCount));
  return true;
}

void scanForKeyboard() {
  Serial.printf("[STATUS] searching for %s...\n", kKeyboardName);
  BLEScan *scan = BLEDevice::getScan();
  scan->setAdvertisedDeviceCallbacks(&advertisementCallbacks);
  scan->setActiveScan(true);
  scan->setInterval(100);
  scan->setWindow(80);
  scan->start(kScanSeconds, false);
  scan->clearResults();

  if (!connectRequested) {
    Serial.println("[STATUS] keyboard not found; retrying shortly");
    Serial.println("[PAIRING] hold an Easy-Switch key until it blinks rapidly");
    nextScanAt = millis() + kRetryDelayMs;
  }
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

  kcKeyboard.begin();
  keyboardReportQueue =
      xQueueCreate(kKeyboardReportQueueLength, sizeof(HidKeyboardReport));
  if (keyboardReportQueue == nullptr) {
    Serial.println("Could not allocate the HID keyboard report queue");
  }

  Serial.println("Initializing BLE...");
  BLEDevice::init("QT Py keyboard host");
  Serial.println("BLE initialized");
  BLEDevice::setSecurityCallbacks(&securityCallbacks);
  BLEDevice::setEncryptionLevel(ESP_BLE_SEC_ENCRYPT);
  security.setAuthenticationMode(ESP_LE_AUTH_REQ_SC_BOND);
  // The MX Keys Mini enters a passkey displayed by its host. Advertise a
  // display-only capability so the passkey arrives in onPassKeyNotify().
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

  HidKeyboardReport report;
  if (keyboardReportQueue != nullptr &&
      xQueueReceive(keyboardReportQueue, &report, 0) == pdTRUE) {
    applyKeyboardReport(report);
  }
  kcKeyboard.service();

  if (ledOffAt != 0 && static_cast<int32_t>(millis() - ledOffAt) >= 0) {
    setLed(false);
    ledOffAt = 0;
  }

  if (connectRequested) {
    connectRequested = false;
    if (!connectToKeyboard()) {
      nextScanAt = millis() + kRetryDelayMs;
    }
  } else if (!connected &&
             static_cast<int32_t>(millis() - nextScanAt) >= 0) {
    scanForKeyboard();
  }

  delay(5);
}
