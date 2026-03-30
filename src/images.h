#pragma once

#include <qpdf/QPDF.hh>
#include <qpdf/QPDFObjectHandle.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>

// iterates all image XObjects across all pages
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
      fn(key, xobj, xobjects);
    }
  }
}

void optimizeImages(QPDF &qpdf, int quality);
void deduplicateImages(QPDF &qpdf);
void optimizeExistingJpegs(QPDF &qpdf);
