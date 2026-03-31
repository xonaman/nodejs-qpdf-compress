#pragma once

#include <cstdint>
#include <set>
#include <vector>

// subset a TrueType font binary (TTF), keeping only the specified glyph IDs.
// glyph 0 (.notdef) is always retained. returns true on success.
bool subsetTrueTypeFont(const uint8_t *data, size_t size,
                        const std::set<uint16_t> &usedGlyphIds,
                        std::vector<uint8_t> &output);

// map character codes to glyph IDs using the font's cmap table.
// returns a set of glyph IDs that correspond to the given character codes.
std::set<uint16_t> mapCodesToGlyphIds(const uint8_t *data, size_t size,
                                      const std::set<uint16_t> &charCodes);
