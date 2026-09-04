/* ble_keyboard.cpp - HID-over-GATT keyboard client. See ble_keyboard.h.
 *
 * Uses the BOOT PROTOCOL rather than the report protocol, which is what keeps
 * this short. In boot mode the keyboard is required to send a fixed 8-byte
 * report - modifiers, one reserved byte, then six key codes - so there is no
 * HID report descriptor to fetch and parse, and no ambiguity about which of a
 * keyboard's several input reports is the keys one. ZMK's default HKRO layout
 * with CONFIG_ZMK_HID_KEYBOARD_REPORT_SIZE = 6 is exactly that shape.
 *
 * Do NOT set CONFIG_ZMK_HID_REPORT_TYPE_NKRO=y in the keyboard's config: NKRO
 * replaces the six key slots with a usage bitmap, which the boot protocol
 * cannot describe and this cannot read. Six simultaneous keys is already more
 * than the NES can use - the most any game needs is five.
 */
#include <Arduino.h>
#include <stdarg.h>
#include <stdio.h>

#include "hw_config.h"

#if defined(NES_BLE_KEYBOARD) && NES_BLE_KEYBOARD

#include <NimBLEDevice.h>
#include <Preferences.h>

#include "ble_keyboard.h"

/* NVS namespace and keys for the remembered keyboard. */
#define PREFS_NS   "nes-kbd"
#define PREFS_NAME "name"
#define PREFS_ADDR "addr"

/* Same bit values as controller.cpp and osd.cpp's HW_MASK_*. */
#define NES_A      0x01
#define NES_B      0x02
#define NES_SELECT 0x04
#define NES_START  0x08
#define NES_UP     0x10
#define NES_DOWN   0x20
#define NES_LEFT   0x40
#define NES_RIGHT  0x80

/* HID-over-GATT, assigned numbers. */
#define UUID_HID_SERVICE   ((uint16_t)0x1812)
#define UUID_PROTOCOL_MODE ((uint16_t)0x2A4E)  /* write 0 = boot protocol */
#define UUID_BOOT_KB_INPUT ((uint16_t)0x2A22)
#define UUID_REPORT        ((uint16_t)0x2A4D)
#define UUID_REPORT_REFERENCE ((uint16_t)0x2908)  /* descriptor: id + type   */
#define UUID_REPORT_MAP       ((uint16_t)0x2A4B)  /* the HID descriptor      */
#define UUID_HID_CONTROL      ((uint16_t)0x2A4C)  /* 0 = suspend, 1 = resume */

#define HID_CONTROL_EXIT_SUSPEND 0x01

#define BOOT_PROTOCOL_MODE 0x00

/* Appearance 0x03C1 = Generic HID / Keyboard. Some keyboards advertise the HID
 * service UUID, some only the appearance, so both are accepted. */
#define APPEARANCE_KEYBOARD 0x03C1

static volatile uint8_t  g_buttons   = 0;

/* Two separate states, because they are genuinely different and conflating them
 * is what made the first version lie about itself. g_link_up means the BLE
 * connection exists and is encrypted; g_ready means reports are actually being
 * delivered to us. A keyboard can sit in the first state indefinitely - ZMK,
 * for instance, stays connected to every bonded host but only sends keystrokes
 * to its ACTIVE profile, so "paired, bonded=1" and "no keys ever arrive" is a
 * perfectly normal combination. Only g_ready is worth telling the user about. */
static volatile bool     g_link_up   = false;
static volatile bool     g_ready     = false;
static volatile uint32_t g_reports   = 0;

static char              g_status[64] = "starting";
static char              g_name[32]   = "";
static NimBLEClient     *g_client    = nullptr;
static NimBLEAddress     g_target;
static volatile bool     g_have_target = false;

/* Which keyboard to look for, chosen once through the menu and kept in NVS.
 * By NAME, deliberately: this keyboard was seen advertising under two different
 * addresses in two consecutive runs, so an address is not an identity. */
static char g_want_name[BLE_KB_NAME_LEN] = NES_BLE_KEYBOARD_NAME;
static char g_want_addr[BLE_KB_ADDR_LEN] = "";

/* Set by the picker's "play without keyboard". Session-only - the picker runs
 * every boot, so there is nothing worth persisting. */
static bool g_disabled = false;

/* Raised just before we hang up on purpose.
 *
 * Every disconnect we initiate follows a failure that already set a status
 * explaining itself - "stale bond dropped", "no notifying input report" and so
 * on. onDisconnect then fires asynchronously and would overwrite that with
 * "we closed the connection", which is true, useless, and hides the one line
 * worth reading. So a deliberate hang-up logs and leaves the status alone. */
static volatile bool g_expect_disconnect = false;

static void set_status(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_status, sizeof g_status, fmt, ap);
    va_end(ap);
    Serial.printf("[BLE] %s\n", g_status);
}

/* HID usage codes, Usage Page 0x07. Arrow keys are accepted alongside WASD
 * because they cost one case each and every keyboard has them. */
static uint8_t nes_bit_for_usage(uint8_t usage)
{
    switch (usage) {
    case NES_KEY_UP:     case 0x52: return NES_UP;      /* 0x52 = Up arrow    */
    case NES_KEY_DOWN:   case 0x51: return NES_DOWN;    /* 0x51 = Down arrow  */
    case NES_KEY_LEFT:   case 0x50: return NES_LEFT;    /* 0x50 = Left arrow  */
    case NES_KEY_RIGHT:  case 0x4F: return NES_RIGHT;   /* 0x4F = Right arrow */
    case NES_KEY_A:      return NES_A;
    case NES_KEY_B:      return NES_B;
    case NES_KEY_START:  return NES_START;
    case NES_KEY_SELECT: return NES_SELECT;
    default:             return 0;
    }
}

/* Boot keyboard report: [0] modifiers, [1] reserved, [2..7] key codes.
 *
 * Runs on NimBLE's host task, not the emulator's. It only writes one byte of
 * state, which the emulator reads with a single volatile load - so there is no
 * lock and nothing for the two to contend over. */
static void on_report(NimBLERemoteCharacteristic *chr, uint8_t *data,
                      size_t length, bool is_notify)
{
    (void)is_notify;

    /* A keyboard's non-key input reports (consumer keys, ZMK's media layer)
     * are a different length. In boot mode we should not see them at all, but
     * the report-protocol fallback below subscribes more broadly, so length is
     * what tells them apart. */
    if (!data || length < 3) return;

    g_reports++;

#if NES_BLE_DEBUG
    /* The first few in full. If the keys turn out to be at a different offset
     * than the boot layout assumes, this is what shows it - and it is the one
     * thing that cannot be worked out from this side without seeing the bytes. */
    if (g_reports <= 8) {
        /* The handle says which of the input reports this came from, so a
         * consumer report arriving instead of a keyboard one is visible rather
         * than being quietly parsed as keys. */
        Serial.printf("[BLE] report %lu from handle %u (%u bytes):",
                      (unsigned long)g_reports,
                      (unsigned)(chr ? chr->getHandle() : 0),
                      (unsigned)length);
        for (size_t i = 0; i < length && i < 16; i++)
            Serial.printf(" %02x", data[i]);
        Serial.println();
    }
#endif

    uint8_t mask = 0;
    for (size_t i = 2; i < length && i < 8; i++) {
        /* 0x01..0x03 are the rollover/POST error codes, not keys. */
        if (data[i] >= 0x04) mask |= nes_bit_for_usage(data[i]);
    }
    g_buttons = mask;
}

/* NimBLE reports HCI errors as 0x200 + the HCI code. The handful below are the
 * ones that actually show up here, and the difference between them is the
 * difference between "retry" and "your bonds disagree, clear both sides". */
static const char *disconnect_reason(int reason)
{
    switch (reason - 0x200) {
    case 0x05: return "authentication failure";
    case 0x06: return "PIN or key missing - stale bond, clear it on both sides";
    case 0x08: return "connection timeout";
    case 0x13: return "the keyboard closed the connection";
    case 0x14: return "the keyboard is low on resources";
    case 0x15: return "the keyboard is powering off";
    case 0x16: return "we closed the connection";
    case 0x3E: return "connection failed to establish";
    default:   return "";
    }
}

class ClientCallbacks : public NimBLEClientCallbacks {
    void onConnect(NimBLEClient *client) override
    {
        /* Deliberately does NOT set g_ready. A link is not an input device
         * until reports are flowing; saying "connected" here is what made the
         * first version report success while no key ever worked. */
        g_link_up = true;
        set_status("linked: %s", client->getPeerAddress().toString().c_str());
    }

    void onDisconnect(NimBLEClient *client, int reason) override
    {
        (void)client;
        /* Release every button. Otherwise a disconnect mid-jump leaves the
         * NES seeing A held down forever. */
        g_buttons = 0;
        g_link_up = false;
        g_ready   = false;

        const char *why = disconnect_reason(reason);

        if (g_expect_disconnect) {
            /* We hung up ourselves, and whatever made us do that has already
             * said so. Log and keep that message. */
            g_expect_disconnect = false;
            Serial.printf("[BLE] link closed as expected (reason %d)\n", reason);
            return;
        }

        if (why[0])
            set_status("disconnected: %s", why);
        else
            set_status("disconnected (reason %d), rescanning", reason);
    }

    /* HID-over-GATT mandates an encrypted link, so pairing is not optional.
     * A keyboard with no display does Just Works, and this side says it has no
     * input or output either, so no passkey is exchanged. */
    void onPassKeyEntry(NimBLEConnInfo &info) override
    {
        (void)info;
        set_status("keyboard asked for a passkey - unsupported");
    }

    void onAuthenticationComplete(NimBLEConnInfo &info) override
    {
        if (!info.isEncrypted()) {
            set_status("link not encrypted - HID needs pairing");
            return;
        }
        set_status("paired, bonded=%d", (int)info.isBonded());
    }
};

static ClientCallbacks g_client_callbacks;

/* True if this advertisement looks like a keyboard. */
static bool looks_like_keyboard(const NimBLEAdvertisedDevice *dev)
{
    if (dev->isAdvertisingService(NimBLEUUID(UUID_HID_SERVICE))) return true;
    return dev->getAppearance() == APPEARANCE_KEYBOARD;
}

/* Tells the keyboard the host is awake.
 *
 * The HID Control Point's suspend bit is host-driven and defaults to "not
 * suspended", so writing this should be redundant - but a peripheral that
 * decided we were asleep stops sending reports entirely and looks exactly like
 * a peripheral that has nothing to say. One write removes that from the list of
 * possible explanations, which is worth more than the byte costs. */
static void exit_suspend(NimBLERemoteService *hid)
{
    NimBLERemoteCharacteristic *ctl =
        hid->getCharacteristic(NimBLEUUID(UUID_HID_CONTROL));
    if (!ctl) return;

    const uint8_t resume = HID_CONTROL_EXIT_SUSPEND;
    ctl->writeValue(&resume, 1, false);   /* write-without-response, per spec */
    Serial.println("[BLE] sent exit-suspend");
}

/* Subscribes to whichever input report carries key codes. Returns false if the
 * device turned out not to have a usable HID service after all. */
static bool subscribe_to_keys(NimBLEClient *client)
{
    Serial.println("[BLE] discovering HID service...");

    NimBLERemoteService *hid = client->getService(NimBLEUUID(UUID_HID_SERVICE));
    if (!hid) {
        set_status("no HID service on this device");
        return false;
    }

#if NES_BLE_DEBUG
    /* Everything the HID service offers, before we touch any of it. Tells the
     * difference between "this keyboard has no boot-mode characteristic" and
     * "the subscribe call failed", which read identically from the outside.
     *
     * The Report Reference descriptor (0x2908) is the interesting part: two
     * bytes, report ID then report type (1 = Input, 2 = Output, 3 = Feature).
     * A keyboard exposes several identical-looking 0x2A4D characteristics and
     * this is the only thing that says which is which. */
    Serial.println("[BLE] HID characteristics:");
    for (auto *chr : hid->getCharacteristics(true)) {
        Serial.printf("        %s  notify=%d indicate=%d",
                      chr->getUUID().toString().c_str(),
                      (int)chr->canNotify(), (int)chr->canIndicate());

        NimBLERemoteDescriptor *ref =
            chr->getDescriptor(NimBLEUUID(UUID_REPORT_REFERENCE));
        if (ref) {
            NimBLEAttValue v = ref->readValue();
            if (v.size() >= 2) {
                static const char *kind[] = { "?", "input", "output", "feature" };
                Serial.printf("  report id=%u %s", v.data()[0],
                              v.data()[1] <= 3 ? kind[v.data()[1]] : "?");
            }
        }
        Serial.println();
    }
#endif

    /* Read the Report Map before anything else. A HOGP host is expected to,
     * and some peripherals will not start sending until it has been read -
     * from their side an unread report map means the host cannot possibly
     * understand a report yet. It is also the authoritative description of the
     * report layout, which beats inferring it from byte counts. */
    NimBLERemoteCharacteristic *map =
        hid->getCharacteristic(NimBLEUUID(UUID_REPORT_MAP));
    if (map) {
        NimBLEAttValue rm = map->readValue();
        Serial.printf("[BLE] report map: %u bytes", (unsigned)rm.size());
#if NES_BLE_DEBUG
        for (uint16_t i = 0; i < rm.size() && i < 64; i++)
            Serial.printf(" %02x", rm.data()[i]);
        if (rm.size() > 64) Serial.print(" ...");
#endif
        Serial.println();
    } else {
        Serial.println("[BLE] no report map characteristic");
    }

    /* Ask for boot protocol. Without this the keyboard stays in report protocol
     * and sends its own layout. ZMK has no Protocol Mode characteristic at all,
     * so this is skipped there and the fallback below is the real path. */
    NimBLERemoteCharacteristic *mode =
        hid->getCharacteristic(NimBLEUUID(UUID_PROTOCOL_MODE));
    if (mode) {
        const uint8_t boot = BOOT_PROTOCOL_MODE;
        mode->writeValue(&boot, 1, false);   /* write-without-response, per spec */
    }

    NimBLERemoteCharacteristic *boot_in =
        hid->getCharacteristic(NimBLEUUID(UUID_BOOT_KB_INPUT));
    if (boot_in && boot_in->canNotify() && boot_in->subscribe(true, on_report)) {
        exit_suspend(hid);
        set_status("ready (boot protocol)");
        g_ready = true;
        return true;
    }
    Serial.printf("[BLE] boot report: present=%d notify=%d - "
                  "falling back to report protocol\n",
                  (int)(boot_in != nullptr),
                  (int)(boot_in && boot_in->canNotify()));

    /* Fallback: some keyboards do not implement boot mode. Subscribe to every
     * input report and let on_report's length check sort out which is the key
     * one - a keyboard report is 8 bytes, consumer and battery reports are not.
     * Cruder than boot mode, which is why it is second. */
    int subscribed = 0;
    for (auto *chr : hid->getCharacteristics(true)) {
        if (chr->getUUID() != NimBLEUUID(UUID_REPORT)) continue;
        if (!chr->canNotify()) continue;
        if (chr->subscribe(true, on_report)) subscribed++;
    }

    if (subscribed) {
        exit_suspend(hid);
        set_status("ready (%d report chars, no boot mode)", subscribed);
        g_ready = true;
        return true;
    }

    set_status("HID service has no notifying input report");
    return false;
}

/* Case-insensitive substring, so a truncated advertised name still matches. */
static bool name_contains(const std::string &name, const char *want)
{
    if (!want[0] || name.empty()) return false;

    for (size_t i = 0; name[i]; i++) {
        size_t j = 0;
        while (want[j] && name[i + j] &&
               tolower((unsigned char)name[i + j]) ==
               tolower((unsigned char)want[j]))
            j++;
        if (!want[j]) return true;
    }
    return false;
}

/* Is this the keyboard the user picked?
 *
 * An empty name filter used to mean "match anything", which is wrong the moment
 * a target has been chosen by address instead - the search would settle for
 * whatever HID device answered first. It now means "match anything" only when
 * NOTHING at all is configured. That distinction is what stops an unnamed
 * device in the room from being adopted as the keyboard. */
static bool device_matches(const std::string &name, const NimBLEAddress &addr)
{
    const bool have_name = g_want_name[0] != '\0';
    const bool have_addr = g_want_addr[0] != '\0';

    if (!have_name && !have_addr) return true;
    if (have_name && name_contains(name, g_want_name)) return true;
    /* Address is the only handle on a device that advertises no name. It is a
     * weak one - private addresses rotate - but the picker runs every boot, so
     * a device that moved can simply be picked again. */
    if (have_addr && addr.toString() == g_want_addr) return true;
    return false;
}

static bool scan_for_keyboard(void)
{
    NimBLEScan *scan = NimBLEDevice::getScan();
    scan->setActiveScan(true);          /* also fetch the scan response, which
                                         * is where the name usually lives */
    set_status("scanning...");

    NimBLEScanResults results = scan->getResults(5000, false);

    const NimBLEAdvertisedDevice *chosen = nullptr;
    std::string chosen_name;

    for (int i = 0; i < results.getCount(); i++) {
        const NimBLEAdvertisedDevice *dev = results.getDevice((uint32_t)i);
        if (!looks_like_keyboard(dev)) continue;

        std::string name = dev->getName();

        /* Every HID device in range, named or not. When the wanted one is not
         * among them the list says so directly, instead of the search silently
         * settling for whatever else was nearby. */
        const bool match = device_matches(name, dev->getAddress());

        Serial.printf("[BLE] HID device: %-24s %s%s\n",
                      name.empty() ? "(no name)" : name.c_str(),
                      dev->getAddress().toString().c_str(),
                      match ? "  <- match" : "");

        if (!chosen && match) {
            chosen = dev;
            chosen_name = name;
        }
    }

    if (chosen) {
        snprintf(g_name, sizeof g_name, "%s",
                 chosen_name.empty() ? "keyboard" : chosen_name.c_str());
        set_status("found %s", g_name);
        g_target = chosen->getAddress();
        g_have_target = true;
        scan->clearResults();
        return true;
    }

    scan->clearResults();
    if (g_want_name[0])
        set_status("no HID device named '%s' - is it awake?", g_want_name);
    else
        set_status("no keyboard advertising - is it in pairing mode?");
    return false;
}

static bool connect_to_target(void)
{
    /* A live link with no subscription would otherwise wedge this: connect()
     * on an already-connected client fails and we would never get back to a
     * clean state. */
    if (g_client && g_client->isConnected()) { g_expect_disconnect = true; g_client->disconnect(); }

    if (!g_client) {
        g_client = NimBLEDevice::createClient();
        if (!g_client) {
            set_status("could not create a BLE client");
            return false;
        }
        g_client->setClientCallbacks(&g_client_callbacks, false);
        /* A keyboard sends a report the moment a key moves; a slow connection
         * interval would add latency for no power saving worth having here. */
        g_client->setConnectionParams(12, 24, 0, 200);
    }

    if (!g_client->connect(g_target)) {
        set_status("connect failed");
        return false;
    }

    /* HID characteristics are unreadable until the link is encrypted. */
    if (!g_client->secureConnection()) {
        /* Almost always a one-sided bond: we kept a key that the keyboard has
         * since forgotten - ZMK's BT_CLR wipes one profile and says nothing to
         * the host that was on it. Encryption then fails forever, because both
         * ends keep presenting credentials the other rejects.
         *
         * Dropping our half is the only move available from here, and it is
         * safe: a bond is a cache, not data. Worst case we re-pair. Doing it
         * automatically is the difference between this recovering by itself and
         * needing an erase-flash cycle to explain. */
        /* Short enough to survive the 64-byte status buffer, which truncated
         * the previous wording mid-sentence exactly when it mattered. The
         * explanation goes to serial, where there is room. */
        if (NimBLEDevice::isBonded(g_target)) {
            NimBLEDevice::deleteBond(g_target);
            set_status("stale key dropped - now clear it on the keyboard");
            Serial.println(
                "[BLE] We held a pairing key the keyboard no longer accepts, so\n"
                "      ours is gone now. Clear its side too - on ZMK that is\n"
                "      &bt BT_CLR while ITS profile for this board is selected.");
        } else {
            set_status("keyboard refuses to pair - clear its profile");
            Serial.println(
                "[BLE] We hold no key for this keyboard, and it will not pair:\n"
                "      it still has a bond for us and considers that profile\n"
                "      taken. Only the keyboard can drop it - select the profile\n"
                "      this board was on and press &bt BT_CLR, then power-cycle\n"
                "      the board. Nothing on this end can force it.");
        }
        g_expect_disconnect = true;
        g_client->disconnect();
        g_have_target = false;      /* rescan: the address may have rotated */
        return false;
    }

    if (!subscribe_to_keys(g_client)) {
        g_expect_disconnect = true;
        g_client->disconnect();
        return false;
    }

    return true;
}

static void ble_task(void *arg)
{
    (void)arg;

    uint32_t silent_ticks = 0;
    bool     warned = false;
    int      failures = 0;

    for (;;) {
        if (!g_ready) {
            /* Try the address we already know before scanning again: after a
             * keyboard sleeps and wakes, the bond is still valid and this
             * reconnects in well under a second. */
            bool ok = false;
            if (g_have_target) ok = connect_to_target();
            if (!ok && scan_for_keyboard()) ok = connect_to_target();

            /* Back off once it is clear this needs a human. The usual reason -
             * a bond only the keyboard still holds - cannot resolve itself, and
             * retrying every two seconds forever just fills the log and drains
             * the battery while the game is running. */
            failures = ok ? 0 : failures + 1;
            if (failures >= 3) {
                if (failures == 3)
                    Serial.println("[BLE] repeated failures - retrying every "
                                   "20 s from now, the cause needs the "
                                   "keyboard's side to change");
                vTaskDelay(pdMS_TO_TICKS(20000));
            }

            silent_ticks = 0;
            warned = false;
        } else if (g_reports == 0) {
            /* Subscribed, encrypted, and nothing is coming. Almost always the
             * host end of a keyboard that is talking to somebody else - say so
             * rather than leaving a silent screen to be interpreted. */
            if (++silent_ticks >= 5 && !warned) {
                warned = true;
                Serial.println(
                    "[BLE] linked and subscribed, but no reports in 5 s.\n"
                    "      On ZMK a keyboard stays connected to EVERY bonded\n"
                    "      host and sends keys only to the ACTIVE profile.\n"
                    "      Select this board's profile with &bt BT_SEL <n>.\n"
                    "      If it is still advertising as pairable, its active\n"
                    "      profile is a different, empty one.");
            }
        }
        vTaskDelay(pdMS_TO_TICKS(g_ready ? 1000 : 2000));
    }
}

void ble_keyboard_init(void)
{
    NimBLEDevice::init("NES-RLCD");

    /* bonding so a keyboard reconnects without re-pairing; no MITM because
     * neither side has a display or keypad to compare a passkey on; secure
     * connections because every stack from the last decade supports it. */
    NimBLEDevice::setSecurityAuth(true, false, true);
    NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);

    /* What we already remember, so a pairing failure can be read against it
     * rather than guessed at. Bonds live in NVS and survive reflashing the
     * sketch - which is exactly why one can outlive the keyboard's own copy. */
    const int bonds = NimBLEDevice::getNumBonds();
    Serial.printf("[BLE] %d bond(s) stored\n", bonds);
    for (int i = 0; i < bonds; i++)
        Serial.printf("        %s\n",
                      NimBLEDevice::getBondedAddress(i).toString().c_str());

    /* The remembered choice, if the menu has ever been through. Falls back to
     * the NES_BLE_KEYBOARD_NAME compile-time default, which is what makes this
     * work on a board that has never seen the menu. */
    Preferences prefs;
    if (prefs.begin(PREFS_NS, true /* read only */)) {
        if (prefs.getStringLength(PREFS_NAME) > 0)
            prefs.getString(PREFS_NAME, g_want_name, sizeof g_want_name);
        if (prefs.getStringLength(PREFS_ADDR) > 0)
            prefs.getString(PREFS_ADDR, g_want_addr, sizeof g_want_addr);
        prefs.end();
    }
    Serial.printf("[BLE] looking for '%s'%s\n",
                  g_want_name[0] ? g_want_name : "(any HID device)",
                  g_want_addr[0] ? " (or its saved address)" : "");
}

void ble_keyboard_set_target(const char *name, const char *addr)
{
    snprintf(g_want_name, sizeof g_want_name, "%s", name ? name : "");
    snprintf(g_want_addr, sizeof g_want_addr, "%s", addr ? addr : "");
    g_have_target = false;              /* force a fresh scan */

    Preferences prefs;
    if (prefs.begin(PREFS_NS, false)) {
        prefs.putString(PREFS_NAME, g_want_name);
        prefs.putString(PREFS_ADDR, g_want_addr);
        prefs.end();
    }
    Serial.printf("[BLE] target saved: '%s' %s\n", g_want_name, g_want_addr);
}

void ble_keyboard_forget(void)
{
    Preferences prefs;
    if (prefs.begin(PREFS_NS, false)) {
        prefs.clear();
        prefs.end();
    }
    g_want_name[0] = '\0';
    g_want_addr[0] = '\0';
    g_have_target  = false;

    /* The pairing keys matter more than the choice. A bond the keyboard has
     * already dropped can never re-establish encryption, and no BLE peer can be
     * asked to forget you - so the only recoverable state is none. */
    const int n = NimBLEDevice::getNumBonds();
    NimBLEDevice::deleteAllBonds();
    Serial.printf("[BLE] forgot the saved keyboard and %d bond(s)\n", n);
}

const char *ble_keyboard_target_name(void)
{
    return g_want_name[0] ? g_want_name : g_want_addr;
}

void ble_keyboard_disable(void)
{
    g_disabled = true;
    set_status("no keyboard selected");
}

int ble_keyboard_scan(ble_kb_device_t *out, int max, uint32_t ms)
{
    if (!out || max <= 0) return 0;

    NimBLEScan *scan = NimBLEDevice::getScan();
    scan->setActiveScan(true);
    NimBLEScanResults results = scan->getResults(ms, false);

    int n = 0;
    for (int i = 0; i < results.getCount() && n < max; i++) {
        const NimBLEAdvertisedDevice *dev = results.getDevice((uint32_t)i);
        if (!looks_like_keyboard(dev)) continue;

        const std::string name = dev->getName();
        const NimBLEAddress addr = dev->getAddress();

        /* The RAW name, empty if there is none. "(no name)" is a thing to show
         * a human, and storing it here would send it into the matcher as a name
         * to look for - which nothing ever advertises, so the keyboard would
         * never be found again. */
        snprintf(out[n].name, sizeof out[n].name, "%s", name.c_str());
        snprintf(out[n].addr, sizeof out[n].addr, "%s",
                 addr.toString().c_str());
        out[n].bonded = NimBLEDevice::isBonded(addr);
        n++;
    }
    scan->clearResults();
    return n;
}

void ble_keyboard_begin(void)
{
    if (g_disabled) {
        Serial.println("[BLE] no keyboard selected - radio idle");
        return;
    }

    /* Core 0: the Arduino loop, and therefore the emulator, owns core 1. 6 KB
     * of stack because NimBLE's service discovery is not shallow. */
    xTaskCreatePinnedToCore(ble_task, "ble_kbd", 6144, nullptr, 2, nullptr, 0);
}

int ble_keyboard_buttons(void)
{
    return g_buttons;
}

bool ble_keyboard_connected(void)
{
    /* g_ready, not g_link_up: callers use this to decide whether the keyboard
     * can actually play, and a linked-but-silent keyboard cannot. */
    return g_ready;
}

const char *ble_keyboard_status(void)
{
    return g_status;
}

#else  /* NES_BLE_KEYBOARD disabled */

#include "ble_keyboard.h"

void ble_keyboard_init(void)  {}
void ble_keyboard_begin(void) {}
void ble_keyboard_forget(void) {}
void ble_keyboard_set_target(const char *name, const char *addr)
{
    (void)name;
    (void)addr;
}
int  ble_keyboard_scan(ble_kb_device_t *out, int max, uint32_t ms)
{
    (void)out;
    (void)max;
    (void)ms;
    return 0;
}
void        ble_keyboard_disable(void)      {}
const char *ble_keyboard_target_name(void)  { return ""; }
int         ble_keyboard_buttons(void)      { return 0; }
bool        ble_keyboard_connected(void)    { return false; }
const char *ble_keyboard_status(void)       { return "disabled"; }
bool        ble_keyboard_menu(void)         { return false; }

#endif /* NES_BLE_KEYBOARD */
