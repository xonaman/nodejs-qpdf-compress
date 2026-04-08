#pragma once

#include <cstddef>
#include <cstdint>
#include <set>
#include <vector>

// subset a font binary (TrueType or CFF/OpenType), keeping only the specified
// glyph IDs. glyph 0 (.notdef) is always retained.
// when preserveCmap is false, the cmap table may be subset or dropped (suitable
// for CID fonts where PDF viewers use /CIDToGIDMap instead).
// returns true on success.
bool subsetFont(const uint8_t *data, size_t size,
                const std::set<uint16_t> &usedGlyphIds,
                std::vector<uint8_t> &output, bool preserveCmap = true);

// map character codes to glyph IDs using the font's cmap table.
// returns a set of glyph IDs that correspond to the given character codes.
std::set<uint16_t> mapCodesToGlyphIds(const uint8_t *data, size_t size,
                                      const std::set<uint16_t> &charCodes);
