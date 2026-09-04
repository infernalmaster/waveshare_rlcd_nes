/* ble_keyboard.h - a BLE keyboard as the NES gamepad.
 *
 * The ESP32-S3 acts as a BLE central and talks HID-over-GATT to any BLE
 * keyboard - a ZMK build, in the case this was written for. Key presses land in
 * a bitmask that nes_get_gamepad_state() ORs in alongside the GPIO buttons and
 * the serial pad.
 *
 * WHY NOT BLUEPAD32, which is the obvious answer to "Bluetooth controller on an
 * ESP32": its Arduino API does not expose keyboards at all. ControllerData's
 * union holds only gamepad, mouse and balance board; there is no isKeyboard()
 * and no key accessor. Keyboards exist solely in the underlying ESP-IDF C layer
 * (uni_keyboard_t), which the Arduino wrapper never surfaces. Using it would
 * also mean installing a forked ESP32 core, since Bluepad32 replaces the
 * Bluetooth stack with BTstack. NimBLE-Arduino is an ordinary library on the
 * stock core, so nothing already working has to change.
 *
 * Requires NimBLE-Arduino 2.x (Library Manager). The 1.x client API differs -
 * setScanCallbacks, connect(const NimBLEAdvertisedDevice*) and the four-argument
 * notify callback are all 2.x spellings.
 *
 * ORDER OF CALLS: init() once, then either menu-driven set_target() or the
 * saved one, then begin(). Scanning has to happen before begin() starts the
 * background task, so the two do not fight over the radio.
 */
#ifndef BLE_KEYBOARD_H
#define BLE_KEYBOARD_H

#include <stdbool.h>
#include <stdint.h>

#define BLE_KB_NAME_LEN 24
#define BLE_KB_ADDR_LEN 20

typedef struct {
    char name[BLE_KB_NAME_LEN];
    char addr[BLE_KB_ADDR_LEN];
    bool bonded;                 /* we already hold a key for this one */
} ble_kb_device_t;

/* Brings up the BLE stack and loads the saved keyboard choice. Does not scan
 * and does not connect. */
void ble_keyboard_init(void);

/* Blocking scan for HID devices. Returns how many were written to `out`.
 * Only valid before ble_keyboard_begin(). */
int ble_keyboard_scan(ble_kb_device_t *out, int max, uint32_t ms);

/* Which keyboard to look for. Matching is by NAME - a BLE address is not a
 * stable identifier here, since private addresses rotate and we watched this
 * keyboard change address between two runs. `addr` is only used when the device
 * advertises no name at all. Persisted to NVS, so this is asked once. */
void ble_keyboard_set_target(const char *name, const char *addr);

/* Forgets the saved choice AND every stored pairing key. The keys are the
 * important half: a bond the keyboard has already dropped can never re-pair,
 * and there is no way to ask a BLE peer to forget you. */
void ble_keyboard_forget(void);

/* The remembered choice, or "" if the menu has never run. The picker shows it
 * every boot anyway - this is what lets it start with the cursor on the
 * keyboard you used last time. */
const char *ble_keyboard_target_name(void);

/* "Play without a keyboard", for this boot. Not persisted: the picker runs
 * every time, so there is nothing to remember. */
void ble_keyboard_disable(void);

/* Starts the background task that connects and reconnects. Returns
 * immediately: the emulator must boot whether or not a keyboard is switched on,
 * and a keyboard powered up ten minutes later still connects. */
void ble_keyboard_begin(void);

/* Currently held keys as HW_MASK_* / NES_* bits. Cheap - one volatile read, so
 * it is fine to call once per emulated frame. */
int ble_keyboard_buttons(void);

/* True once reports are actually being delivered - not merely once a link
 * exists. A keyboard can stay connected and silent indefinitely; see the ZMK
 * profile note in ble_keyboard.cpp. */
bool ble_keyboard_connected(void);

/* One line for a status display. Never NULL. */
const char *ble_keyboard_status(void);

/* Picks a keyboard on the panel and saves the choice. Call between init() and
 * begin() - scanning has to finish before the background task starts, or the
 * two fight over the radio. Returns false if the user chose to play without
 * one. Runs on every boot. */
bool ble_keyboard_menu(void);

#endif /* BLE_KEYBOARD_H */
