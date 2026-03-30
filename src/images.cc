#include "images.h"
#include "jpeg.h"

#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <unordered_map>

#include <qpdf/Buffer.hh>

// ---------------------------------------------------------------------------
// Image recompression for lossy mode
// ---------------------------------------------------------------------------

void optimizeImages(QPDF &qpdf, int quality) {
  forEachImage(
      qpdf, [&](const std::string &, QPDFObjectHandle xobj, QPDFObjectHandle) {
        auto dict = xobj.getDict();

        // only handle 8-bit images
        if (!dict.getKey("/BitsPerComponent").isInteger() ||
            dict.getKey("/BitsPerComponent").getIntValue() != 8)
          return;

        int width = 0, height = 0, components = 0;
        if (dict.getKey("/Width").isInteger())
          width = static_cast<int>(dict.getKey("/Width").getIntValue());
        if (dict.getKey("/Height").isInteger())
          height = static_cast<int>(dict.getKey("/Height").getIntValue());

        if (width <= 0 || height <= 0 || width > 16384 || height > 16384)
          return;

        // determine color components
        auto cs = dict.getKey("/ColorSpace");
        if (cs.isName()) {
          if (cs.getName() == "/DeviceRGB")
            components = 3;
          else if (cs.getName() == "/DeviceGray")
            components = 1;
          else
            return; // skip CMYK, Lab, etc.
        } else {
          return; // skip indexed, ICCBased, etc.
        }

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

        auto currentFilter = dict.getKey("/Filter");
        bool isCurrentlyJpeg =
            currentFilter.isName() && currentFilter.getName() == "/DCTDecode";

        // encode as JPEG via libjpeg-turbo
        std::vector<uint8_t> jpegData;
        if (!encodeJpeg(streamData->getBuffer(), width, height, components,
                        quality, jpegData))
          return;

        // only replace if we actually reduced size
        if (isCurrentlyJpeg) {
          auto rawData = xobj.getRawStreamData();
          if (jpegData.size() >= rawData->getSize())
            return;
        }

        // replace stream data with JPEG
        std::string jpegStr(reinterpret_cast<char *>(jpegData.data()),
                            jpegData.size());
        xobj.replaceStreamData(jpegStr, QPDFObjectHandle::newName("/DCTDecode"),
                               QPDFObjectHandle::newNull());

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
                         QPDFObjectHandle /*xobjects*/) {
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
  forEachImage(qpdf, [&](const std::string &key, QPDFObjectHandle xobj,
                         QPDFObjectHandle xobjects) {
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
                         QPDFObjectHandle /*xobjects*/) {
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
