/* Preferences.h - stub, host compilation only. See Arduino.h in this folder.
 *
 * The Arduino ESP32 core's key/value wrapper over NVS. On the host nothing
 * persists: begin() fails, every get returns the default, so a sketch compiled
 * here always takes its "nothing saved yet" path. Which is, as ever, the only
 * path these stubs can exercise.
 */
#ifndef PREFERENCES_STUB_H
#define PREFERENCES_STUB_H

#include <stdint.h>
#include <stddef.h>

class Preferences {
public:
    bool   begin(const char *name, bool readOnly = false,
                 const char *partition_label = nullptr);
    void   end();
    bool   clear();
    bool   remove(const char *key);

    size_t putString(const char *key, const char *value);
    size_t getString(const char *key, char *value, size_t maxLen);
    size_t getStringLength(const char *key);

    size_t   putUInt(const char *key, uint32_t value);
    uint32_t getUInt(const char *key, uint32_t defaultValue = 0);
    size_t   putBool(const char *key, bool value);
    bool     getBool(const char *key, bool defaultValue = false);
    bool     isKey(const char *key);
};

#endif /* PREFERENCES_STUB_H */
