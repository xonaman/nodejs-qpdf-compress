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

namespace {

/// Checks if a font BaseFont name has a subset prefix (e.g., "ABCDEF+ArialMT").
bool isAlreadySubset(QPDFObjectHandle fontObj) {
  auto baseFont = fontObj.getKey("/BaseFont");
  if (!baseFont.isName())
    return false;
  auto name = baseFont.getName();
  if (!name.empty() && name[0] == '/')
    name = name.substr(1);
  if (name.size() <= 7 || name[6] != '+')
    return false;
  for (int i = 0; i < 6; ++i) {
    if (name[i] < 'A' || name[i] > 'Z')
      return false;
  }
  return true;
}

} // namespace

// ---------------------------------------------------------------------------
// Combined parser callback: collects font names referenced by Tf operators
// AND character codes used by each font in a single pass
// ---------------------------------------------------------------------------

class FontUsageCollector : public QPDFObjectHandle::ParserCallbacks {
public:
  std::map<std::string, std::set<uint16_t>> fontCodes;
  std::set<std::string> cidFonts; // set before parsing

  void handleObject(QPDFObjectHandle obj) override {
    if (obj.isOperator()) {
      std::string op = obj.getOperatorValue();
      if (op == "Tf" && operands.size() >= 2) {
        auto nameObj = operands[operands.size() - 2];
        if (nameObj.isName()) {
          currentFont = nameObj.getName();
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

// convert Adobe Glyph List (AGL) name to Unicode codepoint. handles:
// - "uniXXXX" convention (4 hex digits after "uni")
// - "uXXXX"/"uXXXXX" convention (4-6 hex digits after "u")
// - standard AGL names for common Latin/currency/typographic characters
static uint16_t glyphNameToUnicode(const std::string &name) {
  // strip any variant suffix (e.g., "Euro.oldstyle" → "Euro")
  std::string base = name;
  auto dot = base.find('.');
  if (dot != std::string::npos)
    base = base.substr(0, dot);

  // "uniXXXX" — standard Unicode naming convention
  if (base.size() == 7 && base[0] == 'u' && base[1] == 'n' && base[2] == 'i') {
    char *end = nullptr;
    unsigned long val = strtoul(base.c_str() + 3, &end, 16);
    if (end == base.c_str() + 7 && val > 0 && val <= 0xFFFF)
      return static_cast<uint16_t>(val);
  }

  // "uXXXX" or "uXXXXX" — alternate Unicode naming convention
  if (base.size() >= 5 && base.size() <= 7 && base[0] == 'u' &&
      base[1] != 'n') {
    char *end = nullptr;
    unsigned long val = strtoul(base.c_str() + 1, &end, 16);
    if (end == base.c_str() + static_cast<ptrdiff_t>(base.size()) && val > 0 &&
        val <= 0xFFFF)
      return static_cast<uint16_t>(val);
  }

  // single-character names map to their ASCII value
  if (base.size() == 1 && base[0] >= 0x20)
    return static_cast<uint16_t>(static_cast<unsigned char>(base[0]));

  // standard AGL names — covers WinAnsi, Latin Extended, and common symbols
  // sourced from the Adobe Glyph List (agl-aglfn) specification
  static const std::map<std::string, uint16_t> agl = {
      // basic ASCII names
      {"space", 0x0020},
      {"exclam", 0x0021},
      {"quotedbl", 0x0022},
      {"numbersign", 0x0023},
      {"dollar", 0x0024},
      {"percent", 0x0025},
      {"ampersand", 0x0026},
      {"quotesingle", 0x0027},
      {"parenleft", 0x0028},
      {"parenright", 0x0029},
      {"asterisk", 0x002A},
      {"plus", 0x002B},
      {"comma", 0x002C},
      {"hyphen", 0x002D},
      {"period", 0x002E},
      {"slash", 0x002F},
      {"zero", 0x0030},
      {"one", 0x0031},
      {"two", 0x0032},
      {"three", 0x0033},
      {"four", 0x0034},
      {"five", 0x0035},
      {"six", 0x0036},
      {"seven", 0x0037},
      {"eight", 0x0038},
      {"nine", 0x0039},
      {"colon", 0x003A},
      {"semicolon", 0x003B},
      {"less", 0x003C},
      {"equal", 0x003D},
      {"greater", 0x003E},
      {"question", 0x003F},
      {"at", 0x0040},
      {"bracketleft", 0x005B},
      {"backslash", 0x005C},
      {"bracketright", 0x005D},
      {"asciicircum", 0x005E},
      {"underscore", 0x005F},
      {"grave", 0x0060},
      {"braceleft", 0x007B},
      {"bar", 0x007C},
      {"braceright", 0x007D},
      {"asciitilde", 0x007E},
      // WinAnsi 0x80-0x9F range
      {"Euro", 0x20AC},
      {"quotesinglbase", 0x201A},
      {"florin", 0x0192},
      {"quotedblbase", 0x201E},
      {"ellipsis", 0x2026},
      {"dagger", 0x2020},
      {"daggerdbl", 0x2021},
      {"circumflex", 0x02C6},
      {"perthousand", 0x2030},
      {"Scaron", 0x0160},
      {"guilsinglleft", 0x2039},
      {"OE", 0x0152},
      {"Zcaron", 0x017D},
      {"quoteleft", 0x2018},
      {"quoteright", 0x2019},
      {"quotedblleft", 0x201C},
      {"quotedblright", 0x201D},
      {"bullet", 0x2022},
      {"endash", 0x2013},
      {"emdash", 0x2014},
      {"tilde", 0x02DC},
      {"trademark", 0x2122},
      {"scaron", 0x0161},
      {"guilsinglright", 0x203A},
      {"oe", 0x0153},
      {"zcaron", 0x017E},
      {"Ydieresis", 0x0178},
      // Latin-1 Supplement (0xA0-0xFF)
      {"nbspace", 0x00A0},
      {"exclamdown", 0x00A1},
      {"cent", 0x00A2},
      {"sterling", 0x00A3},
      {"currency", 0x00A4},
      {"yen", 0x00A5},
      {"brokenbar", 0x00A6},
      {"section", 0x00A7},
      {"dieresis", 0x00A8},
      {"copyright", 0x00A9},
      {"ordfeminine", 0x00AA},
      {"guillemotleft", 0x00AB},
      {"logicalnot", 0x00AC},
      {"softhyphen", 0x00AD},
      {"registered", 0x00AE},
      {"macron", 0x00AF},
      {"degree", 0x00B0},
      {"plusminus", 0x00B1},
      {"twosuperior", 0x00B2},
      {"threesuperior", 0x00B3},
      {"acute", 0x00B4},
      {"mu", 0x00B5},
      {"paragraph", 0x00B6},
      {"periodcentered", 0x00B7},
      {"cedilla", 0x00B8},
      {"onesuperior", 0x00B9},
      {"ordmasculine", 0x00BA},
      {"guillemotright", 0x00BB},
      {"onequarter", 0x00BC},
      {"onehalf", 0x00BD},
      {"threequarters", 0x00BE},
      {"questiondown", 0x00BF},
      {"Agrave", 0x00C0},
      {"Aacute", 0x00C1},
      {"Acircumflex", 0x00C2},
      {"Atilde", 0x00C3},
      {"Adieresis", 0x00C4},
      {"Aring", 0x00C5},
      {"AE", 0x00C6},
      {"Ccedilla", 0x00C7},
      {"Egrave", 0x00C8},
      {"Eacute", 0x00C9},
      {"Ecircumflex", 0x00CA},
      {"Edieresis", 0x00CB},
      {"Igrave", 0x00CC},
      {"Iacute", 0x00CD},
      {"Icircumflex", 0x00CE},
      {"Idieresis", 0x00CF},
      {"Eth", 0x00D0},
      {"Ntilde", 0x00D1},
      {"Ograve", 0x00D2},
      {"Oacute", 0x00D3},
      {"Ocircumflex", 0x00D4},
      {"Otilde", 0x00D5},
      {"Odieresis", 0x00D6},
      {"multiply", 0x00D7},
      {"Oslash", 0x00D8},
      {"Ugrave", 0x00D9},
      {"Uacute", 0x00DA},
      {"Ucircumflex", 0x00DB},
      {"Udieresis", 0x00DC},
      {"Yacute", 0x00DD},
      {"Thorn", 0x00DE},
      {"germandbls", 0x00DF},
      {"agrave", 0x00E0},
      {"aacute", 0x00E1},
      {"acircumflex", 0x00E2},
      {"atilde", 0x00E3},
      {"adieresis", 0x00E4},
      {"aring", 0x00E5},
      {"ae", 0x00E6},
      {"ccedilla", 0x00E7},
      {"egrave", 0x00E8},
      {"eacute", 0x00E9},
      {"ecircumflex", 0x00EA},
      {"edieresis", 0x00EB},
      {"igrave", 0x00EC},
      {"iacute", 0x00ED},
      {"icircumflex", 0x00EE},
      {"idieresis", 0x00EF},
      {"eth", 0x00F0},
      {"ntilde", 0x00F1},
      {"ograve", 0x00F2},
      {"oacute", 0x00F3},
      {"ocircumflex", 0x00F4},
      {"otilde", 0x00F5},
      {"odieresis", 0x00F6},
      {"divide", 0x00F7},
      {"oslash", 0x00F8},
      {"ugrave", 0x00F9},
      {"uacute", 0x00FA},
      {"ucircumflex", 0x00FB},
      {"udieresis", 0x00FC},
      {"yacute", 0x00FD},
      {"thorn", 0x00FE},
      {"ydieresis", 0x00FF},
      // Latin Extended-A (commonly found in CE font /Differences)
      {"Amacron", 0x0100},
      {"amacron", 0x0101},
      {"Abreve", 0x0102},
      {"abreve", 0x0103},
      {"Aogonek", 0x0104},
      {"aogonek", 0x0105},
      {"Cacute", 0x0106},
      {"cacute", 0x0107},
      {"Ccircumflex", 0x0108},
      {"ccircumflex", 0x0109},
      {"Cdotaccent", 0x010A},
      {"cdotaccent", 0x010B},
      {"Ccaron", 0x010C},
      {"ccaron", 0x010D},
      {"Dcaron", 0x010E},
      {"dcaron", 0x010F},
      {"Dcroat", 0x0110},
      {"dcroat", 0x0111},
      {"Emacron", 0x0112},
      {"emacron", 0x0113},
      {"Ebreve", 0x0114},
      {"ebreve", 0x0115},
      {"Edotaccent", 0x0116},
      {"edotaccent", 0x0117},
      {"Eogonek", 0x0118},
      {"eogonek", 0x0119},
      {"Ecaron", 0x011A},
      {"ecaron", 0x011B},
      {"Gbreve", 0x011E},
      {"gbreve", 0x011F},
      {"Gdotaccent", 0x0120},
      {"gdotaccent", 0x0121},
      {"Gcommaaccent", 0x0122},
      {"gcommaaccent", 0x0123},
      {"Idotaccent", 0x0130},
      {"dotlessi", 0x0131},
      {"IJ", 0x0132},
      {"ij", 0x0133},
      {"Lacute", 0x0139},
      {"lacute", 0x013A},
      {"Lcommaaccent", 0x013B},
      {"lcommaaccent", 0x013C},
      {"Lcaron", 0x013D},
      {"lcaron", 0x013E},
      {"Ldot", 0x013F},
      {"ldot", 0x0140},
      {"Lslash", 0x0141},
      {"lslash", 0x0142},
      {"Nacute", 0x0143},
      {"nacute", 0x0144},
      {"Ncommaaccent", 0x0145},
      {"ncommaaccent", 0x0146},
      {"Ncaron", 0x0147},
      {"ncaron", 0x0148},
      {"Eng", 0x014A},
      {"eng", 0x014B},
      {"Omacron", 0x014C},
      {"omacron", 0x014D},
      {"Obreve", 0x014E},
      {"obreve", 0x014F},
      {"Ohungarumlaut", 0x0150},
      {"ohungarumlaut", 0x0151},
      {"Racute", 0x0154},
      {"racute", 0x0155},
      {"Rcommaaccent", 0x0156},
      {"rcommaaccent", 0x0157},
      {"Rcaron", 0x0158},
      {"rcaron", 0x0159},
      {"Sacute", 0x015A},
      {"sacute", 0x015B},
      {"Scircumflex", 0x015C},
      {"scircumflex", 0x015D},
      {"Scedilla", 0x015E},
      {"scedilla", 0x015F},
      {"Tcaron", 0x0164},
      {"tcaron", 0x0165},
      {"Tbar", 0x0166},
      {"tbar", 0x0167},
      {"Umacron", 0x016A},
      {"umacron", 0x016B},
      {"Ubreve", 0x016C},
      {"ubreve", 0x016D},
      {"Uring", 0x016E},
      {"uring", 0x016F},
      {"Uhungarumlaut", 0x0170},
      {"uhungarumlaut", 0x0171},
      {"Uogonek", 0x0172},
      {"uogonek", 0x0173},
      {"Zacute", 0x0179},
      {"zacute", 0x017A},
      {"Zdotaccent", 0x017B},
      {"zdotaccent", 0x017C},
      // common currency symbols
      {"colonmonetary", 0x20A1},
      {"franc", 0x20A3},
      {"lira", 0x20A4},
      {"peseta", 0x20A7},
      {"won", 0x20A9},
      {"dong", 0x20AB},
      // typographic symbols
      {"fi", 0xFB01},
      {"fl", 0xFB02},
      {"minus", 0x2212},
      {"fraction", 0x2044},
  };

  auto it = agl.find(base);
  if (it != agl.end())
    return it->second;
  return 0;
}

// parse /Encoding /Differences to build a map of code → glyph name.
// used to identify which codes are remapped by /Differences.
static std::map<uint16_t, std::string>
getDifferencesCodeMap(QPDFObjectHandle fontObj) {
  std::map<uint16_t, std::string> codeToName;

  auto encoding = fontObj.getKey("/Encoding");
  if (!encoding.isDictionary())
    return codeToName;

  auto diffs = encoding.getKey("/Differences");
  if (!diffs.isArray())
    return codeToName;

  int currentCode = 0;
  for (int i = 0; i < diffs.getArrayNItems(); ++i) {
    auto item = diffs.getArrayItem(i);
    if (item.isInteger()) {
      currentCode = static_cast<int>(item.getIntValue());
    } else if (item.isName()) {
      std::string name = item.getName();
      if (!name.empty() && name[0] == '/')
        name = name.substr(1);
      codeToName[static_cast<uint16_t>(currentCode)] = name;
      ++currentCode;
    }
  }
  return codeToName;
}

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
      // recursively scan Form XObjects (queue-based to handle nesting)
      std::vector<QPDFObjectHandle> xobjQueue;
      std::set<QPDFObjGen> visitedXObjects;
      for (auto &xobjKey : xobjects.getKeys())
        xobjQueue.push_back(xobjects.getKey(xobjKey));

      while (!xobjQueue.empty()) {
        auto xobj = xobjQueue.back();
        xobjQueue.pop_back();
        if (!xobj.isStream())
          continue;
        auto xobjOg = xobj.getObjGen();
        if (visitedXObjects.count(xobjOg))
          continue;
        visitedXObjects.insert(xobjOg);

        auto xobjDict = xobj.getDict();
        auto xobjSubtype = xobjDict.getKey("/Subtype");
        if (!xobjSubtype.isName() || xobjSubtype.getName() != "/Form")
          continue;

        auto xobjRes = xobjDict.getKey("/Resources");
        if (xobjRes.isDictionary() && xobjRes.getKey("/Font").isDictionary())
          scanTargets.push_back({xobj, xobjRes.getKey("/Font")});
        else
          scanTargets.push_back({xobj, fonts});

        // queue nested XObjects for recursive scanning
        if (xobjRes.isDictionary()) {
          auto nestedXObjects = xobjRes.getKey("/XObject");
          if (nestedXObjects.isDictionary()) {
            for (auto &nestedKey : nestedXObjects.getKeys())
              xobjQueue.push_back(nestedXObjects.getKey(nestedKey));
          }
        }
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

    // skip already-subset fonts — their encodings are custom and our usage
    // collector may miscount character codes
    if (isAlreadySubset(fontObj))
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

  // true font subsetting: strip unused glyph outlines from embedded fonts.
  // two-pass approach: first collect glyph IDs per font file (merging across
  // all font objects sharing the same embedded file), then subset each file.
  std::set<QPDFObjGen> processedFonts;
  std::map<QPDFObjGen, std::set<uint16_t>> fontFileGlyphIds;
  std::map<QPDFObjGen, QPDFObjectHandle> fontFileHandles;
  std::map<QPDFObjGen, bool> fontFileCID; // true if CID font
  std::set<QPDFObjGen> fontFileSkip;      // skip if any font fails safety check

  // pass 1: collect glyph IDs from each font object
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
      // skip already-subset CID fonts
      if (isAlreadySubset(fontObj))
        continue;

      auto descendants = fontObj.getKey("/DescendantFonts");
      if (!descendants.isArray() || descendants.getArrayNItems() < 1)
        continue;

      auto cidFont = descendants.getArrayItem(0);
      if (!cidFont.isDictionary())
        continue;

      if (isAlreadySubset(cidFont))
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

      auto ffOg = fontFile.getObjGen();
      fontFileHandles[ffOg] = fontFile;
      fontFileCID[ffOg] = true;

      try {
        std::set<uint16_t> glyphIds;

        auto cidToGid = cidFont.getKey("/CIDToGIDMap");
        if (cidToGid.isName() && cidToGid.getName() == "/Identity") {
          glyphIds = usedCodes;
        } else if (cidToGid.isStream()) {
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
          glyphIds = usedCodes;
        } else {
          continue;
        }

        glyphIds.insert(0);
        fontFileGlyphIds[ffOg].insert(glyphIds.begin(), glyphIds.end());
      } catch (...) {
        continue;
      }
      continue;
    }

    // simple TrueType fonts
    if (subtype.getName() != "/TrueType")
      continue;

    // skip already-subset fonts (BaseFont like "ABCDEF+FontName") — their
    // custom encodings and glyph tables can't be reliably re-subset
    // skip already-subset fonts — mark their font file as skip so other
    // fonts sharing it won't subset and corrupt the already-subset glyph data
    if (isAlreadySubset(fontObj)) {
      auto descriptor = fontObj.getKey("/FontDescriptor");
      if (descriptor.isDictionary() && descriptor.hasKey("/FontFile2")) {
        auto fontFile = descriptor.getKey("/FontFile2");
        if (fontFile.isStream())
          fontFileSkip.insert(fontFile.getObjGen());
      }
      continue;
    }

    auto descriptor = fontObj.getKey("/FontDescriptor");
    if (!descriptor.isDictionary() || !descriptor.hasKey("/FontFile2"))
      continue;

    auto fontFile = descriptor.getKey("/FontFile2");
    if (!fontFile.isStream())
      continue;

    auto ffOg = fontFile.getObjGen();
    fontFileHandles[ffOg] = fontFile;
    if (!fontFileCID.count(ffOg))
      fontFileCID[ffOg] = false;

    try {
      auto fontData = fontFile.getStreamData(qpdf_dl_all);
      const uint8_t *ttfData = fontData->getBuffer();
      size_t ttfSize = fontData->getSize();

      std::set<uint16_t> glyphIds;

      // build /Differences map to identify remapped codes
      auto diffMap = getDifferencesCodeMap(fontObj);
      std::set<uint16_t> diffCodes; // codes that have /Differences entries
      for (auto &[code, name] : diffMap) {
        if (usedCodes.count(code))
          diffCodes.insert(code);
      }
      std::set<uint16_t> baseCodes; // codes using base encoding (no remap)
      for (uint16_t code : usedCodes) {
        if (!diffCodes.count(code))
          baseCodes.insert(code);
      }

      bool hasToUnicode = false;

      // strategy 1: /ToUnicode CMap — most reliable Unicode mapping
      auto toUnicode = fontObj.getKey("/ToUnicode");
      if (toUnicode.isStream()) {
        hasToUnicode = true;
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

      // strategy 3: /Differences glyph names → Unicode → cmap lookup.
      // converts glyph names to Unicode codepoints (via AGL/uniXXXX)
      // and looks them up in the font's cmap. this catches characters like
      // ẞ (uni1E9E), € (Euro), • (bullet) that have non-standard byte codes
      // in /Differences but standard Unicode entries in the cmap.
      if (!diffCodes.empty()) {
        std::set<uint16_t> diffUnicodes;
        for (uint16_t code : diffCodes) {
          auto it = diffMap.find(code);
          if (it != diffMap.end()) {
            uint16_t unicode = glyphNameToUnicode(it->second);
            if (unicode > 0)
              diffUnicodes.insert(unicode);
          }
        }
        if (!diffUnicodes.empty()) {
          auto ids = mapCodesToGlyphIds(ttfData, ttfSize, diffUnicodes);
          glyphIds.insert(ids.begin(), ids.end());
        }
      }

      // strategy 4: base encoding conversion (WinAnsi, MacRoman) → cmap
      // only for codes NOT remapped by /Differences — those are already
      // handled by strategies 2 and 3 with correct Unicode
      if (!baseCodes.empty()) {
        auto unicodeCodes = convertCodesToUnicode(baseCodes, fontObj);
        auto encIds = mapCodesToGlyphIds(ttfData, ttfSize, unicodeCodes);
        glyphIds.insert(encIds.begin(), encIds.end());

        // strategy 5: raw character codes as fallback — only when byte codes
        // might directly index the cmap (no /ToUnicode remapping)
        if (!hasToUnicode && unicodeCodes != baseCodes) {
          auto rawGlyphs = mapCodesToGlyphIds(ttfData, ttfSize, baseCodes);
          glyphIds.insert(rawGlyphs.begin(), rawGlyphs.end());
        }

        // strategy 6: "uniXXXX" glyph names via post table — catches fonts
        // where glyphs have no cmap entry but are accessible by name
        {
          std::vector<std::string> uniNames;
          for (uint16_t u : unicodeCodes) {
            if (u > 0x7F) {
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
      }

      // safety check: if we found fewer glyph IDs than used character codes,
      // some characters couldn't be mapped — mark font file as unsafe to
      // subset. if ANY font sharing this file fails, skip the entire file.
      glyphIds.insert(0);
      if (glyphIds.size() - 1 < usedCodes.size()) {
        fontFileSkip.insert(ffOg);
        continue;
      }

      fontFileGlyphIds[ffOg].insert(glyphIds.begin(), glyphIds.end());
    } catch (...) {
      continue;
    }
  }

  // pass 2: subset each font file with merged glyph IDs from all font objects
  for (auto &[ffOg, glyphIds] : fontFileGlyphIds) {
    if (fontFileSkip.count(ffOg))
      continue;

    auto it = fontFileHandles.find(ffOg);
    if (it == fontFileHandles.end())
      continue;

    auto &fontFile = it->second;
    bool isCID = fontFileCID[ffOg];

    try {
      auto fontData = fontFile.getStreamData(qpdf_dl_all);
      const uint8_t *rawData = fontData->getBuffer();
      size_t rawSize = fontData->getSize();

      std::vector<uint8_t> subsetResult;
      if (!subsetFont(rawData, rawSize, glyphIds, subsetResult, !isCID))
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
  }

  // optimize CID font /W arrays — rebuild with only used CID entries
  for (auto &[og, usedCodes] : fontUsedCodes) {
    auto fontObj = qpdf.getObjectByObjGen(og);
    if (!fontObj.isDictionary())
      continue;

    auto subtype = fontObj.getKey("/Subtype");
    if (!subtype.isName() || subtype.getName() != "/Type0")
      continue;

    // skip already-subset Type0 fonts
    if (isAlreadySubset(fontObj))
      continue;

    auto descendants = fontObj.getKey("/DescendantFonts");
    if (!descendants.isArray() || descendants.getArrayNItems() < 1)
      continue;

    auto cidFont = descendants.getArrayItem(0);
    if (!cidFont.isDictionary())
      continue;

    // skip already-subset CID fonts — their width tables may have
    // custom CID mappings our usage collector doesn't fully capture
    if (isAlreadySubset(cidFont))
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
