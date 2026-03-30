#pragma once

#include <qpdf/QPDF.hh>
#include <qpdf/QPDFObjectHandle.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>
#include <qpdf/QPDFPageObjectHelper.hh>

// iterates all image XObjects across all pages, providing the page helper for
// context (e.g. MediaBox for DPI calculations)
template <typename Fn> void forEachImage(QPDF &qpdf, Fn &&fn) {
  for (auto &page : QPDFPageDocumentHelper(qpdf).getAllPages()) {
    auto resources = page.getObjectHandle().getKey("/Resources");
    if (!resources.isDictionary())
      continue;
    auto xobjects = resources.getKey("/XObject");
    if (!xobjects.isDictionary())
      continue;
    for (auto &key : xobjects.getKeys()) {
      auto xobj = xobjects.getKey(key);
      if (!xobj.isStream())
        continue;
      auto dict = xobj.getDict();
      if (!dict.getKey("/Subtype").isName() ||
          dict.getKey("/Subtype").getName() != "/Image")
        continue;
      fn(key, xobj, xobjects, page);
    }
  }
}

struct CompressOptions {
  int skipThreshold = 0; // skip existing JPEGs at or below this quality
  int targetQuality = 0; // encode/re-encode at this quality
};

void optimizeImages(QPDF &qpdf, const CompressOptions &opts);
void downscaleImages(QPDF &qpdf, int maxDpi);
void deduplicateImages(QPDF &qpdf);
void optimizeExistingJpegs(QPDF &qpdf);
void stripMetadata(QPDF &qpdf);
void removeUnusedFonts(QPDF &qpdf);
