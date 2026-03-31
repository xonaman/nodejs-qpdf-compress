#include "font_subset.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <queue>

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

static int16_t readI16(const uint8_t *p) {
  return static_cast<int16_t>(readU16(p));
}

static void writeU16(uint8_t *p, uint16_t v) {
  p[0] = static_cast<uint8_t>(v >> 8);
  p[1] = static_cast<uint8_t>(v);
}

static void writeU32(uint8_t *p, uint32_t v) {
  p[0] = static_cast<uint8_t>(v >> 24);
  p[1] = static_cast<uint8_t>(v >> 16);
  p[2] = static_cast<uint8_t>(v >> 8);
  p[3] = static_cast<uint8_t>(v);
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

  // find a suitable subtable (prefer format 4 — the most common)
  const uint8_t *subtable = nullptr;
  for (uint16_t i = 0; i < numSubtables; ++i) {
    if (4 + i * 8 + 8 > cmapLen)
      break;
    const uint8_t *entry = cmap + 4 + i * 8;
    uint32_t subtableOffset = readU32(entry + 4);
    if (subtableOffset + 2 > cmapLen)
      continue;

    uint16_t format = readU16(cmap + subtableOffset);
    if (format == 4) {
      subtable = cmap + subtableOffset;
      break;
    }
  }

  // fallback: use first subtable
  if (!subtable && numSubtables > 0) {
    uint32_t subtableOffset = readU32(cmap + 4 + 4);
    if (subtableOffset + 2 <= cmapLen)
      subtable = cmap + subtableOffset;
  }

  if (!subtable)
    return glyphIds;

  uint16_t format = readU16(subtable);

  if (format == 4) {
    // format 4: segment mapping to delta values
    if (subtable + 14 > cmap + cmapLen)
      return glyphIds;

    uint16_t segCount = readU16(subtable + 6) / 2;
    const uint8_t *endCodes = subtable + 14;
    const uint8_t *startCodes =
        endCodes + segCount * 2 + 2; // +2 for reservedPad
    const uint8_t *idDeltas = startCodes + segCount * 2;
    const uint8_t *idRangeOffsets = idDeltas + segCount * 2;

    // bounds check
    if (idRangeOffsets + segCount * 2 > cmap + cmapLen)
      return glyphIds;

    for (uint16_t code : charCodes) {
      for (uint16_t seg = 0; seg < segCount; ++seg) {
        uint16_t endCode = readU16(endCodes + seg * 2);
        uint16_t startCode = readU16(startCodes + seg * 2);

        if (code < startCode || code > endCode)
          continue;

        uint16_t rangeOffset = readU16(idRangeOffsets + seg * 2);
        uint16_t delta = readU16(idDeltas + seg * 2);

        uint16_t glyphId;
        if (rangeOffset == 0) {
          glyphId = static_cast<uint16_t>((code + delta) & 0xFFFF);
        } else {
          const uint8_t *glyphIdAddr =
              idRangeOffsets + seg * 2 + rangeOffset +
              2 * static_cast<uint16_t>(code - startCode);
          if (glyphIdAddr + 2 > cmap + cmapLen)
            break;
          glyphId = readU16(glyphIdAddr);
          if (glyphId != 0)
            glyphId = static_cast<uint16_t>((glyphId + delta) & 0xFFFF);
        }

        if (glyphId != 0)
          glyphIds.insert(glyphId);
        break;
      }
    }
  } else if (format == 0) {
    // format 0: byte encoding table
    if (subtable + 6 + 256 > cmap + cmapLen)
      return glyphIds;
    for (uint16_t code : charCodes) {
      if (code < 256) {
        uint8_t gid = subtable[6 + code];
        if (gid != 0)
          glyphIds.insert(gid);
      }
    }
  }

  return glyphIds;
}

// ---------------------------------------------------------------------------
// Collect composite glyph dependencies
// ---------------------------------------------------------------------------

static void collectCompositeGlyphs(const uint8_t *glyfData, size_t glyfLen,
                                   const uint8_t *locaData, bool locaLong,
                                   uint16_t numGlyphs,
                                   std::set<uint16_t> &glyphIds) {
  std::queue<uint16_t> toProcess;
  for (uint16_t gid : glyphIds)
    toProcess.push(gid);

  while (!toProcess.empty()) {
    uint16_t gid = toProcess.front();
    toProcess.pop();

    if (gid >= numGlyphs)
      continue;

    uint32_t offset, nextOffset;
    if (locaLong) {
      offset = readU32(locaData + gid * 4);
      nextOffset = readU32(locaData + (gid + 1) * 4);
    } else {
      offset = static_cast<uint32_t>(readU16(locaData + gid * 2)) * 2;
      nextOffset = static_cast<uint32_t>(readU16(locaData + (gid + 1) * 2)) * 2;
    }

    if (offset >= nextOffset || offset >= glyfLen)
      continue;

    const uint8_t *glyph = glyfData + offset;
    size_t glyphLen = nextOffset - offset;
    if (glyphLen < 10)
      continue;

    int16_t numContours = readI16(glyph);
    if (numContours >= 0)
      continue; // simple glyph, no dependencies

    // composite glyph — parse component records
    size_t pos = 10; // skip header
    while (pos + 4 <= glyphLen) {
      uint16_t flags = readU16(glyph + pos);
      uint16_t componentGid = readU16(glyph + pos + 2);
      pos += 4;

      if (componentGid < numGlyphs &&
          glyphIds.find(componentGid) == glyphIds.end()) {
        glyphIds.insert(componentGid);
        toProcess.push(componentGid);
      }

      // skip arguments based on flags
      if (flags & 0x0001) // ARG_1_AND_2_ARE_WORDS
        pos += 4;
      else
        pos += 2;

      if (flags & 0x0008) // WE_HAVE_A_SCALE
        pos += 2;
      else if (flags & 0x0040) // WE_HAVE_AN_X_AND_Y_SCALE
        pos += 4;
      else if (flags & 0x0080) // WE_HAVE_A_TWO_BY_TWO
        pos += 8;

      if (!(flags & 0x0020)) // MORE_COMPONENTS
        break;
    }
  }
}

// ---------------------------------------------------------------------------
// TrueType font subsetting — keep only used glyphs
// ---------------------------------------------------------------------------

static uint32_t calcChecksum(const uint8_t *data, size_t length) {
  uint32_t sum = 0;
  size_t nLongs = (length + 3) / 4;
  for (size_t i = 0; i < nLongs; ++i) {
    uint32_t val = 0;
    for (size_t j = 0; j < 4; ++j) {
      size_t idx = i * 4 + j;
      val = (val << 8) | (idx < length ? data[idx] : 0);
    }
    sum += val;
  }
  return sum;
}

bool subsetTrueTypeFont(const uint8_t *data, size_t size,
                        const std::set<uint16_t> &usedGlyphIds,
                        std::vector<uint8_t> &output) {
  std::map<uint32_t, TableRecord> tables;
  if (!parseTables(data, size, tables))
    return false;

  // required tables
  auto headIt = tables.find(tag("head"));
  auto maxpIt = tables.find(tag("maxp"));
  auto locaIt = tables.find(tag("loca"));
  auto glyfIt = tables.find(tag("glyf"));

  if (headIt == tables.end() || maxpIt == tables.end() ||
      locaIt == tables.end() || glyfIt == tables.end())
    return false;

  const uint8_t *headData = data + headIt->second.offset;
  if (headIt->second.length < 54)
    return false;

  bool locaLong = readI16(headData + 50) == 1;

  const uint8_t *maxpData = data + maxpIt->second.offset;
  if (maxpIt->second.length < 6)
    return false;
  uint16_t numGlyphs = readU16(maxpData + 4);

  const uint8_t *locaData = data + locaIt->second.offset;
  const uint8_t *glyfData = data + glyfIt->second.offset;
  size_t glyfLen = glyfIt->second.length;

  // collect all needed glyphs including composite dependencies
  auto allGlyphs = usedGlyphIds;
  allGlyphs.insert(0); // always keep .notdef
  collectCompositeGlyphs(glyfData, glyfLen, locaData, locaLong, numGlyphs,
                         allGlyphs);

  // build new glyf table — map old glyph IDs to new glyph data
  // we preserve old glyph IDs (don't remap) to keep cmap valid —
  // instead we zero out unused glyph slots
  std::vector<uint8_t> newGlyf;
  std::vector<uint32_t> newLoca(numGlyphs + 1);

  for (uint16_t gid = 0; gid < numGlyphs; ++gid) {
    newLoca[gid] = static_cast<uint32_t>(newGlyf.size());

    if (allGlyphs.find(gid) == allGlyphs.end())
      continue; // empty glyph — loca points to same offset as next

    uint32_t offset, nextOffset;
    if (locaLong) {
      if ((gid + 1) * 4 + 4 > locaIt->second.length)
        continue;
      offset = readU32(locaData + gid * 4);
      nextOffset = readU32(locaData + (gid + 1) * 4);
    } else {
      if ((gid + 1) * 2 + 2 > locaIt->second.length)
        continue;
      offset = static_cast<uint32_t>(readU16(locaData + gid * 2)) * 2;
      nextOffset = static_cast<uint32_t>(readU16(locaData + (gid + 1) * 2)) * 2;
    }

    if (offset >= nextOffset || offset >= glyfLen)
      continue;

    uint32_t len =
        std::min(nextOffset - offset, static_cast<uint32_t>(glyfLen - offset));
    newGlyf.insert(newGlyf.end(), glyfData + offset, glyfData + offset + len);

    // pad to 2-byte boundary (for short loca) or 4-byte boundary (long loca)
    size_t align = locaLong ? 4 : 2;
    while (newGlyf.size() % align != 0)
      newGlyf.push_back(0);
  }
  newLoca[numGlyphs] = static_cast<uint32_t>(newGlyf.size());

  // build new loca table
  std::vector<uint8_t> newLocaData;
  if (locaLong) {
    newLocaData.resize((numGlyphs + 1) * 4);
    for (uint16_t i = 0; i <= numGlyphs; ++i)
      writeU32(newLocaData.data() + i * 4, newLoca[i]);
  } else {
    newLocaData.resize((numGlyphs + 1) * 2);
    for (uint16_t i = 0; i <= numGlyphs; ++i)
      writeU16(newLocaData.data() + i * 2,
               static_cast<uint16_t>(newLoca[i] / 2));
  }

  // subset hmtx: zero widths for unused glyphs
  auto hheaIt = tables.find(tag("hhea"));
  auto hmtxIt = tables.find(tag("hmtx"));
  std::vector<uint8_t> newHmtx;

  if (hheaIt != tables.end() && hmtxIt != tables.end() &&
      hheaIt->second.length >= 36) {
    uint16_t numHMetrics = readU16(data + hheaIt->second.offset + 34);
    const uint8_t *hmtxData = data + hmtxIt->second.offset;
    size_t hmtxLen = hmtxIt->second.length;

    newHmtx.assign(hmtxData, hmtxData + hmtxLen);

    for (uint16_t gid = 0; gid < numGlyphs; ++gid) {
      if (allGlyphs.find(gid) != allGlyphs.end())
        continue;

      if (gid < numHMetrics) {
        // long metric entry: advanceWidth(2) + lsb(2)
        size_t off = static_cast<size_t>(gid) * 4;
        if (off + 4 <= newHmtx.size()) {
          writeU16(newHmtx.data() + off, 0);
          writeU16(newHmtx.data() + off + 2, 0);
        }
      } else {
        // short metric: just lsb(2) after the long entries
        size_t off = static_cast<size_t>(numHMetrics) * 4 +
                     static_cast<size_t>(gid - numHMetrics) * 2;
        if (off + 2 <= newHmtx.size())
          writeU16(newHmtx.data() + off, 0);
      }
    }
  }

  // collect tables to write (all original tables, replacing glyf/loca/hmtx)
  struct OutputTable {
    uint32_t tag;
    std::vector<uint8_t> data;
  };
  std::vector<OutputTable> outTables;

  for (auto &[tableTag, rec] : tables) {
    OutputTable ot;
    ot.tag = tableTag;

    if (tableTag == tag("glyf")) {
      ot.data = newGlyf;
    } else if (tableTag == tag("loca")) {
      ot.data = newLocaData;
    } else if (tableTag == tag("hmtx") && !newHmtx.empty()) {
      ot.data = newHmtx;
    } else {
      ot.data.assign(data + rec.offset, data + rec.offset + rec.length);
    }

    outTables.push_back(std::move(ot));
  }

  // sort by tag (recommended for efficient access)
  std::sort(
      outTables.begin(), outTables.end(),
      [](const OutputTable &a, const OutputTable &b) { return a.tag < b.tag; });

  // calculate output size
  uint16_t numOutTables = static_cast<uint16_t>(outTables.size());
  size_t headerSize = 12 + numOutTables * 16;
  size_t totalSize = headerSize;
  for (auto &ot : outTables)
    totalSize += ((ot.data.size() + 3) / 4) * 4; // pad to 4 bytes

  output.resize(totalSize, 0);
  uint8_t *out = output.data();

  // write offset table
  writeU32(out, readU32(data)); // sfVersion (same as original)
  writeU16(out + 4, numOutTables);

  // searchRange, entrySelector, rangeShift
  uint16_t searchRange = 1;
  uint16_t entrySelector = 0;
  while (searchRange * 2 <= numOutTables) {
    searchRange *= 2;
    ++entrySelector;
  }
  searchRange *= 16;
  writeU16(out + 6, searchRange);
  writeU16(out + 8, entrySelector);
  writeU16(out + 10, numOutTables * 16 - searchRange);

  // write table records and data
  size_t dataOffset = headerSize;
  for (uint16_t i = 0; i < numOutTables; ++i) {
    auto &ot = outTables[i];
    uint8_t *rec = out + 12 + i * 16;

    writeU32(rec, ot.tag);
    writeU32(rec + 4, calcChecksum(ot.data.data(), ot.data.size()));
    writeU32(rec + 8, static_cast<uint32_t>(dataOffset));
    writeU32(rec + 12, static_cast<uint32_t>(ot.data.size()));

    memcpy(out + dataOffset, ot.data.data(), ot.data.size());
    dataOffset += ((ot.data.size() + 3) / 4) * 4;
  }

  return true;
}
