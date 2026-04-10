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

// map Unicode code points to glyph IDs using HarfBuzz's cmap lookup.
// returns a set of glyph IDs that correspond to the given Unicode code points.
std::set<uint16_t> mapCodesToGlyphIds(const uint8_t *data, size_t size,
                                      const std::set<uint16_t> &charCodes);

// map glyph names (e.g. "germandbls", "Adieresis") to glyph IDs using
// HarfBuzz. returns a set of glyph IDs for the given glyph names.
std::set<uint16_t>
mapGlyphNamesToGlyphIds(const uint8_t *data, size_t size,
                        const std::vector<std::string> &names);
