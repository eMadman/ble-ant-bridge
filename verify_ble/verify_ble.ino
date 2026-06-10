/*
 * verify_ble.ino — Phase 0 BLE hardware verification
 *
 * Advertises as "BLE-ANT-TEST" and reports connect/disconnect over Serial.
 * Scan with nRF Connect (iOS/Android) to confirm S340 BLE is functional.
 *
 * Requirements before flashing:
 *   - Adafruit nRF52 BSP modified for S340 (app start 0x31000, not 0x26000)
 *   - Board variant: SuperMini (custom) or nRF52840 Feather with S340 linker
 *
 * Pass criteria:
 *   [1] Serial prints "Advertising as 'BLE-ANT-TEST'"
 *   [2] nRF Connect on phone sees "BLE-ANT-TEST" in scan list
 *   [3] Phone can connect — Serial prints "Connected"
 *   [4] Phone disconnects — Serial prints "Disconnected"
 */

#include <bluefruit.h>

static void startAdv();
static void onConnect(uint16_t connHandle);
static void onDisconnect(uint16_t connHandle, uint8_t reason);

void setup() {
    Serial.begin(115200);
    while (!Serial && millis() < 3000) delay(10);

    Serial.println("[VERIFY] BLE hardware check starting");

    // 1 peripheral role, 0 central roles — BLE-only for this test
    Bluefruit.begin(1, 0);
    Bluefruit.setTxPower(4);
    Bluefruit.setName("BLE-ANT-TEST");

    Bluefruit.Periph.setConnectCallback(onConnect);
    Bluefruit.Periph.setDisconnectCallback(onDisconnect);

    startAdv();

    Serial.println("[VERIFY] Advertising as 'BLE-ANT-TEST' — scan with nRF Connect");
    Serial.println("[VERIFY] LED blinks 1 Hz while running");
}

void loop() {
    static uint32_t lastBlink = 0;
    if (millis() - lastBlink >= 1000) {
        lastBlink = millis();
        digitalToggle(LED_BUILTIN);
    }
}

static void startAdv() {
    Bluefruit.Advertising.addFlags(BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE);
    Bluefruit.Advertising.addTxPower();
    Bluefruit.Advertising.addName();
    Bluefruit.Advertising.restartOnDisconnect(true);
    Bluefruit.Advertising.setInterval(160, 160); // 100 ms in 0.625 ms units
    Bluefruit.Advertising.setFastTimeout(30);
    Bluefruit.Advertising.start(0);              // advertise indefinitely
}

static void onConnect(uint16_t connHandle) {
    BLEConnection* conn = Bluefruit.Connection(connHandle);
    char peerName[32] = {0};
    conn->getPeerName(peerName, sizeof(peerName));
    Serial.print("[VERIFY] Connected to: ");
    Serial.println(peerName[0] ? peerName : "(unknown)");
}

static void onDisconnect(uint16_t connHandle, uint8_t reason) {
    (void)connHandle;
    Serial.print("[VERIFY] Disconnected, reason = 0x");
    Serial.println(reason, HEX);
    Serial.println("[VERIFY] Restarting advertising...");
}
