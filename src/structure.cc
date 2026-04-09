#include "structure.h"
#include "images.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include <qpdf/QPDFObjectHandle.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>
#include <qpdf/QPDFPageObjectHelper.hh>

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
        // use snprintf instead of std::to_string to avoid locale-dependent
        // decimal separators (e.g. comma instead of period)
        char buf[256];
        std::snprintf(buf, sizeof(buf),
                      "q %.6g 0 0 %.6g %.6g %.6g cm %s Do Q\n", scaleX, scaleY,
                      x1, y1, xobjName.c_str());
        std::string snippet(buf);

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
