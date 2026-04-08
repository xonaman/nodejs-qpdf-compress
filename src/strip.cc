#include "strip.h"
#include "images.h"

#include <set>
#include <string>

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
