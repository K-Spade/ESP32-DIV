#include "UnicodeTextRenderer.h"

#include "UnicodeTextAssets.h"

namespace {

uint32_t textHash(const char* text) {
  uint32_t hash = 2166136261u;
  if (!text) return hash;
  while (*text) {
    hash ^= (uint8_t)*text++;
    hash *= 16777619u;
  }
  return hash;
}

bool findAsset(const char* text, UnicodeTextAsset& out) {
  if (!text || !*text) return false;
  const uint32_t wanted = textHash(text);
  size_t low = 0;
  size_t high = kUnicodeTextAssetCount;
  while (low < high) {
    const size_t mid = low + ((high - low) / 2u);
    const uint32_t value =
        pgm_read_dword(&kUnicodeTextAssets[mid].hash);
    if (value < wanted) low = mid + 1u;
    else high = mid;
  }
  if (low >= kUnicodeTextAssetCount ||
      pgm_read_dword(&kUnicodeTextAssets[low].hash) != wanted) {
    return false;
  }
  memcpy_P(&out, &kUnicodeTextAssets[low], sizeof(out));
  return true;
}

}  // namespace

bool unicodeTextMetrics(const char* utf8, UnicodeTextMetrics& out) {
  UnicodeTextAsset asset{};
  if (!findAsset(utf8, asset)) return false;
  out.width = asset.width;
  out.height = asset.height;
  return true;
}

int16_t unicodeTextDraw(TFT_eSPI& display, const char* utf8, int32_t x,
                        int32_t y, uint16_t foreground,
                        uint16_t background) {
  UnicodeTextAsset asset{};
  if (!findAsset(utf8, asset)) return 0;
  if (foreground != background) {
    display.fillRect(x, y, asset.width, asset.height, background);
  }
  for (uint16_t row = 0; row < asset.height; ++row) {
    uint16_t column = 0;
    while (column < asset.width) {
      const uint32_t byteOffset =
          asset.offset + (uint32_t)row * asset.stride + (column >> 3);
      const uint8_t packed = pgm_read_byte(kUnicodeTextBitmap + byteOffset);
      const bool lit = (packed & (0x80u >> (column & 7u))) != 0u;
      if (!lit) {
        ++column;
        continue;
      }
      const uint16_t start = column;
      do {
        ++column;
        if (column >= asset.width) break;
        const uint32_t nextOffset =
            asset.offset + (uint32_t)row * asset.stride + (column >> 3);
        const uint8_t next = pgm_read_byte(kUnicodeTextBitmap + nextOffset);
        if ((next & (0x80u >> (column & 7u))) == 0u) break;
      } while (true);
      display.drawFastHLine(x + start, y + row, column - start, foreground);
    }
  }
  return (int16_t)asset.width;
}
