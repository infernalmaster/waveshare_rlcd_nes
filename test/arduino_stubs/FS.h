/* FS.h - stub, host compilation only. See Arduino.h in this folder.
 *
 * Enough of the Arduino filesystem API for a sketch that walks a card looking
 * for files. Nothing here touches a filesystem: File is always false, open()
 * always fails, and a loop over openNextFile() ends immediately. That is fine
 * for what these stubs are for - it type-checks the calls and the format
 * strings around them - and useless for anything else.
 */
#ifndef FS_STUB_H
#define FS_STUB_H

#include <stdint.h>
#include <stddef.h>

#define FILE_READ  "r"
#define FILE_WRITE "w"
#define FILE_APPEND "a"

namespace fs {

class File {
public:
    operator bool() const;
    File openNextFile(const char *mode = FILE_READ);
    bool        isDirectory(void);
    const char *name(void);
    const char *path(void);
    size_t      size(void);
    void        close(void);
    int         read(uint8_t *buf, size_t len);
    size_t      readBytes(char *buf, size_t len);
    bool        seek(uint32_t pos);
};

class FS {
public:
    File open(const char *path, const char *mode = FILE_READ);
    bool exists(const char *path);
};

} /* namespace fs */

using fs::File;
using fs::FS;

#endif /* FS_STUB_H */
