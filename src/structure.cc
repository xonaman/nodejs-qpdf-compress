#include "structure.h"
#include "hash_utils.h"
#include "images.h"

#include <algorithm>
#include <cmath>
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

      uint64_t hash = fnv1aHash(rawData->getBuffer(), size);

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
    std::string allSnippets;
    for (int i = 0; i < annots.getArrayNItems(); ++i) {
      auto annot = annots.getArrayItem(i);
      if (!annot.isDictionary())
        continue;

      auto subtype = annot.getKey("/Subtype");
      if (!subtype.isName() || subtype.getName() != "/Widget")
        continue;

      // skip hidden, invisible, or no-view annotations
      if (annot.hasKey("/F")) {
        auto flags = annot.getKey("/F");
        if (flags.isInteger()) {
          int f = static_cast<int>(flags.getIntValue());
          // bit 1 = Invisible, bit 2 = Hidden, bit 6 (32) = NoView
          if (f & (1 | 2 | 32))
            continue;
        }
      }

      // check if there's a normal appearance to flatten
      // skip signature fields — their appearance should not be flattened
      if (annot.hasKey("/FT")) {
        auto ft = annot.getKey("/FT");
        if (ft.isName() && ft.getName() == "/Sig")
          continue;
      }

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
        double rx1 = rect.getArrayItem(0).getNumericValue();
        double ry1 = rect.getArrayItem(1).getNumericValue();
        double rx2 = rect.getArrayItem(2).getNumericValue();
        double ry2 = rect.getArrayItem(3).getNumericValue();

        // normalize rect (some generators produce inverted coordinates)
        if (rx1 > rx2)
          std::swap(rx1, rx2);
        if (ry1 > ry2)
          std::swap(ry1, ry2);

        double rw = rx2 - rx1;
        double rh = ry2 - ry1;
        if (rw <= 0 || rh <= 0)
          continue;

        // get appearance stream bounding box and matrix
        auto apDict = nAppearance.getDict();
        double bx1 = 0, by1 = 0, bx2 = 1, by2 = 1;
        if (apDict.hasKey("/BBox")) {
          auto bbox = apDict.getKey("/BBox");
          if (bbox.isArray() && bbox.getArrayNItems() >= 4) {
            bx1 = bbox.getArrayItem(0).getNumericValue();
            by1 = bbox.getArrayItem(1).getNumericValue();
            bx2 = bbox.getArrayItem(2).getNumericValue();
            by2 = bbox.getArrayItem(3).getNumericValue();
          }
        }

        double bw = bx2 - bx1;
        double bh = by2 - by1;
        if (std::abs(bw) < 1e-10 || std::abs(bh) < 1e-10)
          continue;

        // BBox→Rect mapping: scale then translate
        double sx = rw / bw;
        double sy = rh / bh;
        double stx = rx1 - bx1 * sx;
        double sty = ry1 - by1 * sy;
        // S = [sx, 0, 0, sy, stx, sty]

        // read the form's /Matrix (default identity). the Do operator
        // applies this matrix, so our CTM must compensate:
        // rendered = FormMatrix × OurCTM, we want rendered = S,
        // so OurCTM = FormMatrix⁻¹ × S.
        double ma = 1, mb = 0, mc = 0, md = 1, me = 0, mf = 0;
        if (apDict.hasKey("/Matrix")) {
          auto matrix = apDict.getKey("/Matrix");
          if (matrix.isArray() && matrix.getArrayNItems() >= 6) {
            ma = matrix.getArrayItem(0).getNumericValue();
            mb = matrix.getArrayItem(1).getNumericValue();
            mc = matrix.getArrayItem(2).getNumericValue();
            md = matrix.getArrayItem(3).getNumericValue();
            me = matrix.getArrayItem(4).getNumericValue();
            mf = matrix.getArrayItem(5).getNumericValue();
          }
        }

        // compute CTM = Matrix⁻¹ × S
        // PDF matrix [a b c d e f] represents:
        //   | a  b  0 |
        //   | c  d  0 |
        //   | e  f  1 |
        double ca, cb, cc, cd, ce, cf;
        double det = ma * md - mb * mc;
        if (std::abs(det) < 1e-10) {
          // degenerate matrix — use BBox→Rect directly
          ca = sx;
          cb = 0;
          cc = 0;
          cd = sy;
          ce = stx;
          cf = sty;
        } else {
          // inverse of Matrix
          double ia = md / det;
          double ib = -mb / det;
          double ic = -mc / det;
          double id = ma / det;
          double ie = (mc * mf - md * me) / det;
          double iif = (mb * me - ma * mf) / det;

          // multiply M⁻¹ × S (M⁻¹ is left, S is right):
          // [ia, ib, ic, id, ie, iif] × [sx, 0, 0, sy, stx, sty]
          ca = ia * sx;
          cb = ib * sy;
          cc = ic * sx;
          cd = id * sy;
          ce = ie * sx + stx;
          cf = iif * sy + sty;
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

        // build content stream snippet with the full affine CTM
        // use snprintf for locale-safe decimal formatting
        char buf[384];
        std::snprintf(buf, sizeof(buf),
                      "q %.6g %.6g %.6g %.6g %.6g %.6g cm %s Do Q\n", ca, cb,
                      cc, cd, ce, cf, xobjName.c_str());
        allSnippets += buf;

        widgetIndices.push_back(i);
      } catch (...) {
        continue;
      }
    }

    if (!widgetIndices.empty()) {
      // wrap existing page content in q/Q so the flattened form snippets
      // run with the default (identity) CTM, regardless of any active
      // transforms in the page content (e.g. top-down Y-flip)
      page.addPageContents(QPDFObjectHandle::newStream(&qpdf, "q\n"), true);
      page.addPageContents(
          QPDFObjectHandle::newStream(&qpdf, "Q\n" + allSnippets), false);

      // remove widget annotations (reverse order to preserve indices)
      for (auto it = widgetIndices.rbegin(); it != widgetIndices.rend(); ++it)
        annots.eraseItem(*it);
    }
  }

  // remove the /AcroForm dictionary
  root.removeKey("/AcroForm");
}

// ---------------------------------------------------------------------------
// Page tree flattening — push inherited attributes to pages so QPDFWriter
// can generate a flat single-level page tree
// ---------------------------------------------------------------------------

void flattenPageTree(QPDF &qpdf) { qpdf.pushInheritedAttributesToPage(); }
