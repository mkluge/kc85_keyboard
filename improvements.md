# BLE implementation improvements

## 1. Require authenticated, MITM-protected pairing

Configure BLE security to match the six-digit passkey workflow by using `ESP_BLE_SEC_ENCRYPT_MITM` as the encryption level and `ESP_LE_AUTH_REQ_SC_MITM_BOND` as the authentication mode, and explicitly set the maximum key size to 16 bytes. The connection logic must treat authentication as a required state rather than merely waiting for it opportunistically: if authentication fails or does not complete before the deadline, disconnect and retry instead of continuing with service discovery. Before setting the application-level `connected` flag, verify that the link has completed authentication successfully. Unexpected pairing paths, such as a passkey request or numeric-comparison confirmation when the host is configured as display-only, should be rejected or handled explicitly instead of returning a default PIN or accepting automatically.

## 2. Rediscover GATT services after every reconnect

Do not reuse cached `BLERemoteService` and `BLERemoteCharacteristic` objects across connections. The simplest robust solution is to destroy the existing `BLEClient` after a disconnect and create a fresh client for the next connection. Alternatively, force a complete service rediscovery with `getServices()` before obtaining the HID service, after first clearing the report-subscription table. Rebuild all Report characteristic and Report Reference mappings for the new connection and only mark the keyboard ready after discovery and notification registration succeed. This ensures that changed GATT handles, Service Changed events, firmware updates, or a different matching keyboard cannot leave the adapter using stale handles.

## 3. Synchronize state shared with BLE callbacks

BLE callbacks and the Arduino loop run in different task contexts, so `volatile` flags and unsynchronized `std::vector` access are insufficient. Change the callbacks so they only copy compact events into FreeRTOS queues, such as advertisement candidates, authentication results, disconnect notifications, and HID reports. Perform connection-state changes, bonded-device updates, address replacement, and subscription-table construction from one owner task. If some objects must remain shared, protect them with a mutex and use atomics or FreeRTOS event groups for simple state flags. In particular, never clear or append to `keyboardInputSubscriptions` while the notification callback may be searching it.

## 4. Prevent HID report backlog and key-release latency

Treat incoming keyboard reports as current state rather than as commands that all need to be replayed. Replace the 16-entry FIFO with a one-element overwrite queue or another latest-state mailbox so a new notification supersedes obsolete pending state. If the keyboard exposes multiple keyboard report IDs, retain one state per report ID and merge those states before publishing the aggregate keyboard state. Move the timing-sensitive KC85 frame generation to a dedicated task, hardware timer, or RMT-based implementation so its synchronous 36–50 ms transmission does not delay BLE state consumption. The output side should always see the newest press or release promptly, even during bursts of BLE notifications.

## 5. Make bonded-device recognition privacy-aware

Do not assume that a bonded device will continue advertising the same Bluetooth address stored in the bond database, because BLE privacy may replace it with a resolvable private address. Select connection candidates using a combination of the HID service UUID, HID appearance, expected device name, and known identity information, then allow the Bluetooth stack to resolve the current address against stored bonding keys during connection. Persist the preferred keyboard identity when practical, including the address type or identity metadata needed by the stack, rather than comparing only raw advertised addresses. Update the README so its reconnection guarantee accurately reflects the implemented identity-resolution behavior.
