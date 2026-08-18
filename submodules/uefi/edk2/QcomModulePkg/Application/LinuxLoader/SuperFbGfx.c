/*
 * GOP renderer implementation.  All drawing funnels through Gop->Blt using
 * EFI_GRAPHICS_OUTPUT_BLT_PIXEL, whose field order (Blue, Green, Red) is
 * defined by the spec regardless of the frame buffer pixel format.
 *
 * Copyright (c) 2026, contributors to the canoe ABL tree.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "SuperFbGfx.h"
#include "SuperFbFont.h"

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/GraphicsOutput.h>

STATIC EFI_GRAPHICS_OUTPUT_PROTOCOL  *mSfbGop = NULL;
STATIC UINT32                        mSfbScreenW = 0;
STATIC UINT32                        mSfbScreenH = 0;

STATIC
VOID
SfbGfxPixel (IN EFI_GRAPHICS_OUTPUT_BLT_PIXEL *Pix, IN UINT32 Color)
{
  Pix->Blue     = (UINT8)(Color & 0xFF);
  Pix->Green    = (UINT8)((Color >> 8) & 0xFF);
  Pix->Red      = (UINT8)((Color >> 16) & 0xFF);
  Pix->Reserved = 0;
}

BOOLEAN
SfbGfxInit (VOID)
{
  EFI_STATUS  Status;

  if (mSfbGop != NULL) {
    return TRUE;
  }

  Status = gBS->LocateProtocol (&gEfiGraphicsOutputProtocolGuid, NULL,
                                (VOID **)&mSfbGop);
  if (EFI_ERROR (Status) || mSfbGop == NULL || mSfbGop->Mode == NULL ||
      mSfbGop->Mode->Info == NULL) {
    mSfbGop = NULL;
    return FALSE;
  }

  if (mSfbGop->Mode->Info->PixelFormat != PixelBlueGreenRedReserved8BitPerColor &&
      mSfbGop->Mode->Info->PixelFormat != PixelRedGreenBlueReserved8BitPerColor) {
    DEBUG ((EFI_D_WARN, "SFB: unsupported GOP pixel format %d\n",
            mSfbGop->Mode->Info->PixelFormat));
    mSfbGop = NULL;
    return FALSE;
  }

  mSfbScreenW = mSfbGop->Mode->Info->HorizontalResolution;
  mSfbScreenH = mSfbGop->Mode->Info->VerticalResolution;
  if (mSfbScreenW == 0 || mSfbScreenH == 0) {
    mSfbGop = NULL;
    return FALSE;
  }

  DEBUG ((EFI_D_INFO, "SFB: GOP %u x %u\n", mSfbScreenW, mSfbScreenH));
  return TRUE;
}

BOOLEAN
SfbGfxActive (VOID)
{
  return (BOOLEAN)(mSfbGop != NULL);
}

VOID
SfbGfxGetScreen (OUT UINT32 *Width, OUT UINT32 *Height)
{
  if (Width != NULL) {
    *Width = mSfbScreenW;
  }
  if (Height != NULL) {
    *Height = mSfbScreenH;
  }
}

VOID
SfbGfxClear (IN UINT32 Color)
{
  if (!SfbGfxActive ()) {
    return;
  }

  SfbGfxFillRect (0, 0, mSfbScreenW, mSfbScreenH, Color);
}

VOID
SfbGfxFillRect (IN UINT32 X,
                IN UINT32 Y,
                IN UINT32 W,
                IN UINT32 H,
                IN UINT32 Color)
{
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL  Pixel;

  if (!SfbGfxActive () || W == 0 || H == 0) {
    return;
  }

  SfbGfxPixel (&Pixel, Color);
  mSfbGop->Blt (mSfbGop, &Pixel, EfiBltVideoFill, 0, 0, X, Y, W, H, 0);
}

VOID
SfbGfxHLine (IN UINT32 Y,
             IN UINT32 X0,
             IN UINT32 X1,
             IN UINT32 Thick,
             IN UINT32 Color)
{
  if (X1 < X0) {
    return;
  }
  SfbGfxFillRect (X0, Y, X1 - X0 + 1, Thick, Color);
}

UINT32
SfbGfxTextWidth (IN CONST CHAR16 *Text)
{
  UINT32  Width = 0;
  UINTN   Index;

  for (Index = 0; Text[Index] != L'\0'; Index++) {
    UINT32  Offset;
    UINT8   GlyphWidth;
    UINT8   Advance;

    if (SfbFontGetGlyph (Text[Index], &Offset, &GlyphWidth, &Advance)) {
      Width += Advance;
    } else {
      Width += SFB_FONT_CELL_H / 2;
    }
  }

  return Width;
}

/*
 * Blend one proportional glyph into a row buffer.  Alpha is 4-bit; a missing
 * glyph is drawn as a hollow box.
 */
STATIC
VOID
SfbGfxBlendGlyph (IN OUT EFI_GRAPHICS_OUTPUT_BLT_PIXEL *Row,
                  IN UINT32                            RowWidth,
                  IN UINT32                            PenX,
                  IN CHAR16                            Ch,
                  IN UINT32                            Fg,
                  IN UINT32                            Bg,
                  OUT UINT32                           *Advance)
{
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL  FgPix;
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL  BgPix;
  UINT32                         Offset;
  UINT8                          GlyphWidth;
  UINT8                          GlyphAdvance;
  UINT32                         X;
  UINT32                         Y;

  SfbGfxPixel (&FgPix, Fg);
  SfbGfxPixel (&BgPix, Bg);

  if (!SfbFontGetGlyph (Ch, &Offset, &GlyphWidth, &GlyphAdvance)) {
    /* Hollow placeholder box for characters outside the font table. */
    UINT32  BoxW = SFB_FONT_CELL_H / 2;

    for (X = 0; X < BoxW; X++) {
      Row[PenX + X] = FgPix;
      Row[PenX + X + (SFB_FONT_CELL_H - 1) * RowWidth] = FgPix;
    }
    for (Y = 0; Y < SFB_FONT_CELL_H; Y++) {
      Row[PenX + Y * RowWidth] = FgPix;
      Row[PenX + BoxW - 1 + Y * RowWidth] = FgPix;
    }
    *Advance = BoxW;
    return;
  }

  for (Y = 0; Y < SFB_FONT_CELL_H; Y++) {
    for (X = 0; X < GlyphWidth; X++) {
      UINTN   BitIndex = Y * GlyphWidth + X;
      UINT8   Alpha = (UINT8)(gSfbFontBitmap[Offset + BitIndex / 2] >>
                              ((BitIndex & 1) ? 0 : 4)) & 0xF;
      UINTN   Dest = PenX + X + Y * RowWidth;

      if (Alpha == SFB_FONT_MAX_ALPHA) {
        Row[Dest] = FgPix;
      } else if (Alpha != 0) {
        Row[Dest].Blue  = (UINT8)(((UINTN)FgPix.Blue  * Alpha +
                                   (UINTN)BgPix.Blue  * (SFB_FONT_MAX_ALPHA - Alpha)) /
                                  SFB_FONT_MAX_ALPHA);
        Row[Dest].Green = (UINT8)(((UINTN)FgPix.Green * Alpha +
                                   (UINTN)BgPix.Green * (SFB_FONT_MAX_ALPHA - Alpha)) /
                                  SFB_FONT_MAX_ALPHA);
        Row[Dest].Red   = (UINT8)(((UINTN)FgPix.Red   * Alpha +
                                   (UINTN)BgPix.Red   * (SFB_FONT_MAX_ALPHA - Alpha)) /
                                  SFB_FONT_MAX_ALPHA);
      }
    }
  }

  *Advance = GlyphAdvance;
}

VOID
SfbGfxDrawText (IN CONST CHAR16 *Text,
                IN UINT32       X,
                IN UINT32       Y,
                IN UINT32       Fg,
                IN UINT32       Bg)
{
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL  *Row;
  UINTN                           Length;
  UINTN                           RowWidth;
  UINTN                           MaxWidth;
  UINTN                           Index;
  UINT32                          PenX;

  if (!SfbGfxActive ()) {
    return;
  }

  Length = StrLen (Text);
  if (Length == 0) {
    return;
  }

  /* Clip at the right edge of the screen. */
  MaxWidth = (mSfbScreenW > X) ? mSfbScreenW - X : 0;
  if (MaxWidth == 0) {
    return;
  }

  /* Measure, then drop characters from the end until the text fits. */
  while (TRUE) {
    RowWidth = 0;
    for (Index = 0; Index < Length; Index++) {
      UINT32  Offset;
      UINT8   GlyphWidth;
      UINT8   Advance;

      if (SfbFontGetGlyph (Text[Index], &Offset, &GlyphWidth, &Advance)) {
        RowWidth += Advance;
      } else {
        RowWidth += SFB_FONT_CELL_H / 2;
      }
    }
    if (RowWidth <= MaxWidth || Length <= 1) {
      break;
    }
    Length--;
  }

  Row = AllocateZeroPool (RowWidth * SFB_FONT_CELL_H * sizeof (*Row));
  if (Row == NULL) {
    return;
  }

  /* Pre-fill with the background color. */
  {
    EFI_GRAPHICS_OUTPUT_BLT_PIXEL  BgPix;
    UINTN                          Pixel;

    SfbGfxPixel (&BgPix, Bg);
    for (Pixel = 0; Pixel < RowWidth * SFB_FONT_CELL_H; Pixel++) {
      Row[Pixel] = BgPix;
    }
  }

  PenX = 0;
  for (Index = 0; Index < Length; Index++) {
    UINT32  Advance;

    SfbGfxBlendGlyph (Row, (UINT32)RowWidth, PenX, Text[Index], Fg, Bg,
                      &Advance);
    PenX += Advance;
  }

  mSfbGop->Blt (mSfbGop, Row, EfiBltBufferToVideo, 0, 0, X, Y,
                (UINT32)RowWidth, SFB_FONT_CELL_H,
                RowWidth * sizeof (EFI_GRAPHICS_OUTPUT_BLT_PIXEL));
  FreePool (Row);
}

VOID
SfbGfxDrawTextScaled (IN CONST CHAR16 *Text,
                      IN UINT32       X,
                      IN UINT32       Y,
                      IN UINT32       Scale,
                      IN UINT32       Fg,
                      IN UINT32       Bg)
{
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL  *Row;
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL  FgPix;
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL  BgPix;
  UINTN                           Length;
  UINTN                           RowWidth;
  UINTN                           RowHeight;
  UINTN                           MaxWidth;
  UINTN                           Index;
  UINT32                          PenX;
  UINT32                          ScaledCellH;

  if (!SfbGfxActive () || Scale == 0) {
    return;
  }

  Length = StrLen (Text);
  if (Length == 0) {
    return;
  }

  ScaledCellH = SFB_FONT_CELL_H * Scale;
  MaxWidth = (mSfbScreenW > X) ? mSfbScreenW - X : 0;
  if (MaxWidth == 0) {
    return;
  }

  /* Measure, then drop characters from the end until the text fits. */
  while (TRUE) {
    RowWidth = 0;
    for (Index = 0; Index < Length; Index++) {
      UINT32  Offset;
      UINT8   GlyphWidth;
      UINT8   Advance;

      if (SfbFontGetGlyph (Text[Index], &Offset, &GlyphWidth, &Advance)) {
        RowWidth += (UINTN)Advance * Scale;
      } else {
        RowWidth += (UINTN)(SFB_FONT_CELL_H / 2) * Scale;
      }
    }
    if (RowWidth <= MaxWidth || Length <= 1) {
      break;
    }
    Length--;
  }

  RowHeight = ScaledCellH;
  Row = AllocateZeroPool (RowWidth * RowHeight * sizeof (*Row));
  if (Row == NULL) {
    return;
  }

  SfbGfxPixel (&FgPix, Fg);
  SfbGfxPixel (&BgPix, Bg);
  for (Index = 0; Index < RowWidth * RowHeight; Index++) {
    Row[Index] = BgPix;
  }

  PenX = 0;
  for (Index = 0; Index < Length; Index++) {
    UINT32  Offset;
    UINT8   GlyphWidth;
    UINT8   GlyphAdvance;
    UINT32  Gx;
    UINT32  Gy;

    if (!SfbFontGetGlyph (Text[Index], &Offset, &GlyphWidth, &GlyphAdvance)) {
      /* Hollow placeholder box, scaled like a missing glyph would be. */
      UINT32  BoxW = (SFB_FONT_CELL_H / 2) * Scale;

      for (Gx = 0; Gx < BoxW; Gx++) {
        Row[PenX + Gx] = FgPix;
        Row[PenX + Gx + (RowHeight - 1) * RowWidth] = FgPix;
      }
      for (Gy = 0; Gy < RowHeight; Gy++) {
        Row[PenX + Gy * RowWidth] = FgPix;
        Row[PenX + BoxW - 1 + Gy * RowWidth] = FgPix;
      }
      PenX += BoxW;
      continue;
    }

    /* Nearest-neighbour scale: each source cell becomes Scale x Scale pixels
     * with the same alpha, blended the same way as the unscaled path. */
    for (Gy = 0; Gy < ScaledCellH; Gy++) {
      for (Gx = 0; Gx < (UINT32)GlyphWidth * Scale; Gx++) {
        UINTN   BitIndex = (Gy / Scale) * GlyphWidth + (Gx / Scale);
        UINT8   Alpha = (UINT8)(gSfbFontBitmap[Offset + BitIndex / 2] >>
                                ((BitIndex & 1) ? 0 : 4)) & 0xF;
        UINTN   Dest = PenX + Gx + Gy * RowWidth;

        if (Alpha == SFB_FONT_MAX_ALPHA) {
          Row[Dest] = FgPix;
        } else if (Alpha != 0) {
          Row[Dest].Blue  = (UINT8)(((UINTN)FgPix.Blue  * Alpha +
                                     (UINTN)BgPix.Blue  *
                                       (SFB_FONT_MAX_ALPHA - Alpha)) /
                                    SFB_FONT_MAX_ALPHA);
          Row[Dest].Green = (UINT8)(((UINTN)FgPix.Green * Alpha +
                                     (UINTN)BgPix.Green *
                                       (SFB_FONT_MAX_ALPHA - Alpha)) /
                                    SFB_FONT_MAX_ALPHA);
          Row[Dest].Red   = (UINT8)(((UINTN)FgPix.Red   * Alpha +
                                     (UINTN)BgPix.Red   *
                                       (SFB_FONT_MAX_ALPHA - Alpha)) /
                                    SFB_FONT_MAX_ALPHA);
        }
      }
    }

    PenX += (UINT32)GlyphAdvance * Scale;
  }

  mSfbGop->Blt (mSfbGop, Row, EfiBltBufferToVideo, 0, 0, X, Y,
                (UINT32)RowWidth, (UINT32)RowHeight,
                RowWidth * sizeof (EFI_GRAPHICS_OUTPUT_BLT_PIXEL));
  FreePool (Row);
}
