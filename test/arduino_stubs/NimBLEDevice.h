/* NimBLEDevice.h - stub, host compilation only. See Arduino.h in this folder.
 *
 * Every signature here was transcribed from the real NimBLE-Arduino 2.5.1
 * headers (src/NimBLEClient.h, NimBLERemoteCharacteristic.h, NimBLEScan.h,
 * NimBLEUUID.h, NimBLEConnInfo.h, NimBLERemoteValueAttribute.h), not from
 * memory - so a wrong argument count, a wrong type or a missing const is caught
 * here rather than after a flash cycle.
 *
 * BE CLEAR ABOUT WHAT THIS PROVES. It checks that a sketch calls the 2.x API
 * correctly. It cannot check that the API does what the sketch assumes: no
 * scan runs, no connection is made, nothing pairs, and no notification ever
 * arrives. Bluetooth behaviour is entirely unexercised.
 *
 * NimBLE 1.x will not compile against a sketch written for this - 1.x used
 * setAdvertisedDeviceCallbacks and a different notify callback signature. That
 * is deliberate: failing loudly beats connecting to nothing.
 */
#ifndef NIMBLE_DEVICE_STUB_H
#define NIMBLE_DEVICE_STUB_H

#include <stdint.h>
#include <stddef.h>
#include <functional>
#include <string>
#include <vector>

/* From the NimBLE host headers, which the real NimBLEDevice.h pulls in. */
#define BLE_HS_IO_NO_INPUT_OUTPUT 0x03
#define BLE_HS_IO_DISPLAY_ONLY    0x00
#define BLE_HS_IO_KEYBOARD_ONLY   0x02

class NimBLEUUID {
public:
    NimBLEUUID() = default;
    NimBLEUUID(const std::string &uuid);
    NimBLEUUID(uint16_t uuid);
    NimBLEUUID(uint32_t uuid);
    bool operator==(const NimBLEUUID &rhs) const;
    bool operator!=(const NimBLEUUID &rhs) const;
    std::string toString() const;
};

class NimBLEAddress {
public:
    std::string toString() const;
};

class NimBLEConnInfo {
public:
    bool isBonded() const;
    bool isEncrypted() const;
    bool isAuthenticated() const;
};

class NimBLEAdvertisedDevice {
public:
    uint16_t             getAppearance() const;
    const NimBLEAddress &getAddress() const;
    std::string          getName() const;
    bool                 isAdvertisingService(const NimBLEUUID &uuid) const;
};

class NimBLEScanResults {
public:
    int                          getCount() const;
    const NimBLEAdvertisedDevice *getDevice(uint32_t idx) const;
};

class NimBLEScanCallbacks {
public:
    virtual ~NimBLEScanCallbacks() {}
    virtual void onDiscovered(const NimBLEAdvertisedDevice *advertisedDevice);
    virtual void onResult(const NimBLEAdvertisedDevice *advertisedDevice);
    virtual void onScanEnd(const NimBLEScanResults &scanResults, int reason);
};

class NimBLEScan {
public:
    void              setScanCallbacks(NimBLEScanCallbacks *cb,
                                       bool wantDuplicates = false);
    void              setActiveScan(bool active);
    NimBLEScanResults getResults();
    NimBLEScanResults getResults(uint32_t duration, bool is_continue = false);
    void              clearResults();
};

class NimBLERemoteCharacteristic;

typedef std::function<void(NimBLERemoteCharacteristic *chr, uint8_t *data,
                           size_t length, bool isNotify)> notify_callback;

/* The read side of a characteristic or descriptor value. size()/data() are what
 * a caller needs to look at raw bytes such as a Report Reference descriptor. */
class NimBLEAttValue {
public:
    uint16_t       size() const;
    uint16_t       max_size() const;
    const uint8_t *data() const;
};

class NimBLERemoteDescriptor {
public:
    const NimBLEUUID &getUUID() const;
    NimBLEAttValue    readValue();
};

class NimBLERemoteCharacteristic {
public:
    const NimBLEUUID &getUUID() const;
    uint16_t getHandle() const;
    bool canNotify() const;
    bool canIndicate() const;
    bool subscribe(bool notifications = true,
                   const notify_callback notifyCallback = nullptr,
                   bool response = true) const;
    bool writeValue(const uint8_t *data, size_t length,
                    bool response = false) const;
    NimBLEAttValue readValue();
    NimBLERemoteDescriptor *getDescriptor(const NimBLEUUID &uuid) const;
    const std::vector<NimBLERemoteDescriptor *> &
        getDescriptors(bool refresh = false) const;
};

class NimBLERemoteService {
public:
    NimBLERemoteCharacteristic *getCharacteristic(const NimBLEUUID &uuid) const;
    const std::vector<NimBLERemoteCharacteristic *> &
        getCharacteristics(bool refresh = false) const;
};

class NimBLEClient;

class NimBLEClientCallbacks {
public:
    virtual ~NimBLEClientCallbacks() {}
    virtual void     onConnect(NimBLEClient *pClient);
    virtual void     onConnectFail(NimBLEClient *pClient, int reason);
    virtual void     onDisconnect(NimBLEClient *pClient, int reason);
    virtual void     onPassKeyEntry(NimBLEConnInfo &connInfo);
    virtual uint32_t onPassKeyDisplay(NimBLEConnInfo &connInfo);
    virtual void     onAuthenticationComplete(NimBLEConnInfo &connInfo);
    virtual void     onConfirmPasskey(NimBLEConnInfo &connInfo, uint32_t pin);
    virtual void     onIdentity(NimBLEConnInfo &connInfo);
    virtual void     onMTUChange(NimBLEClient *pClient, uint16_t MTU);
};

class NimBLEClient {
public:
    bool connect(const NimBLEAdvertisedDevice *device,
                 bool deleteAttributes = true, bool asyncConnect = false,
                 bool exchangeMTU = true);
    bool connect(const NimBLEAddress &address, bool deleteAttributes = true,
                 bool asyncConnect = false, bool exchangeMTU = true);
    bool                 disconnect();
    bool                 isConnected();
    NimBLEAddress        getPeerAddress() const;
    void                 setClientCallbacks(NimBLEClientCallbacks *cb,
                                            bool deleteCallbacks = true);
    bool                 secureConnection(bool async = false) const;
    void                 setConnectionParams(uint16_t minInterval,
                                             uint16_t maxInterval,
                                             uint16_t latency, uint16_t timeout,
                                             uint16_t scanInterval = 16,
                                             uint16_t scanWindow = 16);
    NimBLERemoteService *getService(const NimBLEUUID &uuid);
};

class NimBLEDevice {
public:
    static bool          init(const std::string &deviceName);
    static void          deinit(bool clearAll = false);
    static NimBLEScan   *getScan();
    static NimBLEClient *createClient();
    static bool          deleteClient(NimBLEClient *pClient);
    static void          setSecurityAuth(bool bonding, bool mitm, bool sc);
    static void          setSecurityAuth(uint8_t auth);
    static void          setSecurityIOCap(uint8_t iocap);

    /* Bond storage. These live in NVS and outlive a sketch upload, which is
     * what makes a one-sided bond possible in the first place. */
    static bool          deleteBond(const NimBLEAddress &address);
    static bool          deleteAllBonds();
    static int           getNumBonds();
    static bool          isBonded(const NimBLEAddress &address);
    static NimBLEAddress getBondedAddress(int index);
};

#endif /* NIMBLE_DEVICE_STUB_H */
