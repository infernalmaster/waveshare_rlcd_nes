# src/nofrendo — vendored NES emulator core

Nofrendo, by way of
[Esp32NofrendobyDSN](https://github.com/derdacavga/Esp32-nes-emulator-by-DSN)
0.9.0. Upstream's own README is kept as
[README.upstream.md](README.upstream.md); its licence is [LICENSE](LICENSE).

It sits here rather than in `~/Documents/Arduino/libraries` because Arduino
compiles a sketch's `src/` subfolder recursively: the fork travels with the
sketch, and there is no installed copy to fall out of sync with. (It used to be
installed as a library, which went wrong exactly that way — a stale copy meant
edits here never reached the flashed binary, with no compile error to say so.)
**If you have `Esp32NofrendobyDSN` installed, remove it** — upstream has no
ST7305 support and this sketch does not use the installed copy.

Only the upstream `src/` tree came across. `library.properties`,
`keywords.txt`, `img/` and the upstream ST7789 example were dropped: they are
library-packaging artifacts with nothing to package, and the example's
`controller.cpp` would have been compiled into this sketch and collided with
ours. They are still in the upstream release if they are ever wanted back.

## What the fork changes

- **`tft_driver.h`** — under `NES_USE_ST7305`, `TFTDriver` becomes `ST7305Tft`
  from the sketch root instead of the built-in ST7789 class. Same methods, but
  it buffers: drawing does not reach the glass until `flush()`.
- **`osd.cpp`** — the video and audio path: 256×240 RGB to 400×300 ink-or-paper
  through an ordered dither, flushes phase-locked to the panel's TE pin, and
  `nes_codec.h` for board-specific codec bring-up.

## Include rules

**Quotes, never angle brackets, for anything in this folder.** As an installed
library this tree had its own directory on the include path, so upstream wrote
`#include <bitmap.h>` for its own headers. A sketch's `src/` subfolder gets no
such `-I` — only the sketch root is on the path — so all 66 of those were
rewritten to `#include "bitmap.h"`, which the compiler resolves relative to the
file doing the including. Add a new header here and quote it, or it will not be
found. (This is the one thing the move actually broke; `gui.c` → `nes_ppu.h` →
`<bitmap.h>` was the first to fail.)

Files here may also include headers from the **sketch root** one level up —
`hw_config.h`, `st7305_tft.h`, `st7305_gfx.h`, `nes_codec.h` — unqualified,
because the Arduino builder does put the sketch root on the include path for
every translation unit. Going the other way needs the path: sketch-root files
reach in with `#include "src/nofrendo/tft_driver.h"`.
