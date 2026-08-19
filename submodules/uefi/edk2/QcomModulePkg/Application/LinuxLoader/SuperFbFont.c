/*
 * Glyph lookup for the embedded bitmap font.
 *
 * Copyright (c) 2026, contributors to the canoe ABL tree.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "SuperFbFont.h"

BOOLEAN
SfbFontGetGlyph (IN CHAR16 Ch,
                 OUT UINT32 *Offset,
                 OUT UINT8  *Width,
                 OUT UINT8  *Advance)
{
  UINTN  Index;

  for (Index = 0; Index < gSfbFontGlyphCount; Index++) {
    if (gSfbFontGlyphs[Index].Ch == Ch) {
      if (Offset != NULL) {
        *Offset = gSfbFontGlyphs[Index].Offset;
      }
      if (Width != NULL) {
        *Width = gSfbFontGlyphs[Index].Width;
      }
      if (Advance != NULL) {
        *Advance = gSfbFontGlyphs[Index].Advance;
      }
      return TRUE;
    }
  }

  return FALSE;
}

BOOLEAN
SfbArtGetGlyph (IN CHAR16 Ch,
                OUT UINT32 *Offset,
                OUT UINT16 *Width,
                OUT UINT16 *Advance)
{
  UINTN  Index;

  for (Index = 0; Index < gSfbArtGlyphCount; Index++) {
    if (gSfbArtGlyphs[Index].Ch == Ch) {
      if (Offset != NULL) {
        *Offset = gSfbArtGlyphs[Index].Offset;
      }
      if (Width != NULL) {
        *Width = gSfbArtGlyphs[Index].Width;
      }
      if (Advance != NULL) {
        *Advance = gSfbArtGlyphs[Index].Advance;
      }
      return TRUE;
    }
  }

  return FALSE;
}
