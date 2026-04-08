#include "fonts.h"
#include "font_subset.h"

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
        } else if (w.isReal() && std::stod(w.getRealValue()) != 0.0) {
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

  // true font subsetting: strip unused glyph outlines from TrueType fonts
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
    // Type0 composite fonts — subset CIDFontType2 descendants
    if (subtype.getName() == "/Type0") {
      auto descendants = fontObj.getKey("/DescendantFonts");
      if (!descendants.isArray() || descendants.getArrayNItems() < 1)
        continue;

      auto cidFont = descendants.getArrayItem(0);
      if (!cidFont.isDictionary())
        continue;

      auto cidSubtype = cidFont.getKey("/Subtype");
      if (!cidSubtype.isName() || cidSubtype.getName() != "/CIDFontType2")
        continue;

      // only handle Identity CIDToGIDMap — CID values map directly to
      // glyph IDs, so the codes collected by FontCodeCollector are already
      // the glyph IDs we need for HarfBuzz
      auto cidToGid = cidFont.getKey("/CIDToGIDMap");
      if (!cidToGid.isName() || cidToGid.getName() != "/Identity")
        continue;

      auto cidDescriptor = cidFont.getKey("/FontDescriptor");
      if (!cidDescriptor.isDictionary() || !cidDescriptor.hasKey("/FontFile2"))
        continue;

      auto fontFile = cidDescriptor.getKey("/FontFile2");
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

        // CID codes = glyph IDs for Identity CIDToGIDMap
        std::set<uint16_t> glyphIds = usedCodes;
        glyphIds.insert(0); // always keep .notdef

        if (glyphIds.size() >= 200)
          continue;

        std::vector<uint8_t> subsetFont;
        if (!subsetTrueTypeFont(ttfData, ttfSize, glyphIds, subsetFont))
          continue;

        if (subsetFont.size() >= ttfSize)
          continue;

        std::string fontStr(reinterpret_cast<char *>(subsetFont.data()),
                            subsetFont.size());
        fontFile.replaceStreamData(fontStr, QPDFObjectHandle::newNull(),
                                   QPDFObjectHandle::newNull());
      } catch (...) {
        continue;
      }
      continue;
    }

    // simple TrueType fonts — subset via cmap lookup
    if (subtype.getName() != "/TrueType")
      continue;

    // skip fonts with /Encoding /Differences — the cmap-based glyph ID
    // lookup doesn't account for character code remapping via /Differences,
    // which would cause the wrong glyphs to be retained
    auto encoding = fontObj.getKey("/Encoding");
    if (encoding.isDictionary() && encoding.hasKey("/Differences"))
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

      // map character codes → glyph IDs via cmap
      auto glyphIds = mapCodesToGlyphIds(ttfData, ttfSize, usedCodes);

      if (glyphIds.size() >= 200)
        continue;

      std::vector<uint8_t> subsetFont;
      if (!subsetTrueTypeFont(ttfData, ttfSize, glyphIds, subsetFont))
        continue;

      // only replace if the subset is smaller
      if (subsetFont.size() >= ttfSize)
        continue;

      std::string fontStr(reinterpret_cast<char *>(subsetFont.data()),
                          subsetFont.size());
      fontFile.replaceStreamData(fontStr, QPDFObjectHandle::newNull(),
                                 QPDFObjectHandle::newNull());
    } catch (...) {
      continue;
    }
  }
}
