#include "optimize.h"
#include "images.h"

#include <cctype>
#include <cstdint>
#include <cstdlib>
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
    std::set<std::string> usedFonts;

    try {
      // get unparsed content stream data
      auto contents = pageObj.getKey("/Contents");
      std::string contentStr;

      if (contents.isStream()) {
        auto buf = contents.getStreamData(qpdf_dl_generalized);
        contentStr.assign(reinterpret_cast<const char *>(buf->getBuffer()),
                          buf->getSize());
      } else if (contents.isArray()) {
        for (int i = 0; i < contents.getArrayNItems(); ++i) {
          auto stream = contents.getArrayItem(i);
          if (stream.isStream()) {
            auto buf = stream.getStreamData(qpdf_dl_generalized);
            contentStr.append(reinterpret_cast<const char *>(buf->getBuffer()),
                              buf->getSize());
            contentStr += '\n';
          }
        }
      }

      // scan for /FontName references — Tf operator uses font name
      // pattern: /FontName <size> Tf
      for (auto &fontKey : fonts.getKeys()) {
        // fontKey includes the leading '/', e.g. "/F1"
        if (contentStr.find(fontKey) != std::string::npos)
          usedFonts.insert(fontKey);
      }
    } catch (...) {
      continue; // skip this page if content can't be read
    }

    // remove fonts that are not referenced in the content stream
    auto allFontKeys = fonts.getKeys();
    for (auto &fontKey : allFontKeys) {
      if (usedFonts.find(fontKey) == usedFonts.end())
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
          // verify stream dictionaries are compatible (same /Filter, /Subtype)
          auto dictI = group[i].handle.getDict();
          auto dictJ = group[j].handle.getDict();

          auto filterI = dictI.getKey("/Filter");
          auto filterJ = dictJ.getKey("/Filter");
          bool filtersMatch = (filterI.isName() && filterJ.isName() &&
                               filterI.getName() == filterJ.getName()) ||
                              (!filterI.isName() && !filterJ.isName());

          if (filtersMatch)
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

// collects all Unicode code points used in a page's content stream by
// parsing text-showing operators (Tj, TJ, ', ")
static std::set<uint16_t> collectUsedCodes(QPDFPageObjectHelper &page,
                                           const std::string &fontKey,
                                           QPDFObjectHandle fontObj) {
  std::set<uint16_t> usedCodes;

  auto pageObj = page.getObjectHandle();
  auto contents = pageObj.getKey("/Contents");

  std::string contentStr;
  try {
    if (contents.isStream()) {
      auto buf = contents.getStreamData(qpdf_dl_generalized);
      contentStr.assign(reinterpret_cast<const char *>(buf->getBuffer()),
                        buf->getSize());
    } else if (contents.isArray()) {
      for (int i = 0; i < contents.getArrayNItems(); ++i) {
        auto stream = contents.getArrayItem(i);
        if (stream.isStream()) {
          auto buf = stream.getStreamData(qpdf_dl_generalized);
          contentStr.append(reinterpret_cast<const char *>(buf->getBuffer()),
                            buf->getSize());
          contentStr += '\n';
        }
      }
    }
  } catch (...) {
    return usedCodes;
  }

  // simple scan: find all string operands between font selection (Tf) and
  // text-showing operators. For simplicity, collect all hex/literal string
  // bytes used when this font is active.

  // check if this font uses 2-byte CID encoding
  auto subtypeKey = fontObj.getKey("/Subtype");
  bool isCIDFont = subtypeKey.isName() && subtypeKey.getName() == "/Type0";

  // find all ranges where this font is active and collect string bytes
  bool fontActive = false;
  size_t pos = 0;

  while (pos < contentStr.size()) {
    // skip whitespace
    while (pos < contentStr.size() && std::isspace(contentStr[pos]))
      ++pos;

    if (pos >= contentStr.size())
      break;

    // check for font selection: /FontName ... Tf
    if (contentStr[pos] == '/') {
      // read name
      size_t nameStart = pos;
      ++pos;
      while (pos < contentStr.size() && !std::isspace(contentStr[pos]) &&
             contentStr[pos] != '/' && contentStr[pos] != '<' &&
             contentStr[pos] != '(' && contentStr[pos] != '[')
        ++pos;
      std::string name = contentStr.substr(nameStart, pos - nameStart);

      // look ahead for Tf
      size_t lookAhead = pos;
      while (lookAhead < contentStr.size() &&
             std::isspace(contentStr[lookAhead]))
        ++lookAhead;
      // skip number
      while (
          lookAhead < contentStr.size() &&
          (std::isdigit(contentStr[lookAhead]) || contentStr[lookAhead] == '.'))
        ++lookAhead;
      while (lookAhead < contentStr.size() &&
             std::isspace(contentStr[lookAhead]))
        ++lookAhead;
      if (lookAhead + 1 < contentStr.size() && contentStr[lookAhead] == 'T' &&
          contentStr[lookAhead + 1] == 'f') {
        fontActive = (name == fontKey);
        pos = lookAhead + 2;
        continue;
      }
    }

    // collect string data when our font is active
    if (fontActive && contentStr[pos] == '(') {
      // literal string
      ++pos;
      int depth = 1;
      while (pos < contentStr.size() && depth > 0) {
        if (contentStr[pos] == '\\') {
          ++pos; // skip escaped char
          if (pos < contentStr.size())
            ++pos;
          continue;
        }
        if (contentStr[pos] == '(')
          ++depth;
        else if (contentStr[pos] == ')')
          --depth;
        if (depth > 0) {
          if (isCIDFont && pos + 1 < contentStr.size()) {
            uint16_t code = (static_cast<uint8_t>(contentStr[pos]) << 8) |
                            static_cast<uint8_t>(contentStr[pos + 1]);
            usedCodes.insert(code);
            ++pos;
          } else {
            usedCodes.insert(static_cast<uint8_t>(contentStr[pos]));
          }
          ++pos;
        }
      }
      if (depth == 0)
        ++pos; // skip closing ')'
      continue;
    }

    if (fontActive && contentStr[pos] == '<') {
      // hex string
      ++pos;
      std::vector<uint8_t> hexBytes;
      while (pos < contentStr.size() && contentStr[pos] != '>') {
        if (std::isxdigit(contentStr[pos])) {
          char hex[3] = {contentStr[pos], '0', '\0'};
          if (pos + 1 < contentStr.size() &&
              std::isxdigit(contentStr[pos + 1])) {
            hex[1] = contentStr[pos + 1];
            ++pos;
          }
          hexBytes.push_back(
              static_cast<uint8_t>(std::strtol(hex, nullptr, 16)));
        }
        ++pos;
      }
      if (pos < contentStr.size())
        ++pos; // skip '>'

      if (isCIDFont) {
        for (size_t b = 0; b + 1 < hexBytes.size(); b += 2) {
          uint16_t code = (hexBytes[b] << 8) | hexBytes[b + 1];
          usedCodes.insert(code);
        }
      } else {
        for (auto b : hexBytes)
          usedCodes.insert(b);
      }
      continue;
    }

    // skip other tokens
    while (pos < contentStr.size() && !std::isspace(contentStr[pos]))
      ++pos;
  }

  return usedCodes;
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

    for (auto &key : fonts.getKeys()) {
      auto fontObj = fonts.getKey(key);
      if (!fontObj.isDictionary())
        continue;

      auto og = fontObj.getObjGen();
      auto codes = collectUsedCodes(page, key, fontObj);
      fontUsedCodes[og].insert(codes.begin(), codes.end());
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
