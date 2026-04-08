#include "optimize.h"
#include "font_subset.h"
#include "images.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include <qpdf/Buffer.hh>
#include <qpdf/QPDFObjectHandle.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>
#include <qpdf/QPDFPageObjectHelper.hh>

// ---------------------------------------------------------------------------
// Metadata stripping
// ---------------------------------------------------------------------------

void stripMetadata(QPDF &qpdf) {
  auto root = qpdf.getRoot();

  // remove XMP metadata stream
  if (root.hasKey("/Metadata"))
    root.removeKey("/Metadata");

  // remove document info dictionary
  auto trailer = qpdf.getTrailer();
  if (trailer.hasKey("/Info"))
    trailer.removeKey("/Info");

  // remove page-level metadata and PieceInfo
  for (auto &page : QPDFPageDocumentHelper(qpdf).getAllPages()) {
    auto pageObj = page.getObjectHandle();
    if (pageObj.hasKey("/Metadata"))
      pageObj.removeKey("/Metadata");
    if (pageObj.hasKey("/PieceInfo"))
      pageObj.removeKey("/PieceInfo");
  }

  // remove embedded thumbnails
  for (auto &page : QPDFPageDocumentHelper(qpdf).getAllPages()) {
    auto pageObj = page.getObjectHandle();
    if (pageObj.hasKey("/Thumb"))
      pageObj.removeKey("/Thumb");
  }

  // remove MarkInfo and page labels (optional metadata)
  if (root.hasKey("/MarkInfo"))
    root.removeKey("/MarkInfo");
}

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
// Content stream coalescing — merge multiple content streams per page into one
// ---------------------------------------------------------------------------

void coalesceContentStreams(QPDF &qpdf) {
  for (auto &page : QPDFPageDocumentHelper(qpdf).getAllPages()) {
    auto pageObj = page.getObjectHandle();
    auto contents = pageObj.getKey("/Contents");

    // only coalesce if there are multiple content streams (array)
    if (contents.isArray() && contents.getArrayNItems() > 1) {
      page.coalesceContentStreams();
    }
  }
}

// ---------------------------------------------------------------------------
// Deduplicate identical non-image streams (fonts, ICC profiles, etc.)
// ---------------------------------------------------------------------------

void deduplicateStreams(QPDF &qpdf) {
  // collect all stream objects and their raw data hashes
  struct StreamEntry {
    QPDFObjGen og;
    size_t dataSize;
    QPDFObjectHandle handle;
  };

  std::unordered_map<uint64_t, std::vector<StreamEntry>> hashGroups;
  std::set<QPDFObjGen> imageObjGens;

  // collect image object IDs to skip them (already handled by
  // deduplicateImages)
  forEachImage(qpdf, [&](const std::string &, QPDFObjectHandle xobj,
                         QPDFObjectHandle, QPDFPageObjectHelper &) {
    imageObjGens.insert(xobj.getObjGen());
  });

  for (auto &obj : qpdf.getAllObjects()) {
    if (!obj.isStream())
      continue;

    auto og = obj.getObjGen();

    // skip images
    if (imageObjGens.count(og))
      continue;

    try {
      auto rawData = obj.getRawStreamData();
      size_t size = rawData->getSize();
      if (size == 0)
        continue;

      // FNV-1a hash
      uint64_t hash = 14695981039346656037ULL;
      auto *p = rawData->getBuffer();
      for (size_t i = 0; i < size; ++i) {
        hash ^= static_cast<uint64_t>(p[i]);
        hash *= 1099511628211ULL;
      }

      hashGroups[hash].push_back({og, size, obj});
    } catch (...) {
    }
  }

  // find duplicates via full byte comparison
  std::map<QPDFObjGen, QPDFObjectHandle> replacements;

  for (auto &[hash, group] : hashGroups) {
    if (group.size() < 2)
      continue;

    for (size_t i = 0; i < group.size(); ++i) {
      if (replacements.count(group[i].og))
        continue;

      auto rawI = group[i].handle.getRawStreamData();
      for (size_t j = i + 1; j < group.size(); ++j) {
        if (replacements.count(group[j].og))
          continue;

        auto rawJ = group[j].handle.getRawStreamData();
        if (rawI->getSize() != rawJ->getSize())
          continue;

        if (memcmp(rawI->getBuffer(), rawJ->getBuffer(), rawI->getSize()) ==
            0) {
          // verify stream dictionaries are compatible
          auto dictI = group[i].handle.getDict();
          auto dictJ = group[j].handle.getDict();

          auto filterI = dictI.getKey("/Filter");
          auto filterJ = dictJ.getKey("/Filter");
          bool filtersMatch = (filterI.isName() && filterJ.isName() &&
                               filterI.getName() == filterJ.getName()) ||
                              (!filterI.isName() && !filterJ.isName());

          // also verify DecodeParms match — different predictors on
          // identical raw bytes would produce different decoded content
          auto dpI = dictI.getKey("/DecodeParms");
          auto dpJ = dictJ.getKey("/DecodeParms");
          bool paramsMatch = (dpI.isNull() && dpJ.isNull()) ||
                             (!dpI.isNull() && !dpJ.isNull() &&
                              dpI.unparse() == dpJ.unparse());

          if (filtersMatch && paramsMatch)
            replacements[group[j].og] = group[i].handle;
        }
      }
    }
  }

  if (replacements.empty())
    return;

  // rewrite references: scan all objects for indirect references to duplicates
  for (auto &obj : qpdf.getAllObjects()) {
    if (!obj.isDictionary() && !obj.isStream())
      continue;

    auto dict = obj.isStream() ? obj.getDict() : obj;
    for (auto &key : dict.getKeys()) {
      auto val = dict.getKey(key);
      if (val.isIndirect()) {
        auto it = replacements.find(val.getObjGen());
        if (it != replacements.end())
          dict.replaceKey(key, it->second);
      }
    }
  }
}

// ---------------------------------------------------------------------------
// Font subsetting — remove unused glyphs from TrueType/CIDFont fonts
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// ICC profile stripping — replace ICCBased color spaces with Device
// equivalents
// ---------------------------------------------------------------------------

void stripIccProfiles(QPDF &qpdf) {
  std::set<QPDFObjGen> processed;

  // strip ICC profiles from images
  forEachImage(qpdf, [&](const std::string &, QPDFObjectHandle xobj,
                         QPDFObjectHandle, QPDFPageObjectHelper &) {
    auto og = xobj.getObjGen();
    if (processed.count(og))
      return;
    processed.insert(og);

    auto dict = xobj.getDict();
    auto cs = dict.getKey("/ColorSpace");

    if (!cs.isArray() || cs.getArrayNItems() < 2)
      return;

    auto csName = cs.getArrayItem(0);
    if (!csName.isName() || csName.getName() != "/ICCBased")
      return;

    auto profile = cs.getArrayItem(1);
    if (!profile.isStream())
      return;

    auto n = profile.getDict().getKey("/N");
    if (!n.isInteger())
      return;

    int components = static_cast<int>(n.getIntValue());
    if (components == 3)
      dict.replaceKey("/ColorSpace", QPDFObjectHandle::newName("/DeviceRGB"));
    else if (components == 1)
      dict.replaceKey("/ColorSpace", QPDFObjectHandle::newName("/DeviceGray"));
    else if (components == 4)
      dict.replaceKey("/ColorSpace", QPDFObjectHandle::newName("/DeviceCMYK"));
  });

  // strip ICC profiles from page-level color space resources
  for (auto &page : QPDFPageDocumentHelper(qpdf).getAllPages()) {
    auto resources = page.getObjectHandle().getKey("/Resources");
    if (!resources.isDictionary())
      continue;

    auto colorSpaces = resources.getKey("/ColorSpace");
    if (!colorSpaces.isDictionary())
      continue;

    for (auto &key : colorSpaces.getKeys()) {
      auto cs = colorSpaces.getKey(key);
      if (!cs.isArray() || cs.getArrayNItems() < 2)
        continue;

      auto csName = cs.getArrayItem(0);
      if (!csName.isName() || csName.getName() != "/ICCBased")
        continue;

      auto profile = cs.getArrayItem(1);
      if (!profile.isStream())
        continue;

      auto n = profile.getDict().getKey("/N");
      if (!n.isInteger())
        continue;

      int components = static_cast<int>(n.getIntValue());
      if (components == 3)
        colorSpaces.replaceKey(key, QPDFObjectHandle::newName("/DeviceRGB"));
      else if (components == 1)
        colorSpaces.replaceKey(key, QPDFObjectHandle::newName("/DeviceGray"));
      else if (components == 4)
        colorSpaces.replaceKey(key, QPDFObjectHandle::newName("/DeviceCMYK"));
    }
  }
}

// ---------------------------------------------------------------------------
// Embedded file stripping — remove /EmbeddedFiles from the name tree
// ---------------------------------------------------------------------------

void stripEmbeddedFiles(QPDF &qpdf) {
  auto root = qpdf.getRoot();
  if (!root.hasKey("/Names"))
    return;

  auto names = root.getKey("/Names");
  if (!names.isDictionary())
    return;

  if (names.hasKey("/EmbeddedFiles"))
    names.removeKey("/EmbeddedFiles");

  // if /Names is now empty, remove it too
  if (names.getKeys().empty())
    root.removeKey("/Names");
}

// ---------------------------------------------------------------------------
// JavaScript and action removal — strip JS, open actions, and additional
// actions from the catalog and all pages
// ---------------------------------------------------------------------------

void stripJavaScript(QPDF &qpdf) {
  auto root = qpdf.getRoot();

  // remove document-level open action
  if (root.hasKey("/OpenAction"))
    root.removeKey("/OpenAction");

  // remove document-level additional actions
  if (root.hasKey("/AA"))
    root.removeKey("/AA");

  // remove /JavaScript name tree
  if (root.hasKey("/Names")) {
    auto names = root.getKey("/Names");
    if (names.isDictionary() && names.hasKey("/JavaScript"))
      names.removeKey("/JavaScript");
  }

  // remove page-level actions and annotations with JS
  for (auto &page : QPDFPageDocumentHelper(qpdf).getAllPages()) {
    auto pageObj = page.getObjectHandle();

    if (pageObj.hasKey("/AA"))
      pageObj.removeKey("/AA");

    // strip JS actions from annotations
    if (!pageObj.hasKey("/Annots"))
      continue;

    auto annots = pageObj.getKey("/Annots");
    if (!annots.isArray())
      continue;

    for (int i = 0; i < annots.getArrayNItems(); ++i) {
      auto annot = annots.getArrayItem(i);
      if (!annot.isDictionary())
        continue;
      if (annot.hasKey("/AA"))
        annot.removeKey("/AA");
      if (annot.hasKey("/A")) {
        auto action = annot.getKey("/A");
        if (action.isDictionary()) {
          auto s = action.getKey("/S");
          if (s.isName() && s.getName() == "/JavaScript")
            annot.removeKey("/A");
        }
      }
    }
  }
}

// ---------------------------------------------------------------------------
// Form flattening — merge interactive form field appearances into page
// content and remove the /AcroForm dictionary
// ---------------------------------------------------------------------------

void flattenForms(QPDF &qpdf) {
  auto root = qpdf.getRoot();
  if (!root.hasKey("/AcroForm"))
    return;

  // stamp each widget annotation's appearance into the page content,
  // then remove the annotation
  for (auto &page : QPDFPageDocumentHelper(qpdf).getAllPages()) {
    auto pageObj = page.getObjectHandle();
    if (!pageObj.hasKey("/Annots"))
      continue;

    auto annots = pageObj.getKey("/Annots");
    if (!annots.isArray())
      continue;

    std::vector<int> widgetIndices;
    for (int i = 0; i < annots.getArrayNItems(); ++i) {
      auto annot = annots.getArrayItem(i);
      if (!annot.isDictionary())
        continue;

      auto subtype = annot.getKey("/Subtype");
      if (!subtype.isName() || subtype.getName() != "/Widget")
        continue;

      // check if there's a normal appearance to flatten
      auto ap = annot.getKey("/AP");
      if (!ap.isDictionary())
        continue;
      auto nAppearance = ap.getKey("/N");
      if (!nAppearance.isStream())
        continue;

      // get widget rectangle
      auto rect = annot.getKey("/Rect");
      if (!rect.isArray() || rect.getArrayNItems() < 4)
        continue;

      try {
        double x1 = rect.getArrayItem(0).getNumericValue();
        double y1 = rect.getArrayItem(1).getNumericValue();
        double x2 = rect.getArrayItem(2).getNumericValue();
        double y2 = rect.getArrayItem(3).getNumericValue();

        double w = x2 - x1;
        double h = y2 - y1;
        if (w <= 0 || h <= 0)
          continue;

        // get appearance stream bounding box for scaling
        auto apDict = nAppearance.getDict();
        double scaleX = 1.0, scaleY = 1.0;
        if (apDict.hasKey("/BBox")) {
          auto bbox = apDict.getKey("/BBox");
          if (bbox.isArray() && bbox.getArrayNItems() >= 4) {
            double bw = bbox.getArrayItem(2).getNumericValue() -
                        bbox.getArrayItem(0).getNumericValue();
            double bh = bbox.getArrayItem(3).getNumericValue() -
                        bbox.getArrayItem(1).getNumericValue();
            if (bw > 0)
              scaleX = w / bw;
            if (bh > 0)
              scaleY = h / bh;
          }
        }

        // register appearance as a form XObject on the page
        auto resources = pageObj.getKey("/Resources");
        if (!resources.isDictionary()) {
          resources = QPDFObjectHandle::newDictionary();
          pageObj.replaceKey("/Resources", resources);
        }
        auto xobjects = resources.getKey("/XObject");
        if (!xobjects.isDictionary()) {
          xobjects = QPDFObjectHandle::newDictionary();
          resources.replaceKey("/XObject", xobjects);
        }

        std::string xobjName = "/FlatForm" + std::to_string(i);
        xobjects.replaceKey(xobjName, nAppearance);

        // ensure the appearance stream has /Type /XObject /Subtype /Form
        if (!apDict.hasKey("/Type"))
          apDict.replaceKey("/Type", QPDFObjectHandle::newName("/XObject"));
        if (!apDict.hasKey("/Subtype"))
          apDict.replaceKey("/Subtype", QPDFObjectHandle::newName("/Form"));

        // build content stream snippet to stamp the appearance
        std::string snippet = "q " + std::to_string(scaleX) + " 0 0 " +
                              std::to_string(scaleY) + " " +
                              std::to_string(x1) + " " + std::to_string(y1) +
                              " cm " + xobjName + " Do Q\n";

        // append to page content stream
        page.addPageContents(QPDFObjectHandle::newStream(&qpdf, snippet),
                             false);

        widgetIndices.push_back(i);
      } catch (...) {
        continue;
      }
    }

    // remove widget annotations (reverse order to preserve indices)
    for (auto it = widgetIndices.rbegin(); it != widgetIndices.rend(); ++it)
      annots.eraseItem(*it);
  }

  // remove the /AcroForm dictionary
  root.removeKey("/AcroForm");
}

// ---------------------------------------------------------------------------
// Page tree flattening — push inherited attributes to pages so QPDFWriter
// can generate a flat single-level page tree
// ---------------------------------------------------------------------------

void flattenPageTree(QPDF &qpdf) { qpdf.pushInheritedAttributesToPage(); }

// ---------------------------------------------------------------------------
// Content stream minification — normalize whitespace and number formatting
// to reduce content stream size before Flate compression
// ---------------------------------------------------------------------------

// trims a numeric string: remove trailing zeros after decimal point,
// remove the decimal point if it becomes the last char,
// and strip a leading zero for values between -1 and 1.
static std::string trimNumber(const std::string &s) {
  // only process strings that look like decimal numbers
  if (s.find('.') == std::string::npos)
    return s;

  std::string result = s;

  // strip trailing zeros after decimal point
  size_t dot = result.find('.');
  if (dot != std::string::npos) {
    size_t last = result.size() - 1;
    while (last > dot && result[last] == '0')
      --last;
    if (last == dot)
      result.erase(dot); // remove the dot too (e.g. "1." → "1")
    else
      result.erase(last + 1);
  }

  // strip leading zero for values like "0.5" → ".5" or "-0.5" → "-.5"
  if (result.size() >= 2 && result[0] == '0' && result[1] == '.')
    result.erase(0, 1);
  else if (result.size() >= 3 && result[0] == '-' && result[1] == '0' &&
           result[2] == '.')
    result.erase(1, 1);

  return result;
}

void minifyContentStreams(QPDF &qpdf) {
  for (auto &page : QPDFPageDocumentHelper(qpdf).getAllPages()) {
    auto pageObj = page.getObjectHandle();
    auto contents = pageObj.getKey("/Contents");

    if (!contents.isStream())
      continue;

    std::string raw;
    try {
      auto buf = contents.getStreamData(qpdf_dl_generalized);
      raw.assign(reinterpret_cast<const char *>(buf->getBuffer()),
                 buf->getSize());
    } catch (...) {
      continue;
    }

    // tokenize preserving string literals and hex strings intact
    std::string minified;
    minified.reserve(raw.size());
    bool needSpace = false;
    size_t pos = 0;

    while (pos < raw.size()) {
      char ch = raw[pos];

      // skip whitespace
      if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') {
        if (!minified.empty())
          needSpace = true;
        ++pos;
        continue;
      }

      // comments — skip to end of line
      if (ch == '%') {
        while (pos < raw.size() && raw[pos] != '\n')
          ++pos;
        continue;
      }

      // literal string — copy verbatim
      if (ch == '(') {
        if (needSpace) {
          minified += '\n';
          needSpace = false;
        }
        int depth = 1;
        minified += '(';
        ++pos;
        while (pos < raw.size() && depth > 0) {
          if (raw[pos] == '\\') {
            minified += raw[pos++];
            if (pos < raw.size())
              minified += raw[pos++];
          } else {
            if (raw[pos] == '(')
              ++depth;
            else if (raw[pos] == ')')
              --depth;
            minified += raw[pos++];
          }
        }
        needSpace = true;
        continue;
      }

      // hex string — copy verbatim
      if (ch == '<' && pos + 1 < raw.size() && raw[pos + 1] != '<') {
        if (needSpace) {
          minified += '\n';
          needSpace = false;
        }
        minified += '<';
        ++pos;
        while (pos < raw.size() && raw[pos] != '>') {
          if (!std::isspace(static_cast<unsigned char>(raw[pos])))
            minified += raw[pos];
          ++pos;
        }
        if (pos < raw.size()) {
          minified += '>';
          ++pos;
        }
        needSpace = true;
        continue;
      }

      // dict delimiters << >> — self-delimiting, no space needed around them
      if (ch == '<' && pos + 1 < raw.size() && raw[pos + 1] == '<') {
        if (needSpace) {
          minified += '\n';
          needSpace = false;
        }
        minified += "<<";
        pos += 2;
        continue;
      }
      if (ch == '>' && pos + 1 < raw.size() && raw[pos + 1] == '>') {
        minified += ">>";
        pos += 2;
        needSpace = true;
        continue;
      }

      // array delimiters — self-delimiting
      if (ch == '[' || ch == ']') {
        if (needSpace && ch == '[') {
          minified += '\n';
          needSpace = false;
        }
        minified += ch;
        ++pos;
        if (ch == ']')
          needSpace = true;
        continue;
      }

      // name — starts with /
      if (ch == '/') {
        if (needSpace) {
          minified += '\n';
          needSpace = false;
        }
        size_t start = pos;
        ++pos;
        while (pos < raw.size() &&
               !std::isspace(static_cast<unsigned char>(raw[pos])) &&
               raw[pos] != '/' && raw[pos] != '[' && raw[pos] != ']' &&
               raw[pos] != '<' && raw[pos] != '>' && raw[pos] != '(' &&
               raw[pos] != ')')
          ++pos;
        minified.append(raw, start, pos - start);
        needSpace = true;
        continue;
      }

      // regular token (number, operator)
      {
        if (needSpace) {
          minified += '\n';
          needSpace = false;
        }
        size_t start = pos;
        while (pos < raw.size() &&
               !std::isspace(static_cast<unsigned char>(raw[pos])) &&
               raw[pos] != '/' && raw[pos] != '[' && raw[pos] != ']' &&
               raw[pos] != '<' && raw[pos] != '>' && raw[pos] != '(' &&
               raw[pos] != ')')
          ++pos;

        std::string token(raw, start, pos - start);

        // trim numeric formatting
        if (!token.empty() &&
            (token[0] == '-' || token[0] == '+' || token[0] == '.' ||
             (token[0] >= '0' && token[0] <= '9'))) {
          token = trimNumber(token);
        }

        minified += token;
        needSpace = true;
      }
    }

    // only replace if we actually reduced the size
    if (minified.size() >= raw.size())
      continue;

    contents.replaceStreamData(minified, QPDFObjectHandle::newNull(),
                               QPDFObjectHandle::newNull());
  }
}
