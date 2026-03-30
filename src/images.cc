#include "images.h"
#include "jpeg.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <unordered_map>
#include <vector>

#include <qpdf/Buffer.hh>
#include <qpdf/QPDFObjectHandle.hh>

// ---------------------------------------------------------------------------
// Color space helpers
// ---------------------------------------------------------------------------

// resolves /ColorSpace to component count and whether it's CMYK.
// returns 0 on unsupported color spaces.
static int resolveColorSpace(QPDFObjectHandle cs, bool &isCMYK) {
  isCMYK = false;

  if (cs.isName()) {
    auto name = cs.getName();
    if (name == "/DeviceRGB")
      return 3;
    if (name == "/DeviceGray")
      return 1;
    if (name == "/DeviceCMYK") {
      isCMYK = true;
      return 4;
    }
    return 0;
  }

  // ICCBased: [/ICCBased <stream>] — get /N from the ICC profile stream dict
  if (cs.isArray() && cs.getArrayNItems() >= 2) {
    auto csName = cs.getArrayItem(0);
    if (csName.isName() && csName.getName() == "/ICCBased") {
      auto profile = cs.getArrayItem(1);
      if (profile.isStream()) {
        auto n = profile.getDict().getKey("/N");
        if (n.isInteger()) {
          int components = static_cast<int>(n.getIntValue());
          if (components == 4)
            isCMYK = true;
          if (components == 1 || components == 3 || components == 4)
            return components;
        }
      }
    }
  }

  return 0;
}

// naive CMYK → RGB conversion (without ICC profile).
// uses the standard formula: R = 255 * (1-C) * (1-K), etc.
static void cmykToRgb(const unsigned char *cmyk, unsigned char *rgb,
                      size_t pixelCount) {
  for (size_t i = 0; i < pixelCount; ++i) {
    double c = cmyk[i * 4 + 0] / 255.0;
    double m = cmyk[i * 4 + 1] / 255.0;
    double y = cmyk[i * 4 + 2] / 255.0;
    double k = cmyk[i * 4 + 3] / 255.0;
    rgb[i * 3 + 0] =
        static_cast<unsigned char>(255.0 * (1.0 - c) * (1.0 - k) + 0.5);
    rgb[i * 3 + 1] =
        static_cast<unsigned char>(255.0 * (1.0 - m) * (1.0 - k) + 0.5);
    rgb[i * 3 + 2] =
        static_cast<unsigned char>(255.0 * (1.0 - y) * (1.0 - k) + 0.5);
  }
}

// ---------------------------------------------------------------------------
// Bilinear downscaling
// ---------------------------------------------------------------------------

static std::vector<uint8_t> bilinearDownscale(const unsigned char *src,
                                              int srcW, int srcH,
                                              int components, int dstW,
                                              int dstH) {
  std::vector<uint8_t> dst(static_cast<size_t>(dstW) * dstH * components);

  double xRatio = static_cast<double>(srcW) / dstW;
  double yRatio = static_cast<double>(srcH) / dstH;

  for (int y = 0; y < dstH; ++y) {
    double srcY = y * yRatio;
    int y0 = static_cast<int>(srcY);
    int y1 = std::min(y0 + 1, srcH - 1);
    double fy = srcY - y0;

    for (int x = 0; x < dstW; ++x) {
      double srcX = x * xRatio;
      int x0 = static_cast<int>(srcX);
      int x1 = std::min(x0 + 1, srcW - 1);
      double fx = srcX - x0;

      for (int c = 0; c < components; ++c) {
        double v00 = src[(y0 * srcW + x0) * components + c];
        double v10 = src[(y0 * srcW + x1) * components + c];
        double v01 = src[(y1 * srcW + x0) * components + c];
        double v11 = src[(y1 * srcW + x1) * components + c];

        double val = v00 * (1 - fx) * (1 - fy) + v10 * fx * (1 - fy) +
                     v01 * (1 - fx) * fy + v11 * fx * fy;
        dst[(y * dstW + x) * components + c] =
            static_cast<uint8_t>(std::min(std::max(val + 0.5, 0.0), 255.0));
      }
    }
  }

  return dst;
}

// ---------------------------------------------------------------------------
// Image recompression for lossy mode
// ---------------------------------------------------------------------------

// Image recompression thresholds are passed via CompressOptions:
//   - lossless mode: skipThreshold=90, targetQuality=85  (conservative)
//   - lossy mode:    skipThreshold=65, targetQuality=75  (aggressive)

void optimizeImages(QPDF &qpdf, const CompressOptions &opts) {
  forEachImage(qpdf, [&](const std::string &, QPDFObjectHandle xobj,
                         QPDFObjectHandle, QPDFPageObjectHelper &) {
    auto dict = xobj.getDict();

    // only handle 8-bit images
    if (!dict.getKey("/BitsPerComponent").isInteger() ||
        dict.getKey("/BitsPerComponent").getIntValue() != 8)
      return;

    int width = 0, height = 0;
    if (dict.getKey("/Width").isInteger())
      width = static_cast<int>(dict.getKey("/Width").getIntValue());
    if (dict.getKey("/Height").isInteger())
      height = static_cast<int>(dict.getKey("/Height").getIntValue());

    if (width <= 0 || height <= 0 || width > 16384 || height > 16384)
      return;

    // determine color components via color space resolution
    bool isCMYK = false;
    int components = resolveColorSpace(dict.getKey("/ColorSpace"), isCMYK);
    if (components == 0)
      return;

    // skip tiny images (logos, icons)
    if (width * height < 2500)
      return;

    // get fully decoded stream data (raw pixels)
    std::shared_ptr<Buffer> streamData;
    try {
      streamData = xobj.getStreamData(qpdf_dl_all);
    } catch (...) {
      return;
    }

    // overflow-safe size calculation
    auto w = static_cast<size_t>(width);
    auto h = static_cast<size_t>(height);
    auto c = static_cast<size_t>(components);
    if (h > 0 && w > std::numeric_limits<size_t>::max() / h)
      return;
    if (c > 0 && (w * h) > std::numeric_limits<size_t>::max() / c)
      return;
    size_t expectedSize = w * h * c;
    if (streamData->getSize() != expectedSize)
      return;

    // convert CMYK → RGB for JPEG encoding (JPEG doesn't support CMYK
    // natively in most decoders)
    const unsigned char *pixels = streamData->getBuffer();
    std::vector<uint8_t> rgbBuf;
    int encodeComponents = components;
    if (isCMYK) {
      size_t pixelCount = w * h;
      rgbBuf.resize(pixelCount * 3);
      cmykToRgb(pixels, rgbBuf.data(), pixelCount);
      pixels = rgbBuf.data();
      encodeComponents = 3;
    }

    auto currentFilter = dict.getKey("/Filter");
    bool isCurrentlyJpeg =
        currentFilter.isName() && currentFilter.getName() == "/DCTDecode";

    // skip existing JPEGs that are already at or below the threshold —
    // re-encoding them adds generation loss for negligible savings
    if (isCurrentlyJpeg && !isCMYK) {
      auto rawData = xobj.getRawStreamData();
      int existingQ =
          estimateJpegQuality(rawData->getBuffer(), rawData->getSize());
      if (existingQ > 0 && existingQ <= opts.skipThreshold)
        return;
    }

    // encode as JPEG via libjpeg-turbo
    std::vector<uint8_t> jpegData;
    if (!encodeJpeg(pixels, width, height, encodeComponents, opts.targetQuality,
                    jpegData))
      return;

    // only replace if we actually reduced size
    auto rawData = xobj.getRawStreamData();
    if (jpegData.size() >= rawData->getSize())
      return;

    // replace stream data with JPEG
    std::string jpegStr(reinterpret_cast<char *>(jpegData.data()),
                        jpegData.size());
    xobj.replaceStreamData(jpegStr, QPDFObjectHandle::newName("/DCTDecode"),
                           QPDFObjectHandle::newNull());

    // update color space to DeviceRGB if converted from CMYK/ICCBased
    if (isCMYK || !dict.getKey("/ColorSpace").isName() ||
        dict.getKey("/ColorSpace").getName() != "/DeviceRGB") {
      if (encodeComponents == 3)
        dict.replaceKey("/ColorSpace", QPDFObjectHandle::newName("/DeviceRGB"));
      else if (encodeComponents == 1)
        dict.replaceKey("/ColorSpace",
                        QPDFObjectHandle::newName("/DeviceGray"));
    }

    // remove FlateDecode-specific params
    if (dict.hasKey("/DecodeParms"))
      dict.removeKey("/DecodeParms");
    if (dict.hasKey("/Predictor"))
      dict.removeKey("/Predictor");
  });
}

// ---------------------------------------------------------------------------
// Duplicate image detection
// ---------------------------------------------------------------------------

void deduplicateImages(QPDF &qpdf) {
  struct ImageEntry {
    QPDFObjGen og;
    size_t dataSize;
    QPDFObjectHandle handle;
  };

  std::unordered_map<uint64_t, std::vector<ImageEntry>> hashGroups;
  std::set<QPDFObjGen> seen;

  // first pass: collect image objects and hash their raw data
  forEachImage(qpdf, [&](const std::string & /*key*/, QPDFObjectHandle xobj,
                         QPDFObjectHandle /*xobjects*/,
                         QPDFPageObjectHelper & /*page*/) {
    auto og = xobj.getObjGen();
    if (seen.count(og))
      return;
    seen.insert(og);

    try {
      auto rawData = xobj.getRawStreamData();
      size_t size = rawData->getSize();

      // FNV-1a hash
      uint64_t hash = 14695981039346656037ULL;
      auto *p = rawData->getBuffer();
      for (size_t i = 0; i < size; ++i) {
        hash ^= static_cast<uint64_t>(p[i]);
        hash *= 1099511628211ULL;
      }

      hashGroups[hash].push_back({og, size, xobj});
    } catch (...) {
    }
  });

  // second pass: verify hash collisions with full byte comparison
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

        if (memcmp(rawI->getBuffer(), rawJ->getBuffer(), rawI->getSize()) == 0)
          replacements[group[j].og] = group[i].handle;
      }
    }
  }

  if (replacements.empty())
    return;

  // third pass: rewrite XObject references to point to canonical objects
  forEachImage(qpdf,
               [&](const std::string &key, QPDFObjectHandle xobj,
                   QPDFObjectHandle xobjects, QPDFPageObjectHelper & /*page*/) {
                 auto it = replacements.find(xobj.getObjGen());
                 if (it != replacements.end())
                   xobjects.replaceKey(key, it->second);
               });
}

// ---------------------------------------------------------------------------
// Lossless optimization of existing embedded JPEG images
// ---------------------------------------------------------------------------

void optimizeExistingJpegs(QPDF &qpdf) {
  std::set<QPDFObjGen> processed;

  forEachImage(qpdf, [&](const std::string & /*key*/, QPDFObjectHandle xobj,
                         QPDFObjectHandle /*xobjects*/,
                         QPDFPageObjectHelper & /*page*/) {
    auto og = xobj.getObjGen();
    if (processed.count(og))
      return;
    processed.insert(og);

    auto filter = xobj.getDict().getKey("/Filter");
    if (!filter.isName() || filter.getName() != "/DCTDecode")
      return;

    try {
      auto rawData = xobj.getRawStreamData();

      std::vector<uint8_t> optimized;
      if (!losslessJpegOptimize(rawData->getBuffer(), rawData->getSize(),
                                optimized))
        return;

      // only replace if strictly smaller
      if (optimized.size() >= rawData->getSize())
        return;

      std::string jpegStr(reinterpret_cast<char *>(optimized.data()),
                          optimized.size());
      xobj.replaceStreamData(jpegStr, QPDFObjectHandle::newName("/DCTDecode"),
                             QPDFObjectHandle::newNull());
    } catch (...) {
    }
  });
}

// ---------------------------------------------------------------------------
// DPI-based image downscaling
// ---------------------------------------------------------------------------

void downscaleImages(QPDF &qpdf, int maxDpi, int quality) {
  if (maxDpi <= 0)
    return;

  std::set<QPDFObjGen> processed;

  forEachImage(qpdf, [&](const std::string & /*key*/, QPDFObjectHandle xobj,
                         QPDFObjectHandle /*xobjects*/,
                         QPDFPageObjectHelper &page) {
    auto og = xobj.getObjGen();
    if (processed.count(og))
      return;
    processed.insert(og);

    auto dict = xobj.getDict();

    if (!dict.getKey("/BitsPerComponent").isInteger() ||
        dict.getKey("/BitsPerComponent").getIntValue() != 8)
      return;

    int imgW = 0, imgH = 0;
    if (dict.getKey("/Width").isInteger())
      imgW = static_cast<int>(dict.getKey("/Width").getIntValue());
    if (dict.getKey("/Height").isInteger())
      imgH = static_cast<int>(dict.getKey("/Height").getIntValue());

    if (imgW <= 0 || imgH <= 0)
      return;

    bool isCMYK = false;
    int components = resolveColorSpace(dict.getKey("/ColorSpace"), isCMYK);
    if (components == 0)
      return;

    // get page dimensions from MediaBox (in points, 72 per inch)
    auto mediaBox = page.getAttribute("/MediaBox", false);
    if (!mediaBox.isArray() || mediaBox.getArrayNItems() < 4)
      return;

    double pageW = 0, pageH = 0;
    try {
      pageW = mediaBox.getArrayItem(2).getNumericValue() -
              mediaBox.getArrayItem(0).getNumericValue();
      pageH = mediaBox.getArrayItem(3).getNumericValue() -
              mediaBox.getArrayItem(1).getNumericValue();
    } catch (...) {
      return;
    }

    if (pageW <= 0 || pageH <= 0)
      return;

    // estimate effective DPI (assumes image fills page — conservative)
    double dpiX = imgW / (pageW / 72.0);
    double dpiY = imgH / (pageH / 72.0);
    double effectiveDpi = std::max(dpiX, dpiY);

    if (effectiveDpi <= maxDpi)
      return;

    // calculate target dimensions
    double scale = static_cast<double>(maxDpi) / effectiveDpi;
    int newW = std::max(1, static_cast<int>(imgW * scale + 0.5));
    int newH = std::max(1, static_cast<int>(imgH * scale + 0.5));

    // not worth downscaling if the reduction is minimal
    if (newW >= imgW - 1 && newH >= imgH - 1)
      return;

    // decode pixels
    std::shared_ptr<Buffer> streamData;
    try {
      streamData = xobj.getStreamData(qpdf_dl_all);
    } catch (...) {
      return;
    }

    auto w = static_cast<size_t>(imgW);
    auto h = static_cast<size_t>(imgH);
    auto c = static_cast<size_t>(components);
    if (h > 0 && w > std::numeric_limits<size_t>::max() / h)
      return;
    if (c > 0 && (w * h) > std::numeric_limits<size_t>::max() / c)
      return;
    if (streamData->getSize() != w * h * c)
      return;

    const unsigned char *pixels = streamData->getBuffer();
    int downscaleComponents = components;

    // convert CMYK → RGB before downscaling
    std::vector<uint8_t> rgbBuf;
    if (isCMYK) {
      size_t pixelCount = w * h;
      rgbBuf.resize(pixelCount * 3);
      cmykToRgb(pixels, rgbBuf.data(), pixelCount);
      pixels = rgbBuf.data();
      downscaleComponents = 3;
    }

    auto scaled =
        bilinearDownscale(pixels, imgW, imgH, downscaleComponents, newW, newH);

    // re-encode downscaled pixels as JPEG
    std::vector<uint8_t> jpegData;
    if (!encodeJpeg(scaled.data(), newW, newH, downscaleComponents, quality,
                    jpegData))
      return;

    // only replace if the JPEG is actually smaller than the original stream
    auto rawData = xobj.getRawStreamData();
    if (jpegData.size() >= rawData->getSize())
      return;

    std::string jpegStr(reinterpret_cast<char *>(jpegData.data()),
                        jpegData.size());
    xobj.replaceStreamData(jpegStr, QPDFObjectHandle::newName("/DCTDecode"),
                           QPDFObjectHandle::newNull());

    dict.replaceKey("/Width", QPDFObjectHandle::newInteger(newW));
    dict.replaceKey("/Height", QPDFObjectHandle::newInteger(newH));

    if (isCMYK) {
      dict.replaceKey("/ColorSpace", QPDFObjectHandle::newName("/DeviceRGB"));
    }

    // remove predictor params from previous encoding
    if (dict.hasKey("/DecodeParms"))
      dict.removeKey("/DecodeParms");
    if (dict.hasKey("/Predictor"))
      dict.removeKey("/Predictor");
  });
}

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
