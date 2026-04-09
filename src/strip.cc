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

  // remove document-level open action only if it is JavaScript
  if (root.hasKey("/OpenAction")) {
    auto action = root.getKey("/OpenAction");
    if (action.isDictionary()) {
      auto s = action.getKey("/S");
      if (s.isName() && s.getName() == "/JavaScript")
        root.removeKey("/OpenAction");
    }
  }

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
// Document overhead stripping — remove structural and navigational data
// that does not affect visual rendering
// ---------------------------------------------------------------------------

void stripDocumentOverhead(QPDF &qpdf) {
  auto root = qpdf.getRoot();

  // remove structure tree (tagged PDF accessibility info)
  if (root.hasKey("/StructTreeRoot"))
    root.removeKey("/StructTreeRoot");

  // remove output intents (PDF/A color management metadata)
  if (root.hasKey("/OutputIntents"))
    root.removeKey("/OutputIntents");

  // remove viewer preferences
  if (root.hasKey("/ViewerPreferences"))
    root.removeKey("/ViewerPreferences");

  // remove article threads
  if (root.hasKey("/Threads"))
    root.removeKey("/Threads");

  // remove web capture info
  if (root.hasKey("/SpiderInfo"))
    root.removeKey("/SpiderInfo");

  // remove page display mode (single page, outlines, etc.)
  if (root.hasKey("/PageMode"))
    root.removeKey("/PageMode");

  // remove page layout preference
  if (root.hasKey("/PageLayout"))
    root.removeKey("/PageLayout");

  // remove named destinations
  if (root.hasKey("/Dests"))
    root.removeKey("/Dests");

  // remove page-level structure keys and obsolete /ProcSet
  for (auto &page : QPDFPageDocumentHelper(qpdf).getAllPages()) {
    auto pageObj = page.getObjectHandle();

    // structure tree references
    if (pageObj.hasKey("/StructParents"))
      pageObj.removeKey("/StructParents");
    if (pageObj.hasKey("/StructParent"))
      pageObj.removeKey("/StructParent");

    // tab order (tied to structure tree)
    if (pageObj.hasKey("/Tabs"))
      pageObj.removeKey("/Tabs");

    // presentation steps
    if (pageObj.hasKey("/PresSteps"))
      pageObj.removeKey("/PresSteps");

    // remove /ProcSet from resources (obsolete since PDF 1.4)
    auto resources = pageObj.getKey("/Resources");
    if (resources.isDictionary() && resources.hasKey("/ProcSet"))
      resources.removeKey("/ProcSet");
  }

  // remove /Metadata from non-root objects (XObjects, fonts, etc.)
  // root /Metadata is handled separately by stripMetadata()
  for (auto &obj : qpdf.getAllObjects()) {
    if (!obj.isDictionary())
      continue;
    if (!obj.hasKey("/Metadata"))
      continue;
    // skip the document catalog itself
    if (obj.getObjGen() == root.getObjGen())
      continue;
    obj.removeKey("/Metadata");
  }
}

// ---------------------------------------------------------------------------
// Unused resource removal — remove XObject, ExtGState, ColorSpace, Pattern,
// Shading, and Properties entries not referenced by any content stream
// ---------------------------------------------------------------------------

class ResourceCollector : public QPDFObjectHandle::ParserCallbacks {
public:
  std::set<std::string> usedNames;

  void handleObject(QPDFObjectHandle obj) override {
    if (obj.isOperator()) {
      std::string op = obj.getOperatorValue();

      // Do (XObject), gs (ExtGState), sh (Shading)
      if ((op == "Do" || op == "gs" || op == "sh") && !operands.empty() &&
          operands.back().isName())
        usedNames.insert(operands.back().getName());

      // cs/CS (set color space)
      if ((op == "cs" || op == "CS") && !operands.empty() &&
          operands.back().isName())
        usedNames.insert(operands.back().getName());

      // scn/SCN (set color with pattern)
      if (op == "scn" || op == "SCN") {
        for (auto &operand : operands) {
          if (operand.isName())
            usedNames.insert(operand.getName());
        }
      }

      // Tf (font — for completeness, though fonts are handled separately)
      if (op == "Tf" && operands.size() >= 2 &&
          operands[operands.size() - 2].isName())
        usedNames.insert(operands[operands.size() - 2].getName());

      // BDC (marked content with properties)
      if (op == "BDC") {
        for (auto &operand : operands) {
          if (operand.isName())
            usedNames.insert(operand.getName());
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
};

void removeUnusedResources(QPDF &qpdf) {
  for (auto &page : QPDFPageDocumentHelper(qpdf).getAllPages()) {
    auto pageObj = page.getObjectHandle();
    auto resources = pageObj.getKey("/Resources");
    if (!resources.isDictionary())
      continue;

    // collect all resource names used in content streams
    ResourceCollector collector;

    // scan page content
    try {
      auto contents = pageObj.getKey("/Contents");
      QPDFObjectHandle::parseContentStream(contents, &collector);
    } catch (...) {
      continue;
    }

    // scan Form XObjects
    auto xobjects = resources.getKey("/XObject");
    if (xobjects.isDictionary()) {
      for (auto &key : xobjects.getKeys()) {
        auto xobj = xobjects.getKey(key);
        if (!xobj.isStream())
          continue;
        auto xobjDict = xobj.getDict();
        auto st = xobjDict.getKey("/Subtype");
        if (!st.isName() || st.getName() != "/Form")
          continue;
        try {
          QPDFObjectHandle::parseContentStream(xobj, &collector);
        } catch (...) {
        }
      }
    }

    // remove unused entries from resource categories
    static const char *categories[] = {"/ExtGState", "/ColorSpace", "/Pattern",
                                       "/Shading", "/Properties"};

    for (auto category : categories) {
      auto dict = resources.getKey(category);
      if (!dict.isDictionary())
        continue;

      auto keys = dict.getKeys();
      for (auto &key : keys) {
        if (collector.usedNames.find(key) == collector.usedNames.end())
          dict.removeKey(key);
      }

      if (dict.getKeys().empty())
        resources.removeKey(category);
    }

    // remove unused XObjects
    if (xobjects.isDictionary()) {
      auto keys = xobjects.getKeys();
      for (auto &key : keys) {
        if (collector.usedNames.find(key) == collector.usedNames.end())
          xobjects.removeKey(key);
      }
      if (xobjects.getKeys().empty())
        resources.removeKey("/XObject");
    }
  }
}
