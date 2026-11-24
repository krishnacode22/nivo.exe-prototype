# C3 Arcade sprites — usage notes

`c3_arcade_sprites.h` is a complete replacement header for a 128×64 ESP32-C3 OLED.
It preserves the 28 original sprites (12 game sprites and 16 faces) and adds:

- 6 common UI controls, normalized for the OLED.
- 64 item icons in 32×32 and compact 16×16 forms.
- 240 tiles in 32×32 and compact 16×16 forms (12 columns × 20 rows).
- 30 buff icons in 32×32 and compact 16×16 forms (6 columns × 5 rows).

Every bitmap has a compile-time `C3A_STATIC_ASSERT` against `((width + 7) / 8) * height`; 705 byte-count assertions were emitted. XBM uses row padding, so a 13×13 sprite is 26 bytes—not 22.

## Drawing

```cpp
#include "c3_arcade_sprites.h"

c3aDrawSprite(u8g2, 0, 0, C3A_ITEMS[0]);       // 32×32
u8g2.drawXBM(40, 0, 16, 16, ITEM_00_16);       // compact item
c3aDrawSprite(u8g2, 96, 32, C3A_BUFFS[3]);
c3aDrawSprite(u8g2, 0, 32, C3A_TILES[12]);     // tile 1,0
```

For a 128×64 screen, use 32px assets for large UI or a 4×2 tile view; use 16px variants for dense inventories (8×4 icons) or an 8×4 tile view. U8g2's `drawXBM` accepts these LSB-first arrays directly.

## Memory

The arrays are `static const ... PROGMEM`, so they live in flash, not dynamic RAM. The complete bitmap payload is 56,181 bytes (54.9 KiB) of flash; the header source is larger because every byte is written as hexadecimal text. If flash is tight, delete unused numbered arrays or remove all `*_16` variants (about 10 KB). The three metadata tables use pointers in RAM (about 2.7 KB on ESP32); remove the tables and call named arrays directly to reclaim that RAM.
