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
#include <Protocol/SimpleTextIn.h>

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

STATIC
VOID
SfbScreenSize (OUT UINTN *Columns, OUT UINTN *Rows)
{
  UINTN  Cols = 0;
  UINTN  Rs   = 0;

  if (gST->ConOut->Mode != NULL) {
    Cols = gST->ConOut->Mode->Columns;
    Rs   = gST->ConOut->Mode->Rows;
    if ((Cols == 0 || Rs == 0) && gST->ConOut->QueryMode != NULL) {
      gST->ConOut->QueryMode (gST->ConOut, gST->ConOut->Mode->Mode,
                              &Cols, &Rs);
    }
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
VOID
SfbPanelNote (IN CONST CHAR16 *Text)
{
  SfbPanelText (Text, SFB_ATTR_FOOTER);
}

VOID
SfbBeginScreen (IN CONST CHAR16 *Title, IN CONST CHAR16 *Subtitle)
{
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

  /*
   * Fixed chrome: top border, blank, title, blank, [subtitle, blank], the
   * visible list rows, blank, footer and bottom border.  The panel is then
   * grown to at least a third of the screen height so it reads as a centred
   * window no matter how short the list is.
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
  if (Subtitle != NULL) {
    SfbPanelCentered (Subtitle, SFB_ATTR_FOOTER);
    SfbPanelBlank ();
  }
}

VOID
SfbEndScreen (IN CONST CHAR16 *Footer)
{
  UINTN  Index;

  SfbPanelBlank ();
  if (Footer != NULL) {
    SfbPanelCentered (Footer, SFB_ATTR_FOOTER);
  }
  for (Index = 0; Index < gSfbPanelExtra; Index++) {
    SfbPanelBlank ();
  }
  SfbPanelBorder ();
}

VOID
SfbDrawRow (IN BOOLEAN Selected, IN CONST CHAR16 *Marker, IN CONST CHAR16 *Text)
{
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
  gST->ConOut->SetAttribute (gST->ConOut,
                             Selected ? SFB_ATTR_SELECTED : SFB_ATTR_NORMAL);
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
  SfbShowBanner (Text, L"Press power to continue.");
  SfbWaitForKey (0);
}

/*
 * Hand the screen over to fastboot. The menu is the last thing that draws
 * before control leaves for the fastboot loop, which prints nothing of its own
 * until a host connects, so without this the user would be staring at a boot
 * menu that no longer responds to anything.
 */
VOID
SfbShowFastbootMode (VOID)
{
  SfbShowBanner (L"FASTBOOT MODE", L"Connect a host and run fastboot commands.");
}

/*
 * Clear the menu away and announce the launch. The loaded image prints nothing
 * of its own until it takes over, so without this the boot menu would linger on
 * screen through the load.
 */
VOID
SfbShowBootingScreen (IN CONST CHAR16 *Name, IN BOOLEAN ClearScreen)
{
  CHAR16  Text[SFB_DESC_CHARS + 16];

  UnicodeSPrint (Text, sizeof (Text), L"Booting %s",
                 (Name != NULL && Name[0] != L'\0') ? Name : L"...");

  /*
   * An unattended default boot must not blank whatever is already on screen
   * (typically the boot splash): only clear when the launch came from the menu,
   * where the menu itself is what needs clearing away.  When the screen stays,
   * the banner is centred in place instead of being wrapped in a panel.
   */
  if (ClearScreen) {
    SfbShowBanner (Text, NULL);
  } else {
    UINTN  Cols;
    UINTN  Rows;
    UINTN  Col;
    UINTN  Row;

    gST->ConOut->SetAttribute (gST->ConOut, SFB_ATTR_TITLE);
    gST->ConOut->EnableCursor (gST->ConOut, FALSE);
    SfbScreenSize (&Cols, &Rows);
    Col = (StrLen (Text) >= Cols) ? 0 : (Cols - StrLen (Text)) / 2;
    Row = Rows / 2;
    gST->ConOut->SetCursorPosition (gST->ConOut, Col, Row);
    Print (L"%s", Text);
    gST->ConOut->SetAttribute (gST->ConOut, SFB_ATTR_NORMAL);
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
  SfbShowBanner (L"Entering Boot Menu", L"Volume Up/Down: move   Power: select");

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

  SfbBeginScreen (Title, NULL);

  if (Menu->Count == 0) {
    SfbPanelNote (L"No boot entries found.");
  }

  Start = SfbWindowStart (Cursor, Menu->Count, SFB_VISIBLE_ROWS);
  Last = Start + SFB_VISIBLE_ROWS;
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
      SfbDrawRow ((BOOLEAN)(Index == Cursor), Marker, Text);
    } else {
      SfbDrawRow ((BOOLEAN)(Index == Cursor), Marker, Entry->Desc);
    }
  }

  if (Last < Menu->Count) {
    CHAR16  Note[32];

    UnicodeSPrint (Note, sizeof (Note), L"... %u more",
                   (UINT32)(Menu->Count - Last));
    SfbPanelNote (Note);
  }

  SfbEndScreen (L"Vol Up/Down: move   Power: select");
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
        SfbReportStatus (L"Submenu too deep", EFI_BUFFER_TOO_SMALL);
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
        SfbReportStatus (L"Boot failed", Status);
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

    SfbDrawMenu (&Menu, Cursor, L"Boot Menu");

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
      SfbShowActionScreen (L"Powering off...");
      ShutdownDevice ();
      break;

    case SfbEntryRestart:
      SfbShowActionScreen (L"Restarting...");
      RebootDevice (NORMAL_MODE);
      break;

    case SfbEntryEfiFile:
    default:
      Status = SfbLaunchEntry (&Menu.Entry[Chosen], FALSE, TRUE);
      if (EFI_ERROR (Status)) {
        SfbReportStatus (L"Boot failed", Status);
      }
      /* Media or variables may have changed while the image ran. */
      Rebuild = TRUE;
      break;
    }
  }
}
