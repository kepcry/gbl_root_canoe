/*
 * Console UI for the super-fastboot boot menu.
 *
 * Three keys drive everything: volume up and volume down move the cursor, and
 * power confirms.
 *
 * Copyright (c) 2026, contributors to the canoe ABL tree.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "SuperFbMenu.h"

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/ShutdownServices.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Protocol/GraphicsOutput.h>
#include <Protocol/SimpleTextIn.h>

#include "SuperFbGfx.h"
#include "SuperFbLang.h"
#include "SuperFbSettings.h"

/* Keeps the translation unit legal when the feature is compiled out. */
CONST CHAR8 *gSfbMenuModuleTag = "SuperFbMenu";

/*
 * Lilac ("dingxiang") purple theme.  The UEFI text palette has no true pale
 * purple, so the brightest magenta it offers (EFI_LIGHTMAGENTA) carries the
 * accents and the darker magenta is used for the panel frame and hints.
 */
#define SFB_ATTR_NORMAL    EFI_TEXT_ATTR (EFI_LIGHTGRAY, EFI_BLACK)
#define SFB_ATTR_SELECTED  EFI_TEXT_ATTR (EFI_BLACK, EFI_LIGHTMAGENTA)
#define SFB_ATTR_TITLE     EFI_TEXT_ATTR (EFI_LIGHTMAGENTA, EFI_BLACK)
#define SFB_ATTR_PANEL     EFI_TEXT_ATTR (EFI_MAGENTA, EFI_BLACK)
#define SFB_ATTR_FOOTER    EFI_TEXT_ATTR (EFI_MAGENTA, EFI_BLACK)
#define SFB_ATTR_DISABLED  EFI_TEXT_ATTR (EFI_DARKGRAY, EFI_BLACK)

SFB_KEY
SfbWaitForKey (IN UINT32 TimeoutMs)
{
  EFI_STATUS     Status;
  EFI_EVENT      TimerEvent = NULL;
  EFI_EVENT      WaitList[2];
  UINTN          WaitCount;
  UINTN          EventIndex;
  EFI_INPUT_KEY  Key;
  SFB_KEY        Result = SfbKeyTimeout;

  if (TimeoutMs != 0) {
    Status = gBS->CreateEvent (EVT_TIMER, TPL_CALLBACK, NULL, NULL, &TimerEvent);
    if (EFI_ERROR (Status)) {
      TimerEvent = NULL;
    } else {
      /* Boot services timers count in 100ns units. */
      Status = gBS->SetTimer (TimerEvent, TimerRelative,
                              (UINT64)TimeoutMs * 10000);
      if (EFI_ERROR (Status)) {
        gBS->CloseEvent (TimerEvent);
        TimerEvent = NULL;
      }
    }
  }

  WaitList[0] = gST->ConIn->WaitForKey;
  WaitCount = 1;
  if (TimerEvent != NULL) {
    WaitList[1] = TimerEvent;
    WaitCount = 2;
  }

  while (TRUE) {
    Status = gBS->WaitForEvent (WaitCount, WaitList, &EventIndex);
    if (EFI_ERROR (Status)) {
      DEBUG ((EFI_D_ERROR, "SFB: WaitForEvent failed: %r\n", Status));
      break;
    }

    if (EventIndex == 1) {
      break;
    }

    Status = gST->ConIn->ReadKeyStroke (gST->ConIn, &Key);
    if (EFI_ERROR (Status)) {
      continue;
    }

    /*
     * On the handset the Qualcomm keypad driver reports the volume keys as
     * SCAN_UP and SCAN_DOWN, and power arrives as a carriage return.
     *
     * Anything left over counts as confirm: on a three-key handset there is
     * nothing else it can be, so the menu stays usable even if a platform
     * reports power differently from what is expected here.
     */
    if (Key.ScanCode == SCAN_UP) {
      Result = SfbKeyUp;
    } else if (Key.ScanCode == SCAN_DOWN) {
      Result = SfbKeyDown;
    } else {
      DEBUG ((EFI_D_VERBOSE, "SFB: confirm key scan=0x%x char=0x%x\n",
              Key.ScanCode, Key.UnicodeChar));
      Result = SfbKeySelect;
    }
    break;
  }

  if (TimerEvent != NULL) {
    gBS->CloseEvent (TimerEvent);
  }

  return Result;
}

/* ---- drawing ------------------------------------------------------------ */

/*
 * Every screen is drawn as a bordered panel centred on the console.  The
 * panel is sized to roughly a third of the screen (a third of the height, up
 * to two thirds of the width) so the menu reads as one window in the middle
 * of the display instead of a list glued to the top-left corner.  The
 * console font itself is whatever the firmware provides; nothing here changes
 * it, only the footprint the UI takes on screen.
 */
#define SFB_PANEL_MIN_WIDTH  44
#define SFB_PANEL_MAX_WIDTH  60

/* Geometry of the panel currently being drawn; SfbBeginScreen sets it and
 * the row/footer helpers read it until SfbEndScreen closes the panel. */
STATIC UINTN  gSfbPanelWidth  = 0;
STATIC UINTN  gSfbPanelLeft   = 0;
STATIC UINTN  gSfbPanelExtra  = 0;
/* Subtitle (the browser's path) is deferred to the bottom of the panel so the
 * selection bar painted over the list rows can never reach it. */
STATIC CONST CHAR16  *gSfbPanelSubtitle = NULL;

STATIC
VOID
SfbScreenSize (OUT UINTN *Columns, OUT UINTN *Rows)
{
  UINTN  Cols = 0;
  UINTN  Rs   = 0;

  /*
   * EFI_SIMPLE_TEXT_OUTPUT_MODE carries no column/row count; the console
   * dimensions come from QueryMode for the currently selected mode.
   */
  if (gST->ConOut->Mode != NULL &&
      gST->ConOut->QueryMode != NULL &&
      gST->ConOut->Mode->Mode >= 0) {
    gST->ConOut->QueryMode (gST->ConOut, (UINTN)gST->ConOut->Mode->Mode,
                            &Cols, &Rs);
  }

  *Columns = (Cols == 0) ? 80 : Cols;
  *Rows    = (Rs   == 0) ? 30 : Rs;
}

STATIC
UINTN
SfbPanelWidth (VOID)
{
  UINTN  Cols;
  UINTN  Rows;
  UINTN  Width;

  SfbScreenSize (&Cols, &Rows);
  Width = Cols * 2 / 3;
  if (Width > SFB_PANEL_MAX_WIDTH) {
    Width = SFB_PANEL_MAX_WIDTH;
  }
  if (Width < SFB_PANEL_MIN_WIDTH) {
    Width = SFB_PANEL_MIN_WIDTH;
  }
  if (Width > Cols - 2) {
    Width = (Cols > 2) ? Cols - 2 : 1;
  }

  return Width;
}

STATIC
VOID
SfbPrintLeftPad (VOID)
{
  UINTN  Index;
  UINTN  Left = gSfbPanelLeft;

  for (Index = 0; Index < Left; Index++) {
    Print (L" ");
  }
}

/* Print one full-width line of the panel: purple frame, Inner (which must be
 * exactly Width - 2 characters wide) in Attr. */
STATIC
VOID
SfbPanelLine (IN CONST CHAR16 *Inner, IN UINTN Attr)
{
  SfbPrintLeftPad ();
  gST->ConOut->SetAttribute (gST->ConOut, SFB_ATTR_PANEL);
  Print (L"|");
  gST->ConOut->SetAttribute (gST->ConOut, Attr);
  Print (L"%s", Inner);
  gST->ConOut->SetAttribute (gST->ConOut, SFB_ATTR_PANEL);
  Print (L"|\r\n");
  gST->ConOut->SetAttribute (gST->ConOut, SFB_ATTR_NORMAL);
}

STATIC
VOID
SfbPanelBorder (VOID)
{
  UINTN  Index;
  UINTN  Width = gSfbPanelWidth;

  SfbPrintLeftPad ();
  gST->ConOut->SetAttribute (gST->ConOut, SFB_ATTR_PANEL);
  Print (L"+");
  for (Index = 1; Index + 1 < Width; Index++) {
    Print (L"-");
  }
  Print (L"+\r\n");
  gST->ConOut->SetAttribute (gST->ConOut, SFB_ATTR_NORMAL);
}

STATIC
VOID
SfbPanelBlank (VOID)
{
  CHAR16  Inner[SFB_PANEL_MAX_WIDTH + 1];
  UINTN   Width = gSfbPanelWidth;
  UINTN   Index;

  for (Index = 0; Index + 2 < Width; Index++) {
    Inner[Index] = L' ';
  }
  Inner[Index] = L'\0';

  SfbPanelLine (Inner, SFB_ATTR_NORMAL);
}

STATIC
VOID
SfbPanelCentered (IN CONST CHAR16 *Text, IN UINTN Attr)
{
  CHAR16  Inner[SFB_PANEL_MAX_WIDTH + 1];
  UINTN   Width = gSfbPanelWidth;
  UINTN   InnerWidth;
  UINTN   Length;
  UINTN   Pad;
  UINTN   Index;

  InnerWidth = (Width >= 2) ? Width - 2 : 0;
  Length = StrLen (Text);
  if (Length > InnerWidth) {
    Length = InnerWidth;
  }
  Pad = InnerWidth - Length;

  Index = 0;
  while (Index < Pad / 2) {
    Inner[Index++] = L' ';
  }
  CopyMem (&Inner[Index], Text, Length * sizeof (CHAR16));
  Index += Length;
  while (Index < InnerWidth) {
    Inner[Index++] = L' ';
  }
  Inner[Index] = L'\0';

  SfbPanelLine (Inner, Attr);
}

STATIC
VOID
SfbPanelText (IN CONST CHAR16 *Text, IN UINTN Attr)
{
  CHAR16  Inner[SFB_PANEL_MAX_WIDTH + 1];
  UINTN   Width = gSfbPanelWidth;
  UINTN   InnerWidth;
  UINTN   Length;
  UINTN   Index;

  InnerWidth = (Width >= 2) ? Width - 2 : 0;
  Length = StrLen (Text);
  if (InnerWidth > 1 && Length > InnerWidth - 1) {
    Length = InnerWidth - 1;
  }

  Index = 0;
  Inner[Index++] = L' ';
  CopyMem (&Inner[Index], Text, Length * sizeof (CHAR16));
  Index += Length;
  while (Index < InnerWidth) {
    Inner[Index++] = L' ';
  }
  Inner[Index] = L'\0';

  SfbPanelLine (Inner, Attr);
}

/* Dim, left-aligned note inside the panel ("... N more" and friends). */
STATIC
VOID
SfbGfxPanelNote (IN CONST CHAR16 *Text);

VOID
SfbPanelNote (IN CONST CHAR16 *Text)
{
  if (SfbGfxActive ()) {
    SfbGfxPanelNote (Text);
    return;
  }

  SfbPanelText (Text, SFB_ATTR_FOOTER);
}

/* ---- graphical (GOP) UI -------------------------------------------------- */

/*
 * The graphical path draws the same panel as the text path but on the frame
 * buffer with the embedded 32px font.  The panel is a full-width band a third
 * of the screen tall, vertically centred.
 */
STATIC UINT32  gSfbGfxPanelTop    = 0;
STATIC UINT32  gSfbGfxPanelH      = 0;
STATIC UINT32  gSfbGfxRowsStart   = 0;
STATIC UINT32  gSfbGfxRowH        = 52;
STATIC UINT32  gSfbGfxFooterY     = 0;
STATIC UINTN   gSfbGfxRowIndex    = 0;
STATIC UINTN   gSfbGfxVisibleRows = SFB_VISIBLE_ROWS;
/* Same deferred subtitle as the text path, drawn at the bottom of the panel. */
STATIC CONST CHAR16  *gSfbGfxSubtitle = NULL;

/* Copy Text into Out (OutChars includes the NUL), appending "..." when cut. */
STATIC
VOID
SfbGfxFitText (IN CONST CHAR16 *Text, OUT CHAR16 *Out, IN UINTN OutChars)
{
  UINTN  Len;
  UINTN  Keep;

  if (OutChars == 0) {
    return;
  }

  Len = StrLen (Text);
  Keep = OutChars - 1;
  if (Len <= Keep) {
    CopyMem (Out, Text, Len * sizeof (CHAR16));
    Out[Len] = L'\0';
    return;
  }

  if (Keep >= 3) {
    Keep -= 3;
    CopyMem (Out, Text, Keep * sizeof (CHAR16));
    Out[Keep] = L'\0';
    StrnCatS (Out, OutChars, L"...", OutChars - Keep - 1);
  } else {
    Out[0] = L'\0';
  }
}

STATIC
VOID
SfbGfxDrawCentered (IN CONST CHAR16 *Text,
                    IN UINT32       W,
                    IN UINT32       Y,
                    IN UINT32       Fg,
                    IN UINT32       Bg)
{
  UINT32  TextW;
  UINT32  X;

  TextW = SfbGfxTextWidth (Text);
  X = (TextW >= W) ? 0 : (W - TextW) / 2;
  SfbGfxDrawText (Text, X, Y, Fg, Bg);
}

STATIC
VOID
SfbGfxPanelBegin (IN CONST CHAR16 *Title, IN CONST CHAR16 *Subtitle)
{
  UINT32  W;
  UINT32  H;
  UINT32  TitleY;
  UINT32  RowsFloor;

  SfbGfxGetScreen (&W, &H);
  gSfbGfxPanelH = H / 3;
  /* A bottom-centred subtitle (the browser's path) needs a little extra room
   * so it does not steal space from the list or collide with the footer. */
  if (Subtitle != NULL) {
    gSfbGfxPanelH += SFB_FONT_CELL_H + 20;
  }
  if (gSfbGfxPanelH < 3 * SFB_FONT_CELL_H) {
    gSfbGfxPanelH = 3 * SFB_FONT_CELL_H;
  }
  gSfbGfxPanelTop = (H - gSfbGfxPanelH) / 2;

  SfbGfxClear (SFB_COLOR_BG);
  SfbGfxFillRect (0, gSfbGfxPanelTop, W, gSfbGfxPanelH, SFB_COLOR_PANEL);
  SfbGfxHLine (gSfbGfxPanelTop, 0, W - 1, 2, SFB_COLOR_ACCENT);
  SfbGfxHLine (gSfbGfxPanelTop + gSfbGfxPanelH - 2, 0, W - 1, 2,
               SFB_COLOR_ACCENT);

  TitleY = gSfbGfxPanelTop + 20;
  gSfbGfxRowsStart = TitleY + SFB_FONT_CELL_H + 18;
  gSfbGfxFooterY = gSfbGfxPanelTop + gSfbGfxPanelH - 20 - SFB_FONT_CELL_H;
  gSfbGfxSubtitle = Subtitle;

  /* Keep a clear band between the last list row and the footer for the
   * subtitle, so the selection bar never paints over it. */
  RowsFloor = (Subtitle != NULL) ? 8 + SFB_FONT_CELL_H + 8 : 8;
  if (gSfbGfxFooterY > gSfbGfxRowsStart + RowsFloor) {
    gSfbGfxVisibleRows =
      (gSfbGfxFooterY - gSfbGfxRowsStart - RowsFloor) / gSfbGfxRowH;
  } else {
    gSfbGfxVisibleRows = 0;
  }
  if (gSfbGfxVisibleRows > 8) {
    gSfbGfxVisibleRows = 8;
  }
  if (gSfbGfxVisibleRows < 4) {
    gSfbGfxVisibleRows = 4;
  }
  gSfbGfxRowIndex = 0;

  SfbGfxDrawCentered (Title, W, TitleY, SFB_COLOR_ACCENT, SFB_COLOR_PANEL);
}

STATIC
VOID
SfbGfxPanelRow (IN BOOLEAN Selected, IN BOOLEAN Enabled,
                IN CONST CHAR16 *Marker,
                IN CONST CHAR16 *Text)
{
  CHAR16  Full[SFB_DESC_CHARS + 8];
  CHAR16  Fit[SFB_DESC_CHARS + 8];
  UINT32  W;
  UINT32  H;
  UINT32  RowY;
  UINT32  TextY;
  UINTN   MaxChars;

  SfbGfxGetScreen (&W, &H);
  if (Marker != NULL && Marker[0] != L'\0') {
    UnicodeSPrint (Full, sizeof (Full), L"%s %s", Marker, Text);
  } else {
    StrnCpyS (Full, SFB_DESC_CHARS + 8, Text, SFB_DESC_CHARS + 7);
  }

  /* The narrowest Latin glyphs advance 6px and CJK cells 26px; this only
   * bounds the truncation buffer, so the rough 8px estimate is fine. */
  MaxChars = W / 8;
  if (MaxChars > SFB_DESC_CHARS + 4) {
    MaxChars = SFB_DESC_CHARS + 4;
  }
  SfbGfxFitText (Full, Fit, MaxChars + 1);

  RowY = gSfbGfxRowsStart + (UINT32)gSfbGfxRowIndex * gSfbGfxRowH;
  gSfbGfxRowIndex++;
  TextY = RowY + (gSfbGfxRowH - SFB_FONT_CELL_H) / 2;

  if (!Enabled) {
    /* Unavailable rows are drawn dimmed, without the selection bar. */
    SfbGfxDrawText (Fit, 40, TextY, SFB_COLOR_DISABLED, SFB_COLOR_PANEL);
  } else if (Selected) {
    SfbGfxFillRect (0, RowY, W, gSfbGfxRowH - 8, SFB_COLOR_SEL_BG);
    SfbGfxDrawText (Fit, 40, TextY, SFB_COLOR_SEL_FG, SFB_COLOR_SEL_BG);
  } else {
    SfbGfxDrawText (Fit, 40, TextY, SFB_COLOR_TEXT, SFB_COLOR_PANEL);
  }
}

STATIC
VOID
SfbGfxPanelNote (IN CONST CHAR16 *Text)
{
  CHAR16  Fit[SFB_DESC_CHARS + 8];
  UINT32  W;
  UINT32  H;
  UINT32  RowY;
  UINT32  TextY;

  SfbGfxGetScreen (&W, &H);
  SfbGfxFitText (Text, Fit, SFB_DESC_CHARS + 6);
  RowY = gSfbGfxRowsStart + (UINT32)gSfbGfxRowIndex * gSfbGfxRowH;
  gSfbGfxRowIndex++;
  TextY = RowY + (gSfbGfxRowH - SFB_FONT_CELL_H) / 2;
  SfbGfxDrawText (Fit, 40, TextY, SFB_COLOR_ACCENT_D, SFB_COLOR_PANEL);
}

STATIC
VOID
SfbGfxPanelEnd (IN CONST CHAR16 *Footer)
{
  UINT32  W;
  UINT32  H;

  SfbGfxGetScreen (&W, &H);
  if (gSfbGfxSubtitle != NULL) {
    SfbGfxDrawCentered (gSfbGfxSubtitle, W,
                        gSfbGfxFooterY - SFB_FONT_CELL_H - 12,
                        SFB_COLOR_ACCENT_D, SFB_COLOR_PANEL);
  }
  if (Footer != NULL) {
    SfbGfxDrawCentered (Footer, W, gSfbGfxFooterY, SFB_COLOR_ACCENT_D,
                        SFB_COLOR_PANEL);
  }
}

STATIC
VOID
SfbGfxShowBanner (IN CONST CHAR16 *Text, IN CONST CHAR16 *Detail)
{
  UINT32  W;
  UINT32  H;
  UINT32  BlockH;
  UINT32  Top;
  UINT32  TitleY;

  SfbGfxGetScreen (&W, &H);
  BlockH = (Detail != NULL) ? 96 : 64;
  Top = (H >= BlockH) ? (H - BlockH) / 2 : 0;

  SfbGfxClear (SFB_COLOR_BG);
  SfbGfxFillRect (0, Top, W, BlockH, SFB_COLOR_PANEL);
  SfbGfxHLine (Top, 0, W - 1, 2, SFB_COLOR_ACCENT);
  SfbGfxHLine (Top + BlockH - 2, 0, W - 1, 2, SFB_COLOR_ACCENT);

  TitleY = Top + 20;
  SfbGfxDrawCentered (Text, W, TitleY, SFB_COLOR_ACCENT, SFB_COLOR_PANEL);
  if (Detail != NULL) {
    SfbGfxDrawCentered (Detail, W, TitleY + SFB_FONT_CELL_H + 10,
                        SFB_COLOR_TEXT, SFB_COLOR_PANEL);
  }
}

UINTN
SfbVisibleRows (VOID)
{
  if (SfbGfxActive ()) {
    return gSfbGfxVisibleRows;
  }
  return SFB_VISIBLE_ROWS;
}

VOID
SfbBeginScreen (IN CONST CHAR16 *Title, IN CONST CHAR16 *Subtitle)
{
  if (SfbGfxActive ()) {
    SfbGfxPanelBegin (Title, Subtitle);
    return;
  }

  UINTN  Cols;
  UINTN  Rows;
  UINTN  Content;
  UINTN  Top;

  gST->ConOut->SetAttribute (gST->ConOut, SFB_ATTR_TITLE);
  gST->ConOut->ClearScreen (gST->ConOut);
  gST->ConOut->EnableCursor (gST->ConOut, FALSE);

  SfbScreenSize (&Cols, &Rows);
  gSfbPanelWidth = SfbPanelWidth ();
  gSfbPanelLeft  = (Cols - gSfbPanelWidth) / 2;
  gSfbPanelSubtitle = Subtitle;

  /*
   * Fixed chrome: top border, blank, title, blank, the visible list rows,
   * blank, [subtitle, blank,] footer and bottom border.  The subtitle is
   * deferred to SfbEndScreen so it sits at the bottom of the window, clear of
   * the selection bar the row drawing paints across the list.  The panel is
   * then grown to at least a third of the screen height so it reads as a
   * centred window no matter how short the list is.
   */
  Content = SFB_VISIBLE_ROWS + 7;
  if (Subtitle != NULL) {
    Content += 2;
  }
  if (Content < Rows / 3) {
    Content = Rows / 3;
  }
  gSfbPanelExtra = Content - (SFB_VISIBLE_ROWS + 7
                              + ((Subtitle != NULL) ? 2 : 0));
  Top = (Content >= Rows) ? 0 : (Rows - Content) / 2;

  while (Top-- > 0) {
    Print (L"\r\n");
  }

  SfbPanelBorder ();
  SfbPanelBlank ();
  SfbPanelCentered (Title, SFB_ATTR_TITLE);
  SfbPanelBlank ();
}

VOID
SfbEndScreen (IN CONST CHAR16 *Footer)
{
  if (SfbGfxActive ()) {
    SfbGfxPanelEnd (Footer);
    return;
  }

  UINTN  Index;

  SfbPanelBlank ();
  if (gSfbPanelSubtitle != NULL) {
    SfbPanelCentered (gSfbPanelSubtitle, SFB_ATTR_FOOTER);
    SfbPanelBlank ();
  }
  if (Footer != NULL) {
    SfbPanelCentered (Footer, SFB_ATTR_FOOTER);
  }
  for (Index = 0; Index < gSfbPanelExtra; Index++) {
    SfbPanelBlank ();
  }
  SfbPanelBorder ();
}

VOID
SfbDrawRow (IN BOOLEAN Selected, IN BOOLEAN Enabled,
            IN CONST CHAR16 *Marker, IN CONST CHAR16 *Text)
{
  if (SfbGfxActive ()) {
    SfbGfxPanelRow (Selected, Enabled, Marker, Text);
    return;
  }

  CHAR16  Inner[SFB_PANEL_MAX_WIDTH + 1];
  UINTN   Width = gSfbPanelWidth;
  UINTN   InnerWidth;
  UINTN   MarkerLen;
  UINTN   TextLen;
  UINTN   MaxText;
  UINTN   Index;

  InnerWidth = (Width >= 2) ? Width - 2 : 0;
  MarkerLen = (Marker != NULL) ? StrLen (Marker) : 0;
  if (MarkerLen > 4) {
    MarkerLen = 4;
  }
  if (InnerWidth < 3) {
    MarkerLen = 0;
    MaxText = 0;
  } else {
    if (MarkerLen > InnerWidth - 2) {
      MarkerLen = InnerWidth - 2;
    }
    MaxText = InnerWidth - MarkerLen - 2;
  }
  TextLen = StrLen (Text);
  if (TextLen > MaxText) {
    TextLen = MaxText;
  }

  Index = 0;
  Inner[Index++] = L' ';
  CopyMem (&Inner[Index], Marker, MarkerLen * sizeof (CHAR16));
  Index += MarkerLen;
  Inner[Index++] = L' ';
  CopyMem (&Inner[Index], Text, TextLen * sizeof (CHAR16));
  Index += TextLen;
  while (Index < InnerWidth) {
    Inner[Index++] = L' ';
  }
  Inner[Index] = L'\0';

  SfbPrintLeftPad ();
  gST->ConOut->SetAttribute (gST->ConOut, SFB_ATTR_PANEL);
  Print (L"|");
  gST->ConOut->SetAttribute (
    gST->ConOut,
    !Enabled ? SFB_ATTR_DISABLED
             : (Selected ? SFB_ATTR_SELECTED : SFB_ATTR_NORMAL));
  Print (L"%s", Inner);
  gST->ConOut->SetAttribute (gST->ConOut, SFB_ATTR_PANEL);
  Print (L"|\r\n");
  gST->ConOut->SetAttribute (gST->ConOut, SFB_ATTR_NORMAL);
}

/*
 * First row of the visible window, keeping the cursor inside it. Lists longer
 * than the window scroll rather than overflow the console.
 */
UINTN
SfbWindowStart (IN UINTN Cursor, IN UINTN Count, IN UINTN Rows)
{
  if (Count <= Rows) {
    return 0;
  }
  if (Cursor < Rows / 2) {
    return 0;
  }
  if (Cursor > Count - 1 - (Rows - Rows / 2 - 1)) {
    return Count - Rows;
  }

  return Cursor - Rows / 2;
}

VOID
SfbMoveCursor (IN OUT UINTN *Cursor, IN UINTN Count, IN SFB_KEY Key)
{
  if (Count == 0) {
    *Cursor = 0;
    return;
  }

  if (Key == SfbKeyUp) {
    *Cursor = (*Cursor == 0) ? Count - 1 : *Cursor - 1;
  } else if (Key == SfbKeyDown) {
    *Cursor = (*Cursor + 1 >= Count) ? 0 : *Cursor + 1;
  }
}

/*
 * Draw a small centred banner panel with an optional second line, used by the
 * transient screens (entering menu, booting, fastboot mode, power actions and
 * status reports).
 */
STATIC
VOID
SfbShowBanner (IN CONST CHAR16 *Text, IN CONST CHAR16 *Detail OPTIONAL)
{
  if (SfbGfxActive ()) {
    SfbGfxShowBanner (Text, Detail);
    return;
  }

  UINTN  Cols;
  UINTN  Rows;
  UINTN  Height;
  UINTN  Top;
  UINTN  Index;

  gST->ConOut->SetAttribute (gST->ConOut, SFB_ATTR_TITLE);
  gST->ConOut->ClearScreen (gST->ConOut);
  gST->ConOut->EnableCursor (gST->ConOut, FALSE);

  SfbScreenSize (&Cols, &Rows);
  gSfbPanelWidth = SfbPanelWidth ();
  gSfbPanelLeft  = (Cols - gSfbPanelWidth) / 2;

  /* border, blank, text, [blank, detail,] border */
  Height = (Detail != NULL) ? 5 : 3;
  Top = (Height >= Rows) ? 0 : (Rows - Height) / 2;
  for (Index = 0; Index < Top; Index++) {
    Print (L"\r\n");
  }

  SfbPanelBorder ();
  SfbPanelBlank ();
  SfbPanelCentered (Text, SFB_ATTR_TITLE);
  if (Detail != NULL) {
    SfbPanelBlank ();
    SfbPanelCentered (Detail, SFB_ATTR_NORMAL);
  }
  SfbPanelBorder ();

  gST->ConOut->SetAttribute (gST->ConOut, SFB_ATTR_NORMAL);
}

/* Report a failure and hold the screen until the user acknowledges it. */
VOID
SfbReportStatus (IN CONST CHAR16 *What, IN EFI_STATUS Status)
{
  CHAR16  Text[SFB_DESC_CHARS + 32];

  UnicodeSPrint (Text, sizeof (Text), L"%s: %r", What, Status);
  SfbShowBanner (Text, SfbStr (StrPressPower));
  SfbWaitForKey (0);
}

/* ---- "Pretentious Mode" log stream and art ------------------------------ */

/*
 * Pretentious Mode has two separate display styles:
 *   - log mode (optimized or classic): the transient loading screens show a
 *     wall of related but meaningless log lines built from professional boot
 *     terms ("Kernel Patching", "Verifying boot image", ...).  Each line
 *     starts with a pseudo-random [PID NNNN].  The optimized style draws the
 *     glyphs straight onto the frame buffer, blended over the existing
 *     background, so the text takes exactly as many pixels as its characters
 *     need with no black padding; the classic style prints the same lines on
 *     the SimpleFont console, the way the earlier branch did.
 *   - art mode: no logs at all; the character chosen in the "Character"
 *     setting is shown centered on the screen, glyphs scaled up directly on
 *     a pure-black background.
 *   - custom mode: no logs; the image from \efisp\logo.bmp on the ext4
 *     persist volume is shown centered on the physical screen.  Missing or
 *     invalid images are silently ignored so the launch never fails.
 *
 * In log mode the boot path interleaves the stream with the real loading
 * steps, so printing never delays the launch.
 */
#define SFB_PRETENTIOUS_LINES    160
#define SFB_PRETENTIOUS_DELAY_MS 2
#define SFB_ART_CELL_PX          10

#define SFB_ATTR_LOG_NORMAL  EFI_TEXT_ATTR (EFI_LIGHTGRAY, EFI_BLACK)

typedef struct {
  CONST CHAR16  *Text;
  UINTN         Attr;    /* SimpleFont console attribute */
  UINT32        Color;   /* frame-buffer glyph colour */
} SFB_PRETENTIOUS_STATUS;

STATIC CONST CHAR16  *mSfbPretentiousModule[10] = {
  L"abl", L"sbl", L"xbl", L"hyp", L"tz",
  L"aop", L"uefi", L"boot", L"kernel", L"devcfg",
};

STATIC CONST CHAR16  *mSfbPretentiousAction[16] = {
  L"Kernel Patching",
  L"Verifying boot image",
  L"Decompressing kernel",
  L"Loading device tree",
  L"Mapping page tables",
  L"Enabling MMU",
  L"Initializing DMA",
  L"Probing storage",
  L"Enumerating partitions",
  L"Configuring clocks",
  L"Calibrating regulators",
  L"Flushing caches",
  L"Starting watchdog",
  L"Setting up console",
  L"Handing off to kernel",
  L"Validating signatures",
};

STATIC CONST SFB_PRETENTIOUS_STATUS  mSfbPretentiousStatus[5] = {
  { L"[ OK ]",  EFI_TEXT_ATTR (EFI_GREEN, EFI_BLACK),   SFB_COLOR_OK },
  { L"[ OK ]",  EFI_TEXT_ATTR (EFI_GREEN, EFI_BLACK),   SFB_COLOR_OK },
  { L"[ OK ]",  EFI_TEXT_ATTR (EFI_GREEN, EFI_BLACK),   SFB_COLOR_OK },
  { L"[WARN]",  EFI_TEXT_ATTR (EFI_YELLOW, EFI_BLACK),  SFB_COLOR_WARN },
  { L"[SKIP]",  EFI_TEXT_ATTR (EFI_LIGHTCYAN, EFI_BLACK), SFB_COLOR_SKIP },
};

/* Art choices, always shown in Chinese regardless of the UI language. */
STATIC CONST CHAR16  *mSfbPretentiousArtText[4] = {
  L"豪情在天", L"嘉豪", L"嘉欣", L"豪",
};

STATIC BOOLEAN  gSfbPretentiousActive = FALSE;
STATIC UINTN    gSfbPretentiousIndex = 0;
STATIC UINT32   gSfbPretentiousY = 0;
STATIC UINTN    gSfbPretentiousMode = SFB_PRETENTIOUS_LOG_OPTIMIZED;

VOID
SfbPretentiousBegin (VOID)
{
  SFB_SETTINGS  S;

  SfbSettingsGet (&S);
  gSfbPretentiousActive = TRUE;
  gSfbPretentiousIndex = 0;
  /* Top margin: keep the first lines clear of the phone's rounded corner. */
  gSfbPretentiousY = 100;
  gSfbPretentiousMode = S.PretentiousMode;
}

/*
 * Show the chosen text centered on the screen: the embedded font glyphs are
 * scaled up directly (no block conversion), drawn on a pure-black background.
 * Art mode outputs no log lines.
 */
STATIC
VOID
SfbPretentiousShowArt (IN UINTN Choice)
{
  CONST CHAR16  *Art = mSfbPretentiousArtText[Choice %
                                              ARRAY_SIZE (mSfbPretentiousArtText)];
  UINT32        W;
  UINT32        H;
  UINT32        Scale;
  UINT32        ArtW;
  UINT32        ArtH;
  UINT32        X;
  UINT32        Y;

  if (SfbGfxActive ()) {
    SfbGfxGetScreen (&W, &H);

    /* Pick the largest scale that fits the screen with margins; the glyphs
     * are bilinearly smoothed, so large scales stay crisp and even. */
    Scale = 18;
    while (Scale > 2) {
      ArtW = SfbGfxTextWidth (Art) * Scale;
      ArtH = SFB_FONT_CELL_H * Scale;
      if (ArtW <= W - 20 && ArtH <= H - 40) {
        break;
      }
      Scale--;
    }
    ArtW = SfbGfxTextWidth (Art) * Scale;
    ArtH = SFB_FONT_CELL_H * Scale;
    X = (W >= ArtW) ? (W - ArtW) / 2 : 0;
    Y = (H >= ArtH) ? (H - ArtH) / 2 : 0;

    /* Pure-black background behind the scaled glyphs. */
    SfbGfxFillRect (X - 12, Y - 12, ArtW + 24, ArtH + 24, SFB_COLOR_BLACK);
    SfbGfxDrawTextScaled (Art, X, Y, Scale, SFB_COLOR_ACCENT, SFB_COLOR_BLACK);
  } else {
    UINTN  Cols;
    UINTN  Rows;
    UINTN  Col;

    SfbScreenSize (&Cols, &Rows);
    Col = (StrLen (Art) >= Cols) ? 0 : (Cols - StrLen (Art)) / 2;
    gST->ConOut->SetAttribute (gST->ConOut, SFB_ATTR_TITLE);
    gST->ConOut->SetCursorPosition (gST->ConOut, Col, Rows / 2);
    Print (L"%s", Art);
    gST->ConOut->SetAttribute (gST->ConOut, SFB_ATTR_NORMAL);
  }
}

/*
 * Custom Mode: read \efisp\logo.bmp (a BMP image) from the project's already
 * mounted boot volumes (ext4 persist first, FAT32 fallback) and show it
 * centered on the physical screen.  Any failure - file missing or unreadable,
 * not a BMP, unsupported format, image out of range, no GOP - is silently
 * ignored so the launch always continues.
 */
#define SFB_LOGO_MAX_BYTES  (16 * 1024 * 1024)
#define SFB_LOGO_MAX_DIM    4096

STATIC
VOID
SfbPretentiousShowLogo (VOID)
{
  EFI_STATUS                       Status;
  EFI_HANDLE                       *Volumes = NULL;
  UINTN                            VolumeCount = 0;
  UINTN                            VolumeIndex;
  UINT8                            *Buffer = NULL;
  UINTN                            BytesRead = 0;
  BOOLEAN                          Found = FALSE;
  UINT32                           PixelOffset;
  UINT32                           HeightRaw;
  UINT32                           Width;
  UINT32                           Height;
  UINT16                           Bpp;
  UINT32                           Compression;
  UINT32                           RowSize;
  UINT32                           RowIndex;
  BOOLEAN                          TopDown;
  UINT32                           ScreenW;
  UINT32                           ScreenH;
  UINT32                           X;
  UINT32                           Y;
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL    *Pixels = NULL;

  if (!SfbGfxActive ()) {
    return;
  }

  Status = SfbLocateVolumes (&Volumes, &VolumeCount);
  if (EFI_ERROR (Status) || Volumes == NULL) {
    DEBUG ((EFI_D_ERROR, "SFB: logo: no volumes: %r\n", Status));
    goto done;
  }

  Buffer = AllocateZeroPool (SFB_LOGO_MAX_BYTES);
  if (Buffer == NULL) {
    DEBUG ((EFI_D_ERROR, "SFB: logo: out of memory\n"));
    goto done;
  }

  /* Try the project's own mounted volumes: ext4 persist has the \efisp boot
   * root, FAT32 media may carry an efisp folder too. */
  for (VolumeIndex = 0; VolumeIndex < VolumeCount; VolumeIndex++) {
    EFI_FILE_PROTOCOL  *Root = NULL;

    Status = SfbOpenVolumeRoot (Volumes[VolumeIndex], &Root);
    if (EFI_ERROR (Status) || Root == NULL) {
      continue;
    }

    BytesRead = 0;
    Status = SfbReadFileBytes (Root, L"\\efisp\\logo.bmp", Buffer,
                               SFB_LOGO_MAX_BYTES, &BytesRead);
    Root->Close (Root);

    if (!EFI_ERROR (Status) && BytesRead >= 54) {
      Found = TRUE;
      break;
    }
    DEBUG ((EFI_D_VERBOSE, "SFB: logo: volume %u read failed: %r\n",
            (UINT32)VolumeIndex, Status));
  }

  if (!Found) {
    DEBUG ((EFI_D_INFO, "SFB: logo: \\efisp\\logo.bmp not readable\n"));
    goto done;
  }

  /* BITMAPFILEHEADER ('BM') + a DIB header of at least 40 bytes (accepts
   * BITMAPINFOHEADER as well as V4/V5), uncompressed 24/32bpp. */
  if (Buffer[0] != 'B' || Buffer[1] != 'M' || Buffer[14] < 40) {
    DEBUG ((EFI_D_ERROR, "SFB: logo: not a BMP\n"));
    goto done;
  }
  PixelOffset = (UINT32)Buffer[10] | ((UINT32)Buffer[11] << 8) |
                ((UINT32)Buffer[12] << 16) | ((UINT32)Buffer[13] << 24);
  Width  = (UINT32)Buffer[18] | ((UINT32)Buffer[19] << 8) |
           ((UINT32)Buffer[20] << 16) | ((UINT32)Buffer[21] << 24);
  HeightRaw = (UINT32)Buffer[22] | ((UINT32)Buffer[23] << 8) |
              ((UINT32)Buffer[24] << 16) | ((UINT32)Buffer[25] << 24);
  TopDown = (HeightRaw & 0x80000000) != 0;
  Height  = TopDown ? (0U - HeightRaw) : HeightRaw;
  Bpp = (UINT16)(Buffer[28] | ((UINT16)Buffer[29] << 8));
  Compression = (UINT32)Buffer[30] | ((UINT32)Buffer[31] << 8) |
                ((UINT32)Buffer[32] << 16) | ((UINT32)Buffer[33] << 24);

  if (Width == 0 || Height == 0 ||
      Width > SFB_LOGO_MAX_DIM || Height > SFB_LOGO_MAX_DIM ||
      (Bpp != 24 && Bpp != 32) || Compression != 0) {
    DEBUG ((EFI_D_ERROR, "SFB: logo: unsupported %ux%u bpp=%u comp=%u\n",
            Width, Height, Bpp, Compression));
    goto done;
  }
  RowSize = ((Bpp * Width + 31) / 32) * 4;
  if ((UINTN)PixelOffset + (UINTN)RowSize * Height > BytesRead) {
    DEBUG ((EFI_D_ERROR, "SFB: logo: truncated image data\n"));
    goto done;
  }

  Pixels = AllocateZeroPool (Width * Height * sizeof (*Pixels));
  if (Pixels == NULL) {
    DEBUG ((EFI_D_ERROR, "SFB: logo: pixel buffer allocation failed\n"));
    goto done;
  }

  for (RowIndex = 0; RowIndex < Height; RowIndex++) {
    UINT32                            SrcRow = TopDown ? RowIndex
                                                       : Height - 1 - RowIndex;
    UINT8                             *Src = Buffer + PixelOffset +
                                              SrcRow * RowSize;
    EFI_GRAPHICS_OUTPUT_BLT_PIXEL     *Dst = &Pixels[RowIndex * Width];
    UINT32                            Col;

    for (Col = 0; Col < Width; Col++) {
      Dst[Col].Blue     = Src[Col * (Bpp / 8)];
      Dst[Col].Green    = Src[Col * (Bpp / 8) + 1];
      Dst[Col].Red      = Src[Col * (Bpp / 8) + 2];
      Dst[Col].Reserved = 0;
    }
  }

  SfbGfxGetScreen (&ScreenW, &ScreenH);
  X = (ScreenW >= Width) ? (ScreenW - Width) / 2 : 0;
  Y = (ScreenH >= Height) ? (ScreenH - Height) / 2 : 0;

  /* Clean black backdrop, then the logo, absolutely centred. */
  SfbGfxClear (SFB_COLOR_BG);
  SfbGfxBltImage (X, Y, Width, Height, Pixels);
  DEBUG ((EFI_D_INFO, "SFB: logo: shown %ux%u at %u,%u\n",
          Width, Height, X, Y));

done:
  if (Pixels != NULL) {
    FreePool (Pixels);
  }
  if (Buffer != NULL) {
    FreePool (Buffer);
  }
  if (Volumes != NULL) {
    FreePool (Volumes);
  }
}

BOOLEAN
SfbPretentiousEmit (IN UINTN Count)
{
  UINTN  Index;

  if (!gSfbPretentiousActive) {
    return FALSE;
  }

  for (Index = 0;
       Index < Count && gSfbPretentiousIndex < SFB_PRETENTIOUS_LINES;
       Index++, gSfbPretentiousIndex++) {
    UINTN   Mod  = (gSfbPretentiousIndex * 7 + 3) %
                   ARRAY_SIZE (mSfbPretentiousModule);
    UINTN   Act  = (gSfbPretentiousIndex * 5 + 1) %
                   ARRAY_SIZE (mSfbPretentiousAction);
    UINTN   St   = (gSfbPretentiousIndex * 3 + 2) %
                   ARRAY_SIZE (mSfbPretentiousStatus);
    UINT32  Pid  = (UINT32)((gSfbPretentiousIndex * 7919 + 12345) % 9999);
    CHAR16  Line[SFB_DESC_CHARS + 16];

    UnicodeSPrint (Line, sizeof (Line), L"[PID %04u] %s: %s ... ",
                   Pid, mSfbPretentiousModule[Mod], mSfbPretentiousAction[Act]);

    if (SfbGfxActive () && gSfbPretentiousMode == SFB_PRETENTIOUS_LOG_OPTIMIZED) {
      UINT32  W;
      UINT32  H;
      UINT32  Bottom;

      SfbGfxGetScreen (&W, &H);
      /* Bottom margin: stop drawing below the 90px safe area. */
      Bottom = (H > 90) ? H - 90 : 0;
      if (gSfbPretentiousY + SFB_FONT_CELL_H <= Bottom) {
        SfbGfxDrawTextTransparent (Line, 0, gSfbPretentiousY, SFB_COLOR_TEXT);
        SfbGfxDrawTextTransparent (mSfbPretentiousStatus[St].Text,
                                   SfbGfxTextWidth (Line), gSfbPretentiousY,
                                   mSfbPretentiousStatus[St].Color);
      }
    } else {
      /* Classic style (or no GOP): plain SimpleFont console output. */
      gST->ConOut->SetAttribute (gST->ConOut, SFB_ATTR_LOG_NORMAL);
      Print (L"%s", Line);
      gST->ConOut->SetAttribute (gST->ConOut, mSfbPretentiousStatus[St].Attr);
      Print (L"%s\r\n", mSfbPretentiousStatus[St].Text);
    }

    if (SfbGfxActive () && gSfbPretentiousMode == SFB_PRETENTIOUS_LOG_OPTIMIZED) {
      gSfbPretentiousY += SFB_FONT_CELL_H + 4;
    }
  }

  return (BOOLEAN)(gSfbPretentiousIndex < SFB_PRETENTIOUS_LINES);
}

/*
 * Hand the screen over to fastboot. The menu is the last thing that draws
 * before control leaves for the fastboot loop, which prints nothing of its own
 * until a host connects, so without this the user would be staring at a boot
 * menu that no longer responds to anything.  Plain SimpleFont text, matching
 * the fastboot mode screen the library draws next.  In Pretentious Mode this
 * becomes the meaningless-log dump.
 */
VOID
SfbShowFastbootMode (VOID)
{
  SFB_SETTINGS  S;

  SfbSettingsGet (&S);
  if (S.Pretentious) {
    if (S.PretentiousMode == SFB_PRETENTIOUS_ART) {
      SfbPretentiousShowArt (S.PretentiousArt);
    } else if (S.PretentiousMode == SFB_PRETENTIOUS_CUSTOM) {
      SfbPretentiousShowLogo ();
    } else {
      SfbPretentiousBegin ();
      while (SfbPretentiousEmit (SFB_PRETENTIOUS_BATCH)) {
        gBS->Stall (SFB_PRETENTIOUS_DELAY_MS * 1000);
      }
    }
    return;
  }

  gST->ConOut->SetAttribute (gST->ConOut, SFB_ATTR_TITLE);
  gST->ConOut->ClearScreen (gST->ConOut);
  gST->ConOut->EnableCursor (gST->ConOut, FALSE);
  Print (L"FASTBOOT MODE\r\n\r\n");
  Print (L"Connect a host and run fastboot commands.\r\n");
  gST->ConOut->SetAttribute (gST->ConOut, SFB_ATTR_NORMAL);
}

/*
 * Clear the menu away and announce the launch. The loaded image prints nothing
 * of its own until it takes over, so without this the boot menu would linger on
 * screen through the load.
 *
 * The prompt itself stays plain English ("Booting <name>") in the firmware's
 * SimpleFont.  No panel or decoration is drawn around it; SfbBootLog appends
 * every task log line underneath, one line per row.
 */
/* Lines of boot progress drawn under the prompt; reset per launch. */
STATIC UINTN  gSfbBootLogCount = 0;

VOID
SfbShowBootingScreen (IN CONST CHAR16 *Name, IN BOOLEAN ClearScreen)
{
  CHAR16       Text[SFB_DESC_CHARS + 16];
  SFB_SETTINGS S;

  SfbSettingsGet (&S);
  gSfbBootLogCount = 0;

  if (!S.ShowBooting) {
    /* The "Booting" banner is switched off: clear a menu-driven screen so the
     * next image starts on a clean display, and leave an unattended boot's
     * splash untouched. */
    if (ClearScreen) {
      if (SfbGfxActive ()) {
        SfbGfxClear (SFB_COLOR_BG);
      } else {
        gST->ConOut->SetAttribute (gST->ConOut, SFB_ATTR_NORMAL);
        gST->ConOut->ClearScreen (gST->ConOut);
      }
    }
    return;
  }

  if (S.Pretentious) {
    if (S.PretentiousMode == SFB_PRETENTIOUS_ART) {
      SfbPretentiousShowArt (S.PretentiousArt);
    } else if (S.PretentiousMode == SFB_PRETENTIOUS_CUSTOM) {
      SfbPretentiousShowLogo ();
    } else {
      SfbPretentiousBegin ();
      SfbPretentiousEmit (SFB_PRETENTIOUS_BATCH);
    }
    return;
  }

  /* Always English: the boot prompt is not part of the translated UI. */
  UnicodeSPrint (Text, sizeof (Text), L"Booting %s",
                 (Name != NULL && Name[0] != L'\0') ? Name : L"...");

  {
    UINTN  Cols;
    UINTN  Rows;

    gST->ConOut->SetAttribute (gST->ConOut, SFB_ATTR_TITLE);
    gST->ConOut->EnableCursor (gST->ConOut, FALSE);
    if (ClearScreen) {
      gST->ConOut->ClearScreen (gST->ConOut);
    }
    /* SimpleFont; the boot log is printed underneath. */
    SfbScreenSize (&Cols, &Rows);
    gST->ConOut->SetCursorPosition (gST->ConOut, 0, 0);
    Print (L"%s", Text);
    gST->ConOut->SetAttribute (gST->ConOut, SFB_ATTR_NORMAL);
  }
}

/*
 * Append one line of boot progress under the "Booting" prompt, plain
 * SimpleFont text, one line per row from row 1 down.  Every task log line is
 * printed until the console runs out of rows.  Honors the "Show Booting
 * screen" setting, so turning the prompt off also silences the log;
 * Pretentious Mode replaces the prompt with its own dump.
 */
VOID
SfbBootLog (IN CONST CHAR16 *Text)
{
  SFB_SETTINGS  S;

  if (Text == NULL) {
    return;
  }

  SfbSettingsGet (&S);
  if (!S.ShowBooting || S.Pretentious) {
    return;
  }

  {
    UINTN  Cols;
    UINTN  Rows;
    UINTN  Row;

    SfbScreenSize (&Cols, &Rows);
    if (Rows == 0 || gSfbBootLogCount + 1 >= Rows) {
      return;
    }
    Row = 1 + gSfbBootLogCount;
    gST->ConOut->SetCursorPosition (gST->ConOut, 0, Row);
    gST->ConOut->SetAttribute (gST->ConOut, SFB_ATTR_FOOTER);
    Print (L"%s", Text);
    gST->ConOut->SetAttribute (gST->ConOut, SFB_ATTR_NORMAL);
  }

  gSfbBootLogCount++;
}

/* ---- PIN and settings ---------------------------------------------------- */

STATIC
VOID
SfbGfxShowPinScreen (IN CONST CHAR16 *Title, IN CONST UINT8 *Digit,
                     IN UINTN Pos)
{
  UINT32  W;
  UINT32  H;
  UINT32  CellW;
  UINT32  Gap;
  UINT32  TotalW;
  UINT32  X0;
  UINT32  Y0;
  UINTN   Index;
  CHAR16  Ch[2];

  SfbGfxGetScreen (&W, &H);
  /* Enlarge the PIN UI to 2x the embedded font's 32px cell, so the digits are
   * rendered at the largest size the font data supports. */
  CellW = 2 * SFB_FONT_MAX_W;
  Gap = SFB_FONT_MAX_W;
  TotalW = SFB_PIN_DIGITS * CellW + (SFB_PIN_DIGITS - 1) * Gap;
  X0 = (W >= TotalW) ? (W - TotalW) / 2 : 0;
  Y0 = H / 2 - 2 * SFB_FONT_CELL_H - 16;

  SfbGfxClear (SFB_COLOR_BG);
  SfbGfxDrawCentered (Title, W, Y0 - 96, SFB_COLOR_ACCENT, SFB_COLOR_BG);

  for (Index = 0; Index < SFB_PIN_DIGITS; Index++) {
    UINT32  CX = X0 + (UINT32)Index * (CellW + Gap);
    UINT32  DigitW;

    Ch[0] = (CHAR16)(L'0' + Digit[Index]);
    Ch[1] = L'\0';
    DigitW = SfbGfxTextWidth (Ch) * 2;

    if (Index < Pos) {
      SfbGfxFillRect (CX, Y0, CellW, 2 * SFB_FONT_CELL_H + 12,
                      SFB_COLOR_ACCENT_D);
      SfbGfxDrawTextScaled (Ch, CX + (CellW - DigitW) / 2, Y0 + 6, 2,
                            SFB_COLOR_BG, SFB_COLOR_ACCENT_D);
    } else if (Index == Pos) {
      SfbGfxFillRect (CX, Y0, CellW, 2 * SFB_FONT_CELL_H + 12,
                      SFB_COLOR_ACCENT);
      SfbGfxDrawTextScaled (Ch, CX + (CellW - DigitW) / 2, Y0 + 6, 2,
                            SFB_COLOR_BG, SFB_COLOR_ACCENT);
    } else {
      SfbGfxFillRect (CX, Y0, CellW, 2 * SFB_FONT_CELL_H + 12,
                      SFB_COLOR_PANEL);
      SfbGfxHLine (Y0, CX, CX + CellW - 1, 2, SFB_COLOR_ACCENT_D);
    }
  }

  SfbGfxDrawCentered (SfbStr (StrKeyNavPin), W, Y0 + 2 * SFB_FONT_CELL_H + 44,
                      SFB_COLOR_ACCENT_D, SFB_COLOR_BG);
}

STATIC
VOID
SfbTextShowPinScreen (IN CONST CHAR16 *Title, IN CONST UINT8 *Digit,
                      IN UINTN Pos)
{
  UINTN  Index;

  gST->ConOut->SetAttribute (gST->ConOut, SFB_ATTR_TITLE);
  gST->ConOut->ClearScreen (gST->ConOut);
  Print (L"%s\r\n\r\n", Title);
  for (Index = 0; Index < SFB_PIN_DIGITS; Index++) {
    gST->ConOut->SetAttribute (
      gST->ConOut, (Index == Pos) ? SFB_ATTR_SELECTED : SFB_ATTR_NORMAL);
    Print (L"  %d  ", Digit[Index]);
  }
  gST->ConOut->SetAttribute (gST->ConOut, SFB_ATTR_NORMAL);
  Print (L"\r\n\r\n%s\r\n", SfbStr (StrKeyNavPin));
}

/*
 * Enter a 4-digit PIN: volume up/down cycles the current digit, power locks
 * it in and moves to the next.  Returns TRUE after the fourth digit.
 */
STATIC
BOOLEAN
SfbGetPin (IN CONST CHAR16 *Title, OUT CHAR16 Pin[SFB_PIN_DIGITS + 1])
{
  UINT8   Digit[SFB_PIN_DIGITS];
  UINTN   Pos = 0;
  UINTN   Index;
  SFB_KEY Key;

  for (Index = 0; Index < SFB_PIN_DIGITS; Index++) {
    Digit[Index] = 0;
  }

  while (TRUE) {
    if (SfbGfxActive ()) {
      SfbGfxShowPinScreen (Title, Digit, Pos);
    } else {
      SfbTextShowPinScreen (Title, Digit, Pos);
    }

    Key = SfbWaitForKey (0);
    if (Key == SfbKeyUp) {
      Digit[Pos] = (UINT8)((Digit[Pos] + 1) % 10);
    } else if (Key == SfbKeyDown) {
      Digit[Pos] = (UINT8)((Digit[Pos] + 9) % 10);
    } else {
      Pos++;
      if (Pos == SFB_PIN_DIGITS) {
        for (Index = 0; Index < SFB_PIN_DIGITS; Index++) {
          Pin[Index] = (CHAR16)(L'0' + Digit[Index]);
        }
        Pin[SFB_PIN_DIGITS] = L'\0';
        return TRUE;
      }
    }
  }
}

/* Ask for the current PIN; only returns TRUE when it matches. */
STATIC
BOOLEAN
SfbRequireCurrentPin (VOID)
{
  CHAR16  Pin[SFB_PIN_DIGITS + 1];

  if (!SfbGetPin (SfbStr (StrEnterPin), Pin)) {
    return FALSE;
  }
  if (!SfbSettingsCheckPin (Pin)) {
    SfbShowBanner (SfbStr (StrWrongPin), SfbStr (StrPressPower));
    SfbWaitForKey (0);
    return FALSE;
  }
  return TRUE;
}

/* Enter a new PIN twice; only returns TRUE when both entries match. */
STATIC
BOOLEAN
SfbSetNewPin (IN OUT SFB_SETTINGS *S)
{
  CHAR16  Pin1[SFB_PIN_DIGITS + 1];
  CHAR16  Pin2[SFB_PIN_DIGITS + 1];

  if (!SfbGetPin (SfbStr (StrEnterNewPin), Pin1)) {
    return FALSE;
  }
  if (!SfbGetPin (SfbStr (StrConfirmPin), Pin2)) {
    return FALSE;
  }
  if (StrnCmp (Pin1, Pin2, SFB_PIN_DIGITS) != 0) {
    SfbShowBanner (SfbStr (StrPinMismatch), SfbStr (StrPressPower));
    SfbWaitForKey (0);
    return FALSE;
  }

  StrnCpyS (S->Pin, SFB_PIN_DIGITS + 1, Pin1, SFB_PIN_DIGITS);
  return TRUE;
}

BOOLEAN
SfbRunPinGate (VOID)
{
  SFB_SETTINGS  S;
  CHAR16        Pin[SFB_PIN_DIGITS + 1];

  SfbSettingsGet (&S);
  if (!S.PinEnabled) {
    return TRUE;
  }

  while (TRUE) {
    if (SfbGetPin (SfbStr (StrPinLocked), Pin) &&
        SfbSettingsCheckPin (Pin)) {
      return TRUE;
    }
    SfbShowBanner (SfbStr (StrWrongPin), SfbStr (StrWrongPinHint));
    /* Drop any volume-key repeats left over from digit entry, so a key held
     * while confirming the last digit cannot trigger an action by accident. */
    gST->ConIn->Reset (gST->ConIn, FALSE);
    if (SfbWaitForKey (0) == SfbKeyDown) {
      /* Volume Down on the wrong-PIN prompt: power the device off. */
      SfbShowActionScreen (SfbStr (StrPoweringOff));
      ShutdownDevice ();
    }
  }
}

#define SFB_SETTINGS_ROW_COUNT  10

/* TRUE when the settings row may be selected; disabled rows are dimmed and
 * skipped by the cursor.  "Change PIN" needs the PIN on, the key-detect
 * duration needs "Boot to Menu" off, the Mode row needs Pretentious Mode on,
 * and the Character row additionally needs the mode set to character art. */
STATIC
BOOLEAN
SfbSettingsRowEnabled (IN CONST SFB_SETTINGS *S, IN UINTN Index)
{
  if (Index == 2) {
    return S->PinEnabled;
  }
  if (Index == 5) {
    return !S->BootToMenu;
  }
  if (Index == 7) {
    return S->Pretentious;
  }
  if (Index == 8) {
    return (BOOLEAN)(S->Pretentious &&
                     S->PretentiousMode == SFB_PRETENTIOUS_ART);
  }
  return TRUE;
}

VOID
SfbRunSettingsMenu (VOID)
{
  SFB_SETTINGS  S;
  UINTN         Cursor = 0;
  BOOLEAN       Rebuild = TRUE;
  BOOLEAN       Adjusting = FALSE;
  SFB_KEY       Key;
  EFI_STATUS    SaveStatus = EFI_SUCCESS;

  while (TRUE) {
    UINTN   Index;
    CHAR16  RowText[SFB_DESC_CHARS + 8];
    CHAR16  ValueBuf[16];

    if (Rebuild) {
      SfbSettingsGet (&S);
      Rebuild = FALSE;
    }

    /* The settings list fits one screen in full. */
    SfbBeginScreen (SfbStr (StrSettings), NULL);

    for (Index = 0; Index < SFB_SETTINGS_ROW_COUNT; Index++) {
      CONST CHAR16  *Label = NULL;
      CONST CHAR16  *Value = NULL;

      switch (Index) {
      case 0:
        Label = SfbStr (StrLanguage);
        Value = (S.Lang == SfbLangEn) ? SfbStr (StrLangEnglish)
                                      : SfbStr (StrLangChinese);
        break;
      case 1:
        Label = SfbStr (StrPinLock);
        Value = S.PinEnabled ? SfbStr (StrEnabled) : SfbStr (StrDisabled);
        break;
      case 2:
        Label = SfbStr (StrChangePin);
        break;
      case 3:
        Label = SfbStr (StrShowBooting);
        Value = S.ShowBooting ? SfbStr (StrOn) : SfbStr (StrOff);
        break;
      case 4:
        Label = SfbStr (StrBootToMenu);
        Value = S.BootToMenu ? SfbStr (StrOn) : SfbStr (StrOff);
        break;
      case 5:
        Label = SfbStr (StrVolumeKeyTimeout);
        UnicodeSPrint (ValueBuf, sizeof (ValueBuf), SfbStr (StrSecondsFmt),
                       (UINT32)S.VolumeKeyTimeoutSec);
        Value = ValueBuf;
        break;
      case 6:
        Label = SfbStr (StrPretentious);
        Value = S.Pretentious ? SfbStr (StrOn) : SfbStr (StrOff);
        break;
      case 7:
        Label = SfbStr (StrPretentiousMode);
        Value = (S.PretentiousMode == SFB_PRETENTIOUS_LOG_CLASSIC)
                  ? SfbStr (StrLogClassic)
                  : (S.PretentiousMode == SFB_PRETENTIOUS_ART)
                      ? SfbStr (StrArtMode)
                      : (S.PretentiousMode == SFB_PRETENTIOUS_CUSTOM)
                          ? SfbStr (StrCustomMode)
                          : SfbStr (StrLogOptimized);
        break;
      case 8:
        Label = SfbStr (StrPretentiousArt);
        Value = (S.PretentiousArt == 1) ? SfbStr (StrArtJiaHao)
               : (S.PretentiousArt == 2) ? SfbStr (StrArtJiaXin)
               : (S.PretentiousArt == 3) ? SfbStr (StrArtHao)
                                         : SfbStr (StrArtHaoQing);
        break;
      case 9:
      default:
        Label = SfbStr (StrBack);
        break;
      }

      if (Label == NULL) {
        Label = L"";
      }
      if (Value != NULL) {
        UnicodeSPrint (RowText, sizeof (RowText), L"%s: %s", Label, Value);
      } else {
        StrnCpyS (RowText, SFB_DESC_CHARS + 8, Label, SFB_DESC_CHARS + 7);
      }
      SfbDrawRow ((BOOLEAN)(Index == Cursor),
                  SfbSettingsRowEnabled (&S, Index),
                  (Adjusting && Index == Cursor) ? L">" : L" ", RowText);
    }

    SfbEndScreen (Adjusting ? SfbStr (StrKeyNavAdjust)
                            : SfbStr (StrKeyNavSelect));

    Key = SfbWaitForKey (0);
    if (Key == SfbKeyUp || Key == SfbKeyDown) {
      if (Adjusting && Cursor == 5) {
        if ((Key == SfbKeyUp &&
             S.VolumeKeyTimeoutSec < SFB_VOL_KEY_MAX_SEC) ||
            (Key == SfbKeyDown &&
             S.VolumeKeyTimeoutSec > SFB_VOL_KEY_MIN_SEC)) {
          S.VolumeKeyTimeoutSec =
            (Key == SfbKeyUp) ? S.VolumeKeyTimeoutSec + 1
                              : S.VolumeKeyTimeoutSec - 1;
          SaveStatus = SfbSettingsSave (&S);
          Rebuild = TRUE;
          if (EFI_ERROR (SaveStatus)) {
            SfbReportStatus (SfbStr (StrConfigFailed), SaveStatus);
          }
        }
      } else {
        UINTN  Guard;

        SfbMoveCursor (&Cursor, SFB_SETTINGS_ROW_COUNT, Key);
        /* Skip dimmed, unavailable rows. */
        for (Guard = 0;
             Guard < SFB_SETTINGS_ROW_COUNT &&
             !SfbSettingsRowEnabled (&S, Cursor);
             Guard++) {
          SfbMoveCursor (&Cursor, SFB_SETTINGS_ROW_COUNT, Key);
        }
      }
      continue;
    }

    switch (Cursor) {
    case 0:
      S.Lang = (S.Lang == SfbLangEn) ? SfbLangZh : SfbLangEn;
      SaveStatus = SfbSettingsSave (&S);
      Rebuild = TRUE;
      break;

    case 1:
      if (S.PinEnabled) {
        if (SfbRequireCurrentPin ()) {
          S.PinEnabled = FALSE;
          SaveStatus = SfbSettingsSave (&S);
        }
      } else if (SfbSetNewPin (&S)) {
        S.PinEnabled = TRUE;
        SaveStatus = SfbSettingsSave (&S);
      }
      Rebuild = TRUE;
      break;

    case 2:
      if (S.PinEnabled && SfbRequireCurrentPin () && SfbSetNewPin (&S)) {
        SaveStatus = SfbSettingsSave (&S);
      }
      Rebuild = TRUE;
      break;

    case 3:
      S.ShowBooting = !S.ShowBooting;
      SaveStatus = SfbSettingsSave (&S);
      Rebuild = TRUE;
      break;

    case 4:
      S.BootToMenu = !S.BootToMenu;
      SaveStatus = SfbSettingsSave (&S);
      Rebuild = TRUE;
      break;

    case 5:
      /* Power on the duration row toggles adjust mode; volume keys then
       * change the seconds and power confirms. */
      Adjusting = !Adjusting;
      Rebuild = TRUE;
      break;

    case 6:
      S.Pretentious = !S.Pretentious;
      SaveStatus = SfbSettingsSave (&S);
      Rebuild = TRUE;
      break;

    case 7:
      if (S.Pretentious) {
        S.PretentiousMode = (S.PretentiousMode + 1) % 4;
        SaveStatus = SfbSettingsSave (&S);
      }
      Rebuild = TRUE;
      break;

    case 8:
      if (S.Pretentious && S.PretentiousMode == SFB_PRETENTIOUS_ART) {
        S.PretentiousArt = (S.PretentiousArt + 1) % 4;
        SaveStatus = SfbSettingsSave (&S);
      }
      Rebuild = TRUE;
      break;

    case 9:
    default:
      return;
    }

    if (EFI_ERROR (SaveStatus)) {
      SfbReportStatus (SfbStr (StrConfigFailed), SaveStatus);
    }
  }
}

/*
 * Announce a power action (Power Off / Restart) and leave the message on
 * screen while the reset takes effect. Neither action returns, so the screen is
 * the last thing the user sees.
 */
VOID
SfbShowActionScreen (IN CONST CHAR16 *Text)
{
  SfbShowBanner (Text, NULL);
}

/*
 * Seconds to hold on the "Entering Boot Menu" screen before the menu starts
 * taking input. Long enough that a volume key held from power-on has been
 * released, so it does not immediately move the menu cursor.
 */
#define SFB_ENTER_MENU_DELAY_S  3

VOID
SfbShowEnteringMenu (VOID)
{
  /* The "Entering Boot Menu" transition (after the PIN gate) is not affected
   * by Pretentious Mode: it always shows the plain banner. */
  SfbShowBanner (SfbStr (StrEnteringBootMenu), SfbStr (StrKeyNavSelect));

  /* Wait for the key to be released... */
  gBS->Stall (SFB_ENTER_MENU_DELAY_S * 1000 * 1000);

  /* ...then drop anything typed or held during the wait so it does not leak
   * into the menu as a spurious keypress. */
  gST->ConIn->Reset (gST->ConIn, FALSE);
}

/* ---- boot menu ---------------------------------------------------------- */

STATIC
VOID
SfbDrawMenu (IN CONST SFB_MENU_STATE *Menu,
             IN UINTN                Cursor,
             IN CONST CHAR16         *Title)
{
  UINTN  Start;
  UINTN  Index;
  UINTN  Last;
  UINTN  Visible;

  SfbBeginScreen (Title, NULL);

  if (Menu->Count == 0) {
    SfbPanelNote (SfbStr (StrNoEntries));
  }

  Visible = SfbVisibleRows ();
  Start = SfbWindowStart (Cursor, Menu->Count, Visible);
  Last = Start + Visible;
  if (Last > Menu->Count) {
    Last = Menu->Count;
  }

  for (Index = Start; Index < Last; Index++) {
    CONST SFB_BOOT_ENTRY  *Entry = &Menu->Entry[Index];
    CONST CHAR16          *Marker = (Index == Menu->DefaultIndex) ? L"*" : L" ";

    /* Submenu rows get a trailing '>' so it is obvious they open another list
     * rather than launch an image. */
    if (Entry->Kind == SfbEntrySubmenu) {
      CHAR16  Text[SFB_DESC_CHARS + 4];

      UnicodeSPrint (Text, sizeof (Text), L"%s >", Entry->Desc);
      SfbDrawRow ((BOOLEAN)(Index == Cursor), TRUE, Marker, Text);
    } else {
      SfbDrawRow ((BOOLEAN)(Index == Cursor), TRUE, Marker, Entry->Desc);
    }
  }

  if (Last < Menu->Count) {
    CHAR16  Note[32];

    UnicodeSPrint (Note, sizeof (Note), SfbStr (StrMore),
                   (UINT32)(Menu->Count - Last));
    SfbPanelNote (Note);
  }

  SfbEndScreen (SfbStr (StrKeyNavSelect));
}

/*
 * Run a submenu defined by the ENTRIES file at EntriesPath on Volume. The file
 * is parsed exactly like the root BOOTENTRIES, and may itself contain further
 * '%' submenu rows; Depth bounds the nesting so a chain of files that points at
 * one another cannot recurse without limit. The submenu state is heap-allocated
 * (a single SFB_MENU_STATE is ~17 KB) so deep nesting stays off the call stack.
 *
 * Returns when the user picks the trailing "Back" row, or when the file could
 * not be built at all; the caller then redraws its own menu.
 */
STATIC
VOID
SfbRunSubMenu (IN EFI_HANDLE   Volume,
               IN CONST CHAR16 *EntriesPath,
               IN CONST CHAR16 *Title,
               IN UINTN        Depth)
{
  SFB_MENU_STATE  *Menu = NULL;
  UINTN           Cursor = 0;
  BOOLEAN         Rebuild = TRUE;
  SFB_KEY         Key;
  EFI_STATUS      Status;

  Menu = AllocateZeroPool (sizeof (*Menu));
  if (Menu == NULL) {
    return;
  }
  Menu->DefaultIndex = SFB_NO_INDEX;

  while (TRUE) {
    UINTN  Chosen;

    if (Rebuild) {
      SfbFreeMenu (Menu);
      Status = SfbBuildSubMenu (Menu, Volume, EntriesPath);
      if (EFI_ERROR (Status)) {
        SfbReportStatus (Title, Status);
        break;
      }
      Cursor = 0;
      Rebuild = FALSE;
    }

    SfbDrawMenu (Menu, Cursor, Title);

    /* Same input model as the root menu: volume keys move, power confirms. */
    Key = SfbWaitForKey (0);

    if (Key == SfbKeyUp || Key == SfbKeyDown) {
      SfbMoveCursor (&Cursor, Menu->Count, Key);
      continue;
    }

    if (Menu->Count == 0) {
      continue;
    }

    Chosen = Cursor;
    switch (Menu->Entry[Chosen].Kind) {
    case SfbEntryBack:
      goto done;

    case SfbEntrySubmenu:
      if (Depth >= SFB_MAX_SUBMENU_DEPTH) {
        SfbReportStatus (SfbStr (StrSubmenuTooDeep), EFI_BUFFER_TOO_SMALL);
      } else {
        SfbRunSubMenu (Menu->Entry[Chosen].Volume,
                       Menu->Entry[Chosen].Path,
                       Menu->Entry[Chosen].Desc,
                       Depth + 1);
      }
      /* Media may have changed while the child menu was open. */
      Rebuild = TRUE;
      break;

    case SfbEntryEfiFile:
    default:
      Status = SfbLaunchEntry (&Menu->Entry[Chosen], TRUE, TRUE);//Entries in submenu never defaults
      if (EFI_ERROR (Status)) {
        SfbReportStatus (SfbStr (StrBootFailed), Status);
      }
      Rebuild = TRUE;
      break;
    }
  }

done:
  SfbFreeMenu (Menu);
  FreePool (Menu);
}

BOOLEAN
SfbRunBootMenu (VOID)
{
  SFB_MENU_STATE  Menu;
  UINTN           Cursor = 0;
  BOOLEAN         Rebuild = TRUE;
  SFB_KEY         Key;
  EFI_STATUS      Status;

  ZeroMem (&Menu, sizeof (Menu));
  Menu.DefaultIndex = SFB_NO_INDEX;

  while (TRUE) {
    UINTN  Chosen;

    if (Rebuild) {
      SfbFreeMenu (&Menu);
      SfbBuildMenu (&Menu);
      Cursor = (Menu.DefaultIndex == SFB_NO_INDEX) ? 0 : Menu.DefaultIndex;
      Rebuild = FALSE;
    }

    SfbDrawMenu (&Menu, Cursor, SfbStr (StrBootMenu));

    /* The menu is purely interactive: it waits for a key indefinitely and
     * never launches anything unattended. */
    Key = SfbWaitForKey (0);

    if (Key == SfbKeyUp || Key == SfbKeyDown) {
      SfbMoveCursor (&Cursor, Menu.Count, Key);
      continue;
    }

    Chosen = Cursor;

    if (Menu.Count == 0) {
      continue;
    }

    switch (Menu.Entry[Chosen].Kind) {
    case SfbEntryFastboot:
      SfbFreeMenu (&Menu);
      return TRUE;

    case SfbEntrySelector:
      SfbRunFileBrowser ();
      /* The browser may have added a custom entry. */
      Rebuild = TRUE;
      break;

    case SfbEntrySettings:
      SfbRunSettingsMenu ();
      /* Language may have changed while the settings menu was open. */
      Rebuild = TRUE;
      break;

    case SfbEntrySubmenu:
      SfbRunSubMenu (Menu.Entry[Chosen].Volume,
                     Menu.Entry[Chosen].Path,
                     Menu.Entry[Chosen].Desc,
                     1);
      /* Media may have changed while the submenu was open. */
      Rebuild = TRUE;
      break;

    case SfbEntryBack:
      /* Only submenus carry a Back row; the root menu never adds one. */
      Rebuild = TRUE;
      break;

    case SfbEntryPowerOff:
      SfbShowActionScreen (SfbStr (StrPoweringOff));
      ShutdownDevice ();
      break;

    case SfbEntryRestart:
      SfbShowActionScreen (SfbStr (StrRestarting));
      RebootDevice (NORMAL_MODE);
      break;

    case SfbEntryEfiFile:
    default:
      Status = SfbLaunchEntry (&Menu.Entry[Chosen], FALSE, TRUE);
      if (EFI_ERROR (Status)) {
        SfbReportStatus (SfbStr (StrBootFailed), Status);
      }
      /* Media or variables may have changed while the image ran. */
      Rebuild = TRUE;
      break;
    }
  }
}
