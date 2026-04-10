#include "font_subset.h"

#include <cstring>
#include <string>
#include <vector>

#include <hb-subset.h>
#include <hb.h>

// ---------------------------------------------------------------------------
// Glyph lookup using HarfBuzz — replaces manual cmap parsing
// ---------------------------------------------------------------------------

// helper: create an hb_face from raw font data (HarfBuzz owns a copy)
static hb_face_t *createFace(const uint8_t *data, size_t size) {
  hb_blob_t *blob = hb_blob_create(reinterpret_cast<const char *>(data), size,
                                   HB_MEMORY_MODE_DUPLICATE, nullptr, nullptr);
  hb_face_t *face = hb_face_create(blob, 0);
  hb_blob_destroy(blob);
  return face;
}

std::set<uint16_t> mapCodesToGlyphIds(const uint8_t *data, size_t size,
                                      const std::set<uint16_t> &charCodes) {
  std::set<uint16_t> glyphIds;
  glyphIds.insert(0); // always keep .notdef

  hb_face_t *face = createFace(data, size);
  if (hb_face_get_glyph_count(face) == 0) {
    hb_face_destroy(face);
    return glyphIds;
  }

  // collect Unicode → GID mapping from ALL cmap subtables (not just the
  // preferred one). hb_font_get_nominal_glyph only checks the preferred
  // subtable and can miss glyphs in fonts with multiple cmap encodings.
  hb_map_t *mapping = hb_map_create();
  hb_set_t *unicodes = hb_set_create();
  hb_face_collect_nominal_glyph_mapping(face, mapping, unicodes);

  for (uint16_t code : charCodes) {
    hb_codepoint_t glyph = hb_map_get(mapping, code);
    if (glyph != HB_MAP_VALUE_INVALID && glyph != 0)
      glyphIds.insert(static_cast<uint16_t>(glyph & 0xFFFF));

    // try 0xF000 offset for symbol fonts (Windows Symbol encoding)
    if ((glyph == HB_MAP_VALUE_INVALID || glyph == 0) && code < 256) {
      glyph = hb_map_get(mapping, code + 0xF000);
      if (glyph != HB_MAP_VALUE_INVALID && glyph != 0)
        glyphIds.insert(static_cast<uint16_t>(glyph & 0xFFFF));
    }
  }

  hb_set_destroy(unicodes);
  hb_map_destroy(mapping);
  hb_face_destroy(face);
  return glyphIds;
}

std::set<uint16_t>
mapGlyphNamesToGlyphIds(const uint8_t *data, size_t size,
                        const std::vector<std::string> &names) {
  std::set<uint16_t> glyphIds;
  glyphIds.insert(0); // always keep .notdef

  hb_face_t *face = createFace(data, size);
  if (hb_face_get_glyph_count(face) == 0) {
    hb_face_destroy(face);
    return glyphIds;
  }

  hb_font_t *font = hb_font_create(face);

  for (const auto &name : names) {
    hb_codepoint_t glyph = 0;
    if (hb_font_get_glyph_from_name(font, name.c_str(),
                                    static_cast<int>(name.size()), &glyph) &&
        glyph != 0)
      glyphIds.insert(static_cast<uint16_t>(glyph & 0xFFFF));
  }

  hb_font_destroy(font);
  hb_face_destroy(face);
  return glyphIds;
}

// ---------------------------------------------------------------------------
// Font subsetting using HarfBuzz hb-subset (TrueType and CFF/OpenType)
// ---------------------------------------------------------------------------

static uint32_t readU32(const uint8_t *p) {
  return (static_cast<uint32_t>(p[0]) << 24) |
         (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) | p[3];
}

bool subsetFont(const uint8_t *data, size_t size,
                const std::set<uint16_t> &usedGlyphIds,
                std::vector<uint8_t> &output, bool preserveCmap) {
  // validate font header — accept TrueType (0x00010000, 'true') and
  // OpenType-CFF ('OTTO')
  if (size < 12)
    return false;
  uint32_t sfVersion = readU32(data);
  if (sfVersion != 0x00010000 && sfVersion != 0x74727565 &&
      sfVersion != 0x4F54544F) // OTTO
    return false;

  // let HarfBuzz own a copy of the font data to avoid any lifetime issues
  hb_blob_t *blob = hb_blob_create(reinterpret_cast<const char *>(data), size,
                                   HB_MEMORY_MODE_DUPLICATE, nullptr, nullptr);
  hb_face_t *face = hb_face_create(blob, 0);
  hb_blob_destroy(blob);

  if (hb_face_get_glyph_count(face) == 0) {
    hb_face_destroy(face);
    return false;
  }

  hb_subset_input_t *input = hb_subset_input_create_or_fail();
  if (!input) {
    hb_face_destroy(face);
    return false;
  }

  // retain original glyph IDs so all existing PDF text references remain
  // valid — unused glyph slots are zeroed out
  hb_subset_input_set_flags(input, HB_SUBSET_FLAGS_RETAIN_GIDS);

  // drop font tables unnecessary for PDF rendering
  hb_set_t *dropTables =
      hb_subset_input_set(input, HB_SUBSET_SETS_DROP_TABLE_TAG);
  hb_set_add(dropTables, HB_TAG('G', 'P', 'O', 'S')); // OpenType positioning
  hb_set_add(dropTables, HB_TAG('G', 'S', 'U', 'B')); // OpenType substitution
  hb_set_add(dropTables, HB_TAG('G', 'D', 'E', 'F')); // OpenType definitions
  hb_set_add(dropTables, HB_TAG('D', 'S', 'I', 'G')); // digital signature
  hb_set_add(dropTables, HB_TAG('k', 'e', 'r', 'n')); // legacy kerning
  hb_set_add(dropTables, HB_TAG('n', 'a', 'm', 'e')); // font naming strings
  hb_set_add(dropTables, HB_TAG('g', 'a', 's', 'p')); // grid-fitting hints
  hb_set_add(dropTables,
             HB_TAG('h', 'd', 'm', 'x')); // horizontal device metrics
  hb_set_add(dropTables, HB_TAG('L', 'T', 'S', 'H')); // linear threshold
  hb_set_add(dropTables, HB_TAG('V', 'D', 'M', 'X')); // device metrics

  // preserve the cmap table unchanged for simple TrueType fonts —
  // PDF viewers use it to resolve character codes to glyph IDs.
  // for CID fonts, PDF viewers use /CIDToGIDMap instead, so the cmap
  // can be freely subset.
  if (preserveCmap) {
    hb_set_t *noSubsetTables =
        hb_subset_input_set(input, HB_SUBSET_SETS_NO_SUBSET_TABLE_TAG);
    hb_set_add(noSubsetTables, HB_TAG('c', 'm', 'a', 'p'));
  }

  // populate the glyph set to keep
  hb_set_t *glyphs = hb_subset_input_glyph_set(input);
  for (uint16_t gid : usedGlyphIds)
    hb_set_add(glyphs, gid);

  hb_face_t *subset = hb_subset_or_fail(face, input);
  hb_subset_input_destroy(input);
  hb_face_destroy(face);

  if (!subset)
    return false;

  hb_blob_t *result = hb_face_reference_blob(subset);
  unsigned int length = 0;
  const char *resultData = hb_blob_get_data(result, &length);

  if (length == 0 || !resultData) {
    hb_blob_destroy(result);
    hb_face_destroy(subset);
    return false;
  }

  output.assign(reinterpret_cast<const uint8_t *>(resultData),
                reinterpret_cast<const uint8_t *>(resultData) + length);

  hb_blob_destroy(result);
  hb_face_destroy(subset);
  return true;
}
