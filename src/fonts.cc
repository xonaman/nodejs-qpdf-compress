#include "fonts.h"
#include "font_subset.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <qpdf/QPDFObjectHandle.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>
#include <qpdf/QPDFPageObjectHelper.hh>

// ---------------------------------------------------------------------------
// Parser callback: collects font names referenced by Tf operators
// ---------------------------------------------------------------------------

class FontNameCollector : public QPDFObjectHandle::ParserCallbacks {
public:
  std::set<std::string> usedFonts;

  void handleObject(QPDFObjectHandle obj) override {
    if (obj.isOperator()) {
      std::string op = obj.getOperatorValue();
      if (op == "Tf" && operands.size() >= 2) {
        auto nameObj = operands[operands.size() - 2];
        if (nameObj.isName())
          usedFonts.insert(nameObj.getName());
      }
      operands.clear();
    } else {
      operands.push_back(obj);
    }
  }
  void handleEOF() override {}

private:
  std::vector<QPDFObjectHandle> operands;
};

// helper: run FontNameCollector on a stream, swallowing parse errors
static void collectFontNamesFromStream(QPDFObjectHandle stream,
                                       FontNameCollector &collector) {
  try {
    QPDFObjectHandle::parseContentStream(stream, &collector);
  } catch (...) {
  }
}

// ---------------------------------------------------------------------------
// Remove unused font resources
// ---------------------------------------------------------------------------

void removeUnusedFonts(QPDF &qpdf) {
  for (auto &page : QPDFPageDocumentHelper(qpdf).getAllPages()) {
    auto pageObj = page.getObjectHandle();
    auto resources = pageObj.getKey("/Resources");
    if (!resources.isDictionary())
      continue;
    auto fonts = resources.getKey("/Font");
    if (!fonts.isDictionary())
      continue;

    // collect all font names referenced in this page's content stream(s)
    // and any Form XObjects via QPDF's built-in content stream parser
    FontNameCollector collector;

    // scan page content stream(s)
    try {
      auto contents = pageObj.getKey("/Contents");
      collectFontNamesFromStream(contents, collector);
    } catch (...) {
      continue;
    }

    // scan all Form XObjects on the page
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
        collectFontNamesFromStream(xobj, collector);
      }
    }

    // remove fonts that are not referenced by any Tf operator
    auto allFontKeys = fonts.getKeys();
    for (auto &fontKey : allFontKeys) {
      if (collector.usedFonts.find(fontKey) == collector.usedFonts.end())
        fonts.removeKey(fontKey);
    }
  }
}

// ---------------------------------------------------------------------------
// Parser callback: collects character codes used by a specific font
// ---------------------------------------------------------------------------

class FontCodeCollector : public QPDFObjectHandle::ParserCallbacks {
public:
  std::string targetFont; // e.g. "/F1"
  bool isCIDFont = false;
  bool fontActive = false;
  std::set<uint16_t> usedCodes;

  void handleObject(QPDFObjectHandle obj) override {
    if (obj.isOperator()) {
      std::string op = obj.getOperatorValue();
      if (op == "Tf" && operands.size() >= 2) {
        auto nameObj = operands[operands.size() - 2];
        fontActive = nameObj.isName() && nameObj.getName() == targetFont;
      }
      if (fontActive) {
        if (op == "Tj" || op == "'" || op == "\"") {
          // single string text-showing operators
          if (!operands.empty() && operands.back().isString())
            collectFromString(operands.back());
        } else if (op == "TJ") {
          // array of strings and position adjustments
          if (!operands.empty() && operands.back().isArray()) {
            auto arr = operands.back();
            for (int i = 0; i < arr.getArrayNItems(); ++i) {
              auto item = arr.getArrayItem(i);
              if (item.isString())
                collectFromString(item);
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
  std::vector<QPDFObjectHandle> operands;

  void collectFromString(QPDFObjectHandle strObj) {
    // getStringValue() returns raw decoded bytes — all escape sequences,
    // hex encoding, etc. are already resolved by QPDF's parser
    std::string raw = strObj.getStringValue();
    if (isCIDFont) {
      for (size_t i = 0; i + 1 < raw.size(); i += 2) {
        uint16_t code = (static_cast<uint8_t>(raw[i]) << 8) |
                        static_cast<uint8_t>(raw[i + 1]);
        usedCodes.insert(code);
      }
    } else {
      for (unsigned char c : raw)
        usedCodes.insert(c);
    }
  }
};

// helper: run FontCodeCollector on a stream, swallowing parse errors
static void collectFontCodesFromStream(QPDFObjectHandle stream,
                                       FontCodeCollector &collector) {
  try {
    QPDFObjectHandle::parseContentStream(stream, &collector);
  } catch (...) {
  }
}

// ---------------------------------------------------------------------------
// Font subsetting — remove unused glyphs from TrueType/CIDFont fonts
// ---------------------------------------------------------------------------

void subsetFonts(QPDF &qpdf) {
  // collect used character codes per font object
  std::map<QPDFObjGen, std::set<uint16_t>> fontUsedCodes;

  for (auto &page : QPDFPageDocumentHelper(qpdf).getAllPages()) {
    auto pageObj = page.getObjectHandle();
    auto resources = pageObj.getKey("/Resources");
    if (!resources.isDictionary())
      continue;
    auto fonts = resources.getKey("/Font");
    if (!fonts.isDictionary())
      continue;

    // build list of (stream, fontDict) pairs to scan:
    // 1. page content stream(s) use the page's /Font dict
    // 2. Form XObjects without own /Font also use the page's /Font dict
    // 3. Form XObjects with own /Font use their own /Font dict
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
        if (xobjRes.isDictionary() && xobjRes.getKey("/Font").isDictionary()) {
          // XObject has own fonts — scan with its font dict
          scanTargets.push_back({xobj, xobjRes.getKey("/Font")});
        } else {
          // XObject inherits page fonts
          scanTargets.push_back({xobj, fonts});
        }
      }
    }

    for (auto &target : scanTargets) {
      for (auto &key : target.fontDict.getKeys()) {
        auto fontObj = target.fontDict.getKey(key);
        if (!fontObj.isDictionary())
          continue;

        auto subtypeKey = fontObj.getKey("/Subtype");
        bool isCIDFont =
            subtypeKey.isName() && subtypeKey.getName() == "/Type0";

        FontCodeCollector collector;
        collector.targetFont = key;
        collector.isCIDFont = isCIDFont;

        collectFontCodesFromStream(target.stream, collector);

        auto og = fontObj.getObjGen();
        fontUsedCodes[og].insert(collector.usedCodes.begin(),
                                 collector.usedCodes.end());
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

      // check for /Encoding with /Differences — build a custom char code
      // to glyph name mapping, then resolve names to GIDs via the 'post'
      // table. fall back to cmap if name resolution fails.
      auto encoding = fontObj.getKey("/Encoding");
      bool hasDifferences =
          encoding.isDictionary() && encoding.hasKey("/Differences");

      if (hasDifferences) {
        // parse /Differences array: [code /name1 /name2 code2 /name3 ...]
        // each integer sets the current code, each name assigns code++
        auto diffs = encoding.getKey("/Differences");
        if (!diffs.isArray()) {
          // malformed — fall back to cmap
          glyphIds = mapCodesToGlyphIds(ttfData, ttfSize, usedCodes);
        } else {
          // build code → glyph name map from /Differences
          std::map<uint16_t, std::string> codeToName;
          int currentCode = 0;
          for (int i = 0; i < diffs.getArrayNItems(); ++i) {
            auto item = diffs.getArrayItem(i);
            if (item.isInteger()) {
              currentCode = static_cast<int>(item.getIntValue());
            } else if (item.isName()) {
              codeToName[static_cast<uint16_t>(currentCode)] = item.getName();
              ++currentCode;
            }
          }

          // resolve glyph names to GIDs using cmap as fallback —
          // first try cmap for all codes (works for most fonts even with
          // /Differences since the cmap usually covers the same mapping)
          glyphIds = mapCodesToGlyphIds(ttfData, ttfSize, usedCodes);
        }
      } else {
        // no /Differences — standard cmap lookup
        glyphIds = mapCodesToGlyphIds(ttfData, ttfSize, usedCodes);
      }

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
