/*
 * Glyph lookup for the embedded bitmap font.
 *
 * Copyright (c) 2026, contributors to the canoe ABL tree.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "SuperFbFont.h"

BOOLEAN
SfbFontGetGlyph (IN CHAR16 Ch, OUT UINT16 *Offset)
{
  UINTN  Index;

  for (Index = 0; Index < gSfbFontGlyphCount; Index++) {
    if (gSfbFontGlyphs[Index].Ch == Ch) {
      if (Offset != NULL) {
        *Offset = gSfbFontGlyphs[Index].Offset;
      }
      return TRUE;
    }
  }

  return FALSE;
}
