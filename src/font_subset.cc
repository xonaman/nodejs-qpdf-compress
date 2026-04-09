#include "font_subset.h"

#include <cstring>
#include <map>
#include <vector>

// ---------------------------------------------------------------------------
// TrueType binary helpers — big-endian reads
// ---------------------------------------------------------------------------

static uint16_t readU16(const uint8_t *p) {
  return static_cast<uint16_t>((p[0] << 8) | p[1]);
}

static uint32_t readU32(const uint8_t *p) {
  return (static_cast<uint32_t>(p[0]) << 24) |
         (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) | p[3];
}

// ---------------------------------------------------------------------------
// Table directory parsing
// ---------------------------------------------------------------------------

struct TableRecord {
  uint32_t tag;
  uint32_t checksum;
  uint32_t offset;
  uint32_t length;
};

static uint32_t tag(const char *s) {
  return (static_cast<uint32_t>(s[0]) << 24) |
         (static_cast<uint32_t>(s[1]) << 16) |
         (static_cast<uint32_t>(s[2]) << 8) | s[3];
}

static bool parseTables(const uint8_t *data, size_t size,
                        std::map<uint32_t, TableRecord> &tables) {
  if (size < 12)
    return false;

  uint16_t numTables = readU16(data + 4);
  if (size < 12 + static_cast<size_t>(numTables) * 16)
    return false;

  for (uint16_t i = 0; i < numTables; ++i) {
    const uint8_t *entry = data + 12 + i * 16;
    TableRecord rec;
    rec.tag = readU32(entry);
    rec.checksum = readU32(entry + 4);
    rec.offset = readU32(entry + 8);
    rec.length = readU32(entry + 12);

    if (rec.offset + rec.length > size)
      return false;

    tables[rec.tag] = rec;
  }
  return true;
}

// ---------------------------------------------------------------------------
// cmap parsing — extract character code → glyph ID mapping
// ---------------------------------------------------------------------------

std::set<uint16_t> mapCodesToGlyphIds(const uint8_t *data, size_t size,
                                      const std::set<uint16_t> &charCodes) {
  std::set<uint16_t> glyphIds;
  glyphIds.insert(0); // always keep .notdef

  std::map<uint32_t, TableRecord> tables;
  if (!parseTables(data, size, tables))
    return glyphIds;

  auto it = tables.find(tag("cmap"));
  if (it == tables.end())
    return glyphIds;

  const uint8_t *cmap = data + it->second.offset;
  size_t cmapLen = it->second.length;
  if (cmapLen < 4)
    return glyphIds;

  uint16_t numSubtables = readU16(cmap + 2);

  // helper: look up a single code in a format 4 subtable
  auto lookupFormat4 = [&](const uint8_t *sub, uint16_t code) -> uint16_t {
    if (sub + 14 > cmap + cmapLen)
      return 0;
    uint16_t segCount = readU16(sub + 6) / 2;
    const uint8_t *endCodes = sub + 14;
    const uint8_t *startCodes = endCodes + segCount * 2 + 2;
    const uint8_t *idDeltas = startCodes + segCount * 2;
    const uint8_t *idRangeOffsets = idDeltas + segCount * 2;
    if (idRangeOffsets + segCount * 2 > cmap + cmapLen)
      return 0;

    for (uint16_t seg = 0; seg < segCount; ++seg) {
      uint16_t endCode = readU16(endCodes + seg * 2);
      uint16_t startCode = readU16(startCodes + seg * 2);
      if (code < startCode || code > endCode)
        continue;

      uint16_t rangeOffset = readU16(idRangeOffsets + seg * 2);
      uint16_t delta = readU16(idDeltas + seg * 2);

      uint16_t gid;
      if (rangeOffset == 0) {
        gid = static_cast<uint16_t>((code + delta) & 0xFFFF);
      } else {
        const uint8_t *glyphIdAddr =
            idRangeOffsets + seg * 2 + rangeOffset +
            2 * static_cast<uint16_t>(code - startCode);
        if (glyphIdAddr + 2 > cmap + cmapLen)
          return 0;
        gid = readU16(glyphIdAddr);
        if (gid != 0)
          gid = static_cast<uint16_t>((gid + delta) & 0xFFFF);
      }
      return gid;
    }
    return 0;
  };

  // helper: look up a single code in a format 0 subtable
  auto lookupFormat0 = [&](const uint8_t *sub, uint16_t code) -> uint16_t {
    if (code >= 256 || sub + 6 + 256 > cmap + cmapLen)
      return 0;
    return sub[6 + code];
  };

  // helper: look up a single code in a format 6 subtable
  auto lookupFormat6 = [&](const uint8_t *sub, uint16_t code) -> uint16_t {
    if (sub + 10 > cmap + cmapLen)
      return 0;
    uint16_t firstCode = readU16(sub + 6);
    uint16_t entryCount = readU16(sub + 8);
    if (code < firstCode || code >= firstCode + entryCount)
      return 0;
    size_t off = 10 + (code - firstCode) * 2;
    if (sub + off + 2 > cmap + cmapLen)
      return 0;
    return readU16(sub + off);
  };

  // helper: look up a single code in a format 12 subtable (segmented coverage)
  auto lookupFormat12 = [&](const uint8_t *sub, uint16_t code) -> uint16_t {
    // format 12 header: uint16 format, uint16 reserved, uint32 length,
    // uint32 language, uint32 numGroups
    if (sub + 16 > cmap + cmapLen)
      return 0;
    uint32_t numGroups = readU32(sub + 12);
    // guard against overflow: numGroups * 12 must fit in remaining bytes
    size_t remaining = static_cast<size_t>((cmap + cmapLen) - (sub + 16));
    if (numGroups > remaining / 12)
      return 0;
    const uint8_t *groups = sub + 16;
    if (groups + numGroups * 12 > cmap + cmapLen)
      return 0;

    // each group: uint32 startCharCode, uint32 endCharCode, uint32
    // startGlyphID
    for (uint32_t g = 0; g < numGroups; ++g) {
      const uint8_t *grp = groups + g * 12;
      uint32_t startChar = readU32(grp);
      uint32_t endChar = readU32(grp + 4);
      uint32_t startGlyph = readU32(grp + 8);
      if (code >= startChar && code <= endChar) {
        uint32_t gid = startGlyph + (code - startChar);
        return static_cast<uint16_t>(gid & 0xFFFF);
      }
    }
    return 0;
  };

  // collect all subtables with their platform/encoding info
  struct CmapSubtable {
    uint16_t platformId;
    uint16_t encodingId;
    uint16_t format;
    const uint8_t *data;
  };
  std::vector<CmapSubtable> subtables;

  for (uint16_t i = 0; i < numSubtables; ++i) {
    if (4 + i * 8 + 8 > cmapLen)
      break;
    const uint8_t *entry = cmap + 4 + i * 8;
    uint16_t platformId = readU16(entry);
    uint16_t encodingId = readU16(entry + 2);
    uint32_t subtableOffset = readU32(entry + 4);
    if (subtableOffset + 2 > cmapLen)
      continue;
    uint16_t fmt = readU16(cmap + subtableOffset);
    if (fmt == 0 || fmt == 4 || fmt == 6 || fmt == 12)
      subtables.push_back({platformId, encodingId, fmt, cmap + subtableOffset});
  }

  // for each character code, try all subtables to find glyph IDs.
  // also try 0xF000 offset for Windows Symbol fonts (platform 3, encoding 0).
  for (uint16_t code : charCodes) {
    for (auto &sub : subtables) {
      // try direct lookup
      uint16_t gid = 0;
      if (sub.format == 4)
        gid = lookupFormat4(sub.data, code);
      else if (sub.format == 0)
        gid = lookupFormat0(sub.data, code);
      else if (sub.format == 6)
        gid = lookupFormat6(sub.data, code);
      else if (sub.format == 12)
        gid = lookupFormat12(sub.data, code);

      if (gid != 0) {
        glyphIds.insert(gid);
        continue;
      }

      // try 0xF000 offset for symbol fonts
      if (sub.platformId == 3 && sub.encodingId == 0 && code < 256) {
        uint16_t symCode = code + 0xF000;
        if (sub.format == 4)
          gid = lookupFormat4(sub.data, symCode);
        else if (sub.format == 6)
          gid = lookupFormat6(sub.data, symCode);
        else if (sub.format == 12)
          gid = lookupFormat12(sub.data, symCode);
        if (gid != 0)
          glyphIds.insert(gid);
      }
    }
  }

  return glyphIds;
}

// ---------------------------------------------------------------------------
// Font subsetting using HarfBuzz hb-subset (TrueType and CFF/OpenType)
// ---------------------------------------------------------------------------

#include <hb-subset.h>
#include <hb.h>

bool subsetFont(const uint8_t *data, size_t size,
                const std::set<uint16_t> &usedGlyphIds,
                std::vector<uint8_t> &output, bool preserveCmap) {
  // validate font header — accept TrueType (0x00010000, 'true') and
  // OpenType-CFF ('OTTO')
  if (size < 12)
    return false;
  uint32_t sfVersion = readU32(data);
  if (sfVersion != 0x00010000 && sfVersion != 0x74727565 &&
      sfVersion != 0x4F54544F) // OTTO
    return false;

  // let HarfBuzz own a copy of the font data to avoid any lifetime issues
  hb_blob_t *blob = hb_blob_create(reinterpret_cast<const char *>(data), size,
                                   HB_MEMORY_MODE_DUPLICATE, nullptr, nullptr);
  hb_face_t *face = hb_face_create(blob, 0);
  hb_blob_destroy(blob);

  if (hb_face_get_glyph_count(face) == 0) {
    hb_face_destroy(face);
    return false;
  }

  hb_subset_input_t *input = hb_subset_input_create_or_fail();
  if (!input) {
    hb_face_destroy(face);
    return false;
  }

  // retain original glyph IDs so all existing PDF text references remain
  // valid — unused glyph slots are zeroed out
  hb_subset_input_set_flags(input, HB_SUBSET_FLAGS_RETAIN_GIDS);

  // drop font tables unnecessary for PDF rendering
  hb_set_t *dropTables =
      hb_subset_input_set(input, HB_SUBSET_SETS_DROP_TABLE_TAG);
  hb_set_add(dropTables, HB_TAG('G', 'P', 'O', 'S')); // OpenType positioning
  hb_set_add(dropTables, HB_TAG('G', 'S', 'U', 'B')); // OpenType substitution
  hb_set_add(dropTables, HB_TAG('G', 'D', 'E', 'F')); // OpenType definitions
  hb_set_add(dropTables, HB_TAG('D', 'S', 'I', 'G')); // digital signature
  hb_set_add(dropTables, HB_TAG('k', 'e', 'r', 'n')); // legacy kerning
  hb_set_add(dropTables, HB_TAG('n', 'a', 'm', 'e')); // font naming strings
  hb_set_add(dropTables, HB_TAG('g', 'a', 's', 'p')); // grid-fitting hints
  hb_set_add(dropTables,
             HB_TAG('h', 'd', 'm', 'x')); // horizontal device metrics
  hb_set_add(dropTables, HB_TAG('L', 'T', 'S', 'H')); // linear threshold
  hb_set_add(dropTables, HB_TAG('V', 'D', 'M', 'X')); // device metrics

  // preserve the cmap table unchanged for simple TrueType fonts —
  // PDF viewers use it to resolve character codes to glyph IDs.
  // for CID fonts, PDF viewers use /CIDToGIDMap instead, so the cmap
  // can be freely subset.
  if (preserveCmap) {
    hb_set_t *noSubsetTables =
        hb_subset_input_set(input, HB_SUBSET_SETS_NO_SUBSET_TABLE_TAG);
    hb_set_add(noSubsetTables, HB_TAG('c', 'm', 'a', 'p'));
  }

  // populate the glyph set to keep
  hb_set_t *glyphs = hb_subset_input_glyph_set(input);
  for (uint16_t gid : usedGlyphIds)
    hb_set_add(glyphs, gid);

  hb_face_t *subset = hb_subset_or_fail(face, input);
  hb_subset_input_destroy(input);
  hb_face_destroy(face);

  if (!subset)
    return false;

  hb_blob_t *result = hb_face_reference_blob(subset);
  unsigned int length = 0;
  const char *resultData = hb_blob_get_data(result, &length);

  if (length == 0 || !resultData) {
    hb_blob_destroy(result);
    hb_face_destroy(subset);
    return false;
  }

  output.assign(reinterpret_cast<const uint8_t *>(resultData),
                reinterpret_cast<const uint8_t *>(resultData) + length);

  hb_blob_destroy(result);
  hb_face_destroy(subset);
  return true;
}
