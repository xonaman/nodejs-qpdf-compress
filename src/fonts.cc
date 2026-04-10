#include "fonts.h"
#include "encoding_tables.h"
#include "font_subset.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <qpdf/QPDFObjectHandle.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>
#include <qpdf/QPDFPageObjectHelper.hh>

// ---------------------------------------------------------------------------
// Combined parser callback: collects font names referenced by Tf operators
// AND character codes used by each font in a single pass
// ---------------------------------------------------------------------------

class FontUsageCollector : public QPDFObjectHandle::ParserCallbacks {
public:
  std::set<std::string> usedFonts;
  std::map<std::string, std::set<uint16_t>> fontCodes;
  std::set<std::string> cidFonts; // set before parsing

  void handleObject(QPDFObjectHandle obj) override {
    if (obj.isOperator()) {
      std::string op = obj.getOperatorValue();
      if (op == "Tf" && operands.size() >= 2) {
        auto nameObj = operands[operands.size() - 2];
        if (nameObj.isName()) {
          currentFont = nameObj.getName();
          usedFonts.insert(currentFont);
        }
      }
      if (!currentFont.empty()) {
        bool isCID = cidFonts.count(currentFont) > 0;
        if (op == "Tj" || op == "'" || op == "\"") {
          if (!operands.empty() && operands.back().isString())
            collectFromString(operands.back(), isCID);
        } else if (op == "TJ") {
          if (!operands.empty() && operands.back().isArray()) {
            auto arr = operands.back();
            for (int i = 0; i < arr.getArrayNItems(); ++i) {
              auto item = arr.getArrayItem(i);
              if (item.isString())
                collectFromString(item, isCID);
            }
          }
        }
      }
      operands.clear();
    } else {
      operands.push_back(obj);
    }
  }
  void handleEOF() override {}

private:
  std::string currentFont;
  std::vector<QPDFObjectHandle> operands;

  void collectFromString(QPDFObjectHandle strObj, bool isCID) {
    std::string raw = strObj.getStringValue();
    if (isCID) {
      for (size_t i = 0; i + 1 < raw.size(); i += 2) {
        uint16_t code = (static_cast<uint8_t>(raw[i]) << 8) |
                        static_cast<uint8_t>(raw[i + 1]);
        fontCodes[currentFont].insert(code);
      }
    } else {
      for (unsigned char c : raw)
        fontCodes[currentFont].insert(c);
    }
  }
};

// helper: run FontUsageCollector on a stream, swallowing parse errors
static void collectFontUsageFromStream(QPDFObjectHandle stream,
                                       FontUsageCollector &collector) {
  try {
    QPDFObjectHandle::parseContentStream(stream, &collector);
  } catch (...) {
  }
}

// ---------------------------------------------------------------------------
// /ToUnicode CMap parser — converts character codes to Unicode using the
// font's /ToUnicode stream. handles beginbfchar, beginbfrange (scalar and
// array forms). this is the most reliable way to map codes to Unicode.
// ---------------------------------------------------------------------------

static std::set<uint16_t> parseToUnicode(QPDFObjectHandle toUnicodeStream,
                                         const std::set<uint16_t> &charCodes) {
  std::set<uint16_t> unicodeCodes;

  try {
    auto data = toUnicodeStream.getStreamData(qpdf_dl_all);
    std::string cmap(reinterpret_cast<const char *>(data->getBuffer()),
                     data->getSize());

    // helper: find <hex> value at/after pos, return parsed value and position
    // after closing >
    auto findHexValue = [&](size_t pos,
                            size_t end) -> std::pair<uint16_t, size_t> {
      size_t start = cmap.find('<', pos);
      if (start == std::string::npos || start >= end)
        return {0, std::string::npos};
      size_t stop = cmap.find('>', start);
      if (stop == std::string::npos || stop >= end)
        return {0, std::string::npos};
      std::string hex = cmap.substr(start + 1, stop - start - 1);
      if (hex.empty())
        return {0, stop + 1};
      return {static_cast<uint16_t>(std::stoul(hex, nullptr, 16)), stop + 1};
    };

    // process beginbfchar sections: <srcCode> <dstUnicode>
    size_t pos = 0;
    while (true) {
      size_t start = cmap.find("beginbfchar", pos);
      if (start == std::string::npos)
        break;
      start += 11;
      size_t end = cmap.find("endbfchar", start);
      if (end == std::string::npos)
        break;

      size_t p = start;
      while (p < end) {
        auto [src, after1] = findHexValue(p, end);
        if (after1 == std::string::npos)
          break;
        auto [dst, after2] = findHexValue(after1, end);
        if (after2 == std::string::npos)
          break;
        if (charCodes.count(src))
          unicodeCodes.insert(dst);
        p = after2;
      }
      pos = end + 9;
    }

    // process beginbfrange sections: <lo> <hi> <dstStart> or <lo> <hi> [...]
    pos = 0;
    while (true) {
      size_t start = cmap.find("beginbfrange", pos);
      if (start == std::string::npos)
        break;
      start += 12;
      size_t end = cmap.find("endbfrange", start);
      if (end == std::string::npos)
        break;

      size_t p = start;
      while (p < end) {
        auto [lo, after1] = findHexValue(p, end);
        if (after1 == std::string::npos)
          break;
        auto [hi, after2] = findHexValue(after1, end);
        if (after2 == std::string::npos)
          break;

        // check for array form vs scalar form
        size_t next = after2;
        while (next < end &&
               std::isspace(static_cast<unsigned char>(cmap[next])))
          ++next;

        if (next < end && cmap[next] == '[') {
          // array form: <lo> <hi> [<v1> <v2> ...]
          size_t arrEnd = cmap.find(']', next);
          if (arrEnd == std::string::npos || arrEnd >= end)
            break;
          uint16_t code = lo;
          size_t ap = next + 1;
          while (ap < arrEnd && code <= hi) {
            auto [val, afterVal] = findHexValue(ap, arrEnd);
            if (afterVal == std::string::npos)
              break;
            if (charCodes.count(code))
              unicodeCodes.insert(val);
            ++code;
            ap = afterVal;
          }
          p = arrEnd + 1;
        } else {
          // scalar form: <lo> <hi> <dstStart>
          auto [dstStart, after3] = findHexValue(after2, end);
          if (after3 == std::string::npos)
            break;
          for (uint16_t c = lo; c <= hi; ++c) {
            if (charCodes.count(c))
              unicodeCodes.insert(static_cast<uint16_t>(dstStart + (c - lo)));
          }
          p = after3;
        }
      }
      pos = end + 10;
    }
  } catch (...) {
    // parsing failed — return what we have so far
  }

  return unicodeCodes;
}

// ---------------------------------------------------------------------------
// /Encoding /Differences parser — extract glyph names for character codes
// that have been remapped via /Differences entries.
// format: [code1 /name1 /name2 ... code2 /name3 ...]
// integers set current position; names are assigned sequentially.
// ---------------------------------------------------------------------------

static std::vector<std::string>
getGlyphNamesFromEncoding(const std::set<uint16_t> &codes,
                          QPDFObjectHandle fontObj) {
  std::vector<std::string> names;

  auto encoding = fontObj.getKey("/Encoding");
  if (!encoding.isDictionary())
    return names;

  auto diffs = encoding.getKey("/Differences");
  if (!diffs.isArray())
    return names;

  // parse /Differences: integers set position, names are glyph names
  std::map<uint16_t, std::string> codeToName;
  int currentCode = 0;
  for (int i = 0; i < diffs.getArrayNItems(); ++i) {
    auto item = diffs.getArrayItem(i);
    if (item.isInteger()) {
      currentCode = static_cast<int>(item.getIntValue());
    } else if (item.isName()) {
      std::string name = item.getName();
      // strip leading / from PDF name
      if (!name.empty() && name[0] == '/')
        name = name.substr(1);
      codeToName[static_cast<uint16_t>(currentCode)] = name;
      ++currentCode;
    }
  }

  // return glyph names for used codes that have /Differences entries
  for (uint16_t code : codes) {
    auto it = codeToName.find(code);
    if (it != codeToName.end())
      names.push_back(it->second);
  }

  return names;
}

// ---------------------------------------------------------------------------
// Encoding helpers — convert encoding-specific byte codes to Unicode
// for correct cmap lookups during font subsetting
// ---------------------------------------------------------------------------

// convert encoding-specific character codes to Unicode for cmap lookup
static std::set<uint16_t> convertCodesToUnicode(const std::set<uint16_t> &codes,
                                                QPDFObjectHandle fontObj) {
  auto encoding = fontObj.getKey("/Encoding");

  bool isWinAnsi = false;
  bool isMacRoman = false;
  if (encoding.isName()) {
    if (encoding.getName() == "/WinAnsiEncoding")
      isWinAnsi = true;
    else if (encoding.getName() == "/MacRomanEncoding")
      isMacRoman = true;
  } else if (encoding.isDictionary()) {
    auto baseEnc = encoding.getKey("/BaseEncoding");
    if (baseEnc.isName()) {
      if (baseEnc.getName() == "/WinAnsiEncoding")
        isWinAnsi = true;
      else if (baseEnc.getName() == "/MacRomanEncoding")
        isMacRoman = true;
    }
  }

  if (!isWinAnsi && !isMacRoman)
    return codes;

  std::set<uint16_t> unicodeCodes;
  for (uint16_t code : codes) {
    if (isWinAnsi)
      unicodeCodes.insert(winAnsiToUnicode(static_cast<uint8_t>(code)));
    else
      unicodeCodes.insert(macRomanToUnicode(static_cast<uint8_t>(code)));
  }
  return unicodeCodes;
}

// ---------------------------------------------------------------------------
// Combined font optimization — remove unused fonts AND subset remaining
// fonts in a single page walk
// ---------------------------------------------------------------------------

void optimizeFonts(QPDF &qpdf) {
  // single page walk: collect used font names + character codes per font
  std::map<QPDFObjGen, std::set<uint16_t>> fontUsedCodes;

  for (auto &page : QPDFPageDocumentHelper(qpdf).getAllPages()) {
    auto pageObj = page.getObjectHandle();
    auto resources = pageObj.getKey("/Resources");
    if (!resources.isDictionary())
      continue;
    auto fonts = resources.getKey("/Font");
    if (!fonts.isDictionary())
      continue;

    // build list of (stream, fontDict) pairs to scan
    struct StreamFonts {
      QPDFObjectHandle stream;
      QPDFObjectHandle fontDict;
    };
    std::vector<StreamFonts> scanTargets;

    try {
      auto contents = pageObj.getKey("/Contents");
      scanTargets.push_back({contents, fonts});
    } catch (...) {
      continue;
    }

    auto xobjects = resources.getKey("/XObject");
    if (xobjects.isDictionary()) {
      for (auto &xobjKey : xobjects.getKeys()) {
        auto xobj = xobjects.getKey(xobjKey);
        if (!xobj.isStream())
          continue;
        auto xobjDict = xobj.getDict();
        auto xobjSubtype = xobjDict.getKey("/Subtype");
        if (!xobjSubtype.isName() || xobjSubtype.getName() != "/Form")
          continue;
        auto xobjRes = xobjDict.getKey("/Resources");
        if (xobjRes.isDictionary() && xobjRes.getKey("/Font").isDictionary())
          scanTargets.push_back({xobj, xobjRes.getKey("/Font")});
        else
          scanTargets.push_back({xobj, fonts});
      }
    }

    // determine which fonts are CID for the collector
    FontUsageCollector collector;
    for (auto &target : scanTargets) {
      for (auto &key : target.fontDict.getKeys()) {
        auto fontObj = target.fontDict.getKey(key);
        if (!fontObj.isDictionary())
          continue;
        auto subtypeKey = fontObj.getKey("/Subtype");
        if (subtypeKey.isName() && subtypeKey.getName() == "/Type0")
          collector.cidFonts.insert(key);
      }
    }

    // parse all content streams for this page in one pass per stream
    for (auto &target : scanTargets)
      collectFontUsageFromStream(target.stream, collector);

    // remove fonts not referenced by any Tf operator
    auto allFontKeys = fonts.getKeys();
    for (auto &fontKey : allFontKeys) {
      if (collector.usedFonts.find(fontKey) == collector.usedFonts.end())
        fonts.removeKey(fontKey);
    }

    // map per-name codes to per-ObjGen codes for subsetting
    for (auto &target : scanTargets) {
      for (auto &key : target.fontDict.getKeys()) {
        auto it = collector.fontCodes.find(key);
        if (it == collector.fontCodes.end())
          continue;
        auto fontObj = target.fontDict.getKey(key);
        if (!fontObj.isDictionary())
          continue;
        auto og = fontObj.getObjGen();
        fontUsedCodes[og].insert(it->second.begin(), it->second.end());
      }
    }
  }

  // for each font with a /Widths array, zero out widths for unused glyphs
  // and truncate trailing zeros
  for (auto &[og, usedCodes] : fontUsedCodes) {
    auto fontObj = qpdf.getObjectByObjGen(og);
    if (!fontObj.isDictionary())
      continue;

    auto subtype = fontObj.getKey("/Subtype");
    if (!subtype.isName())
      continue;

    // only handle simple fonts with /Widths arrays (TrueType, Type1)
    if (subtype.getName() != "/TrueType" && subtype.getName() != "/Type1")
      continue;

    auto widths = fontObj.getKey("/Widths");
    auto firstCharObj = fontObj.getKey("/FirstChar");
    if (!widths.isArray() || !firstCharObj.isInteger())
      continue;

    int firstChar = static_cast<int>(firstCharObj.getIntValue());
    int widthCount = widths.getArrayNItems();

    // zero out widths for unused character codes
    bool modified = false;
    for (int i = 0; i < widthCount; ++i) {
      int charCode = firstChar + i;
      if (usedCodes.find(static_cast<uint16_t>(charCode)) == usedCodes.end()) {
        auto w = widths.getArrayItem(i);
        if (w.isInteger() && w.getIntValue() != 0) {
          widths.setArrayItem(i, QPDFObjectHandle::newInteger(0));
          modified = true;
        } else if (w.isReal() && w.getNumericValue() != 0.0) {
          widths.setArrayItem(i, QPDFObjectHandle::newInteger(0));
          modified = true;
        }
      }
    }

    if (!modified)
      continue;

    // trim trailing zero-width entries and adjust /LastChar
    int lastUsed = widthCount - 1;
    while (lastUsed >= 0) {
      auto w = widths.getArrayItem(lastUsed);
      if (w.isInteger() && w.getIntValue() == 0)
        --lastUsed;
      else
        break;
    }

    if (lastUsed < widthCount - 1) {
      // rebuild the widths array with only the needed entries
      auto newWidths = QPDFObjectHandle::newArray();
      for (int i = 0; i <= lastUsed; ++i)
        newWidths.appendItem(widths.getArrayItem(i));

      fontObj.replaceKey("/Widths", newWidths);
      fontObj.replaceKey("/LastChar",
                         QPDFObjectHandle::newInteger(firstChar + lastUsed));
    }
  }

  // true font subsetting: strip unused glyph outlines from embedded fonts
  std::set<QPDFObjGen> processedFonts;
  std::set<QPDFObjGen> processedFontFiles;
  for (auto &[og, usedCodes] : fontUsedCodes) {
    if (processedFonts.count(og))
      continue;
    processedFonts.insert(og);

    auto fontObj = qpdf.getObjectByObjGen(og);
    if (!fontObj.isDictionary())
      continue;

    auto subtype = fontObj.getKey("/Subtype");
    if (!subtype.isName())
      continue;
    // Type0 composite fonts — subset CIDFontType2 and CIDFontType0
    // descendants
    if (subtype.getName() == "/Type0") {
      auto descendants = fontObj.getKey("/DescendantFonts");
      if (!descendants.isArray() || descendants.getArrayNItems() < 1)
        continue;

      auto cidFont = descendants.getArrayItem(0);
      if (!cidFont.isDictionary())
        continue;

      auto cidSubtype = cidFont.getKey("/Subtype");
      if (!cidSubtype.isName())
        continue;

      bool isCIDFontType2 = cidSubtype.getName() == "/CIDFontType2";
      bool isCIDFontType0 = cidSubtype.getName() == "/CIDFontType0";
      if (!isCIDFontType2 && !isCIDFontType0)
        continue;

      auto cidDescriptor = cidFont.getKey("/FontDescriptor");
      if (!cidDescriptor.isDictionary())
        continue;

      // determine which font file key to use:
      // CIDFontType2 → /FontFile2 (TrueType)
      // CIDFontType0 → /FontFile3 (CFF)
      std::string fontFileKey;
      if (isCIDFontType2 && cidDescriptor.hasKey("/FontFile2"))
        fontFileKey = "/FontFile2";
      else if (isCIDFontType0 && cidDescriptor.hasKey("/FontFile3"))
        fontFileKey = "/FontFile3";
      else
        continue;

      auto fontFile = cidDescriptor.getKey(fontFileKey);
      if (!fontFile.isStream())
        continue;

      // skip if this font file was already subset by another font object
      auto ffOg = fontFile.getObjGen();
      if (processedFontFiles.count(ffOg))
        continue;
      processedFontFiles.insert(ffOg);

      try {
        auto fontData = fontFile.getStreamData(qpdf_dl_all);
        const uint8_t *rawData = fontData->getBuffer();
        size_t rawSize = fontData->getSize();

        std::set<uint16_t> glyphIds;

        auto cidToGid = cidFont.getKey("/CIDToGIDMap");
        if (cidToGid.isName() && cidToGid.getName() == "/Identity") {
          // Identity mapping — CID codes map directly to glyph IDs
          glyphIds = usedCodes;
        } else if (cidToGid.isStream()) {
          // explicit CIDToGIDMap stream — array of 2-byte big-endian GIDs
          // indexed by CID
          auto mapData = cidToGid.getStreamData(qpdf_dl_all);
          const uint8_t *mapBuf = mapData->getBuffer();
          size_t mapSize = mapData->getSize();
          for (uint16_t cid : usedCodes) {
            size_t offset = static_cast<size_t>(cid) * 2;
            if (offset + 2 <= mapSize) {
              uint16_t gid = static_cast<uint16_t>((mapBuf[offset] << 8) |
                                                   mapBuf[offset + 1]);
              if (gid != 0)
                glyphIds.insert(gid);
            }
          }
        } else if (isCIDFontType0) {
          // CIDFontType0 without explicit mapping — CID = GID
          glyphIds = usedCodes;
        } else {
          continue;
        }

        glyphIds.insert(0); // always keep .notdef

        std::vector<uint8_t> subsetResult;
        if (!subsetFont(rawData, rawSize, glyphIds, subsetResult, false))
          continue;

        if (subsetResult.size() >= rawSize)
          continue;

        std::string fontStr(reinterpret_cast<char *>(subsetResult.data()),
                            subsetResult.size());
        fontFile.replaceStreamData(fontStr, QPDFObjectHandle::newNull(),
                                   QPDFObjectHandle::newNull());
      } catch (...) {
        continue;
      }
      continue;
    }

    // simple TrueType fonts
    if (subtype.getName() != "/TrueType")
      continue;

    auto descriptor = fontObj.getKey("/FontDescriptor");
    if (!descriptor.isDictionary() || !descriptor.hasKey("/FontFile2"))
      continue;

    auto fontFile = descriptor.getKey("/FontFile2");
    if (!fontFile.isStream())
      continue;

    // skip if this FontFile2 was already subset by another font object
    auto ffOg = fontFile.getObjGen();
    if (processedFontFiles.count(ffOg))
      continue;
    processedFontFiles.insert(ffOg);

    try {
      auto fontData = fontFile.getStreamData(qpdf_dl_all);
      const uint8_t *ttfData = fontData->getBuffer();
      size_t ttfSize = fontData->getSize();

      std::set<uint16_t> glyphIds;

      // strategy 1: /ToUnicode CMap — most reliable Unicode mapping
      auto toUnicode = fontObj.getKey("/ToUnicode");
      if (toUnicode.isStream()) {
        auto tuCodes = parseToUnicode(toUnicode, usedCodes);
        if (!tuCodes.empty()) {
          auto ids = mapCodesToGlyphIds(ttfData, ttfSize, tuCodes);
          glyphIds.insert(ids.begin(), ids.end());
        }
      }

      // strategy 2: /Encoding /Differences — glyph name lookup via post table
      auto diffNames = getGlyphNamesFromEncoding(usedCodes, fontObj);
      if (!diffNames.empty()) {
        auto nameIds = mapGlyphNamesToGlyphIds(ttfData, ttfSize, diffNames);
        glyphIds.insert(nameIds.begin(), nameIds.end());
      }

      // strategy 3: base encoding conversion (WinAnsi, MacRoman) → cmap
      auto unicodeCodes = convertCodesToUnicode(usedCodes, fontObj);
      auto encIds = mapCodesToGlyphIds(ttfData, ttfSize, unicodeCodes);
      glyphIds.insert(encIds.begin(), encIds.end());

      // strategy 4: raw character codes as fallback for fonts with custom
      // encodings where the cmap may be indexed by raw byte values
      if (unicodeCodes != usedCodes) {
        auto rawGlyphs = mapCodesToGlyphIds(ttfData, ttfSize, usedCodes);
        glyphIds.insert(rawGlyphs.begin(), rawGlyphs.end());
      }

      // strategy 5: standard glyph names via post table — catches fonts
      // where glyphs have no cmap entry but are accessible by name
      {
        std::vector<std::string> uniNames;
        // collect all Unicode code points from previous strategies
        std::set<uint16_t> allUnicode;
        allUnicode.insert(unicodeCodes.begin(), unicodeCodes.end());
        allUnicode.insert(usedCodes.begin(), usedCodes.end());
        for (uint16_t u : allUnicode) {
          if (u > 0x7F) {
            // standard "uniXXXX" glyph name convention
            char buf[8];
            snprintf(buf, sizeof(buf), "uni%04X", u);
            uniNames.emplace_back(buf);
          }
        }
        if (!uniNames.empty()) {
          auto nameIds = mapGlyphNamesToGlyphIds(ttfData, ttfSize, uniNames);
          glyphIds.insert(nameIds.begin(), nameIds.end());
        }
      }

      // safety check: if we found fewer glyph IDs than used character codes,
      // some characters couldn't be mapped — skip subsetting to avoid
      // dropping visible glyphs. subtract 1 for .notdef (glyph 0).
      if (glyphIds.size() - 1 < usedCodes.size())
        continue;

      std::vector<uint8_t> subsetResult;
      if (!subsetFont(ttfData, ttfSize, glyphIds, subsetResult))
        continue;

      // only replace if the subset is smaller
      if (subsetResult.size() >= ttfSize)
        continue;

      std::string fontStr(reinterpret_cast<char *>(subsetResult.data()),
                          subsetResult.size());
      fontFile.replaceStreamData(fontStr, QPDFObjectHandle::newNull(),
                                 QPDFObjectHandle::newNull());
    } catch (...) {
      continue;
    }
  }

  // optimize CID font /W arrays — rebuild with only used CID entries
  for (auto &[og, usedCodes] : fontUsedCodes) {
    auto fontObj = qpdf.getObjectByObjGen(og);
    if (!fontObj.isDictionary())
      continue;

    auto subtype = fontObj.getKey("/Subtype");
    if (!subtype.isName() || subtype.getName() != "/Type0")
      continue;

    auto descendants = fontObj.getKey("/DescendantFonts");
    if (!descendants.isArray() || descendants.getArrayNItems() < 1)
      continue;

    auto cidFont = descendants.getArrayItem(0);
    if (!cidFont.isDictionary())
      continue;

    auto w = cidFont.getKey("/W");
    if (!w.isArray() || w.getArrayNItems() == 0)
      continue;

    // parse /W into CID → width value map
    std::map<int, QPDFObjectHandle> cidWidths;
    int n = w.getArrayNItems();
    int i = 0;
    while (i < n) {
      auto first = w.getArrayItem(i);
      if (!first.isInteger()) {
        ++i;
        continue;
      }
      int cidStart = static_cast<int>(first.getIntValue());
      ++i;
      if (i >= n)
        break;

      auto second = w.getArrayItem(i);
      if (second.isArray()) {
        // format: cidStart [w1 w2 w3 ...]
        for (int j = 0; j < second.getArrayNItems(); ++j)
          cidWidths[cidStart + j] = second.getArrayItem(j);
        ++i;
      } else if (second.isInteger()) {
        // format: cidStart cidEnd sameWidth
        int cidEnd = static_cast<int>(second.getIntValue());
        ++i;
        if (i >= n)
          break;
        auto width = w.getArrayItem(i);
        for (int cid = cidStart; cid <= cidEnd; ++cid)
          cidWidths[cid] = width;
        ++i;
      } else {
        ++i;
      }
    }

    // rebuild /W with only used CIDs grouped by consecutive runs
    std::vector<int> sortedUsed;
    for (uint16_t c : usedCodes) {
      if (cidWidths.count(static_cast<int>(c)))
        sortedUsed.push_back(static_cast<int>(c));
    }
    std::sort(sortedUsed.begin(), sortedUsed.end());

    auto newW = QPDFObjectHandle::newArray();
    size_t idx = 0;
    while (idx < sortedUsed.size()) {
      int start = sortedUsed[idx];
      auto widths = QPDFObjectHandle::newArray();
      widths.appendItem(cidWidths[start]);
      ++idx;
      while (idx < sortedUsed.size() &&
             sortedUsed[idx] == sortedUsed[idx - 1] + 1) {
        widths.appendItem(cidWidths[sortedUsed[idx]]);
        ++idx;
      }
      newW.appendItem(QPDFObjectHandle::newInteger(start));
      newW.appendItem(widths);
    }

    // only replace if the new /W has fewer entries
    if (newW.getArrayNItems() < w.getArrayNItems())
      cidFont.replaceKey("/W", newW);
  }
}
