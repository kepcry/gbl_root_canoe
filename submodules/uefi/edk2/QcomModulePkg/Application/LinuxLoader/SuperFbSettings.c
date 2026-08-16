/*
 * Implementation of the efisp tail settings record.
 *
 * Copyright (c) 2026, contributors to the canoe ABL tree.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "SuperFbSettings.h"
#include "SuperFbMenu.h"

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/PrintLib.h>

#define SFB_CFG_TAG  "SFBCFG1;"

STATIC SFB_SETTINGS  mSfbSettings = {
  SfbLangZh,      /* Lang */
  FALSE,          /* PinEnabled */
  L"0000",        /* Pin */
  TRUE,           /* ShowBooting */
  FALSE           /* BootToMenu: volume key opens the menu, no key boots */
};

/*
 * Find "key=" inside a ';' separated record and copy the value into Value
 * (ValueChars includes the NUL).  Returns the value length, 0 when absent.
 */
STATIC
UINTN
SfbSettingsFindKey (IN CONST CHAR8 *Record, IN CONST CHAR8 *Key,
                    OUT CHAR8 *Value, IN UINTN ValueChars)
{
  UINTN   KeyLen = AsciiStrLen (Key);
  CONST CHAR8  *Cursor = Record;

  while (Cursor != NULL && *Cursor != '\0') {
    CONST CHAR8  *Next;
    UINTN        TokenLen;

    Next = AsciiStrStr (Cursor, ";");
    TokenLen = (Next != NULL) ? (UINTN)(Next - Cursor) : AsciiStrLen (Cursor);

    if (TokenLen > KeyLen &&
        AsciiStrnCmp (Cursor, Key, KeyLen) == 0 &&
        Cursor[KeyLen] == '=') {
      UINTN  VLen = TokenLen - KeyLen - 1;

      if (VLen >= ValueChars) {
        VLen = ValueChars - 1;
      }
      CopyMem (Value, Cursor + KeyLen + 1, VLen);
      Value[VLen] = '\0';
      return VLen;
    }

    Cursor = (Next != NULL) ? Next + 1 : NULL;
  }

  Value[0] = '\0';
  return 0;
}

STATIC
UINT32
SfbSettingsParseU32 (IN CONST CHAR8 *Text)
{
  UINT32  Result = 0;

  while (*Text >= '0' && *Text <= '9') {
    Result = Result * 10 + (UINT32)(*Text - '0');
    Text++;
  }
  return Result;
}

EFI_STATUS
SfbSettingsLoad (VOID)
{
  EFI_STATUS  Status;
  CHAR8       Record[SFB_STORE_SLOT_BYTES];
  CHAR8       Value[16];

  Status = SfbStoreRead (SFB_STORE_SETTINGS, Record, sizeof (Record));
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_WARN, "SFB: settings store unavailable: %r\n", Status));
    return Status;
  }

  if (AsciiStrnCmp (Record, SFB_CFG_TAG, sizeof (SFB_CFG_TAG) - 1) != 0) {
    DEBUG ((EFI_D_WARN, "SFB: settings record missing, using defaults\n"));
    return EFI_NOT_FOUND;
  }

  if (SfbSettingsFindKey (Record + sizeof (SFB_CFG_TAG) - 1, "lang",
                          Value, sizeof (Value)) != 0) {
    if (AsciiStriCmp (Value, "en") == 0) {
      mSfbSettings.Lang = SfbLangEn;
    } else if (AsciiStriCmp (Value, "zh") == 0) {
      mSfbSettings.Lang = SfbLangZh;
    }
  }

  if (SfbSettingsFindKey (Record + sizeof (SFB_CFG_TAG) - 1, "pin_enable",
                          Value, sizeof (Value)) != 0) {
    mSfbSettings.PinEnabled = (BOOLEAN)(SfbSettingsParseU32 (Value) != 0);
  }

  if (SfbSettingsFindKey (Record + sizeof (SFB_CFG_TAG) - 1, "pin",
                          Value, sizeof (Value)) != 0) {
    UINTN  Index;

    for (Index = 0; Index < SFB_PIN_DIGITS; Index++) {
      mSfbSettings.Pin[Index] =
        (Value[Index] >= '0' && Value[Index] <= '9') ? Value[Index] : L'0';
    }
    mSfbSettings.Pin[SFB_PIN_DIGITS] = L'\0';
  }

  if (SfbSettingsFindKey (Record + sizeof (SFB_CFG_TAG) - 1, "show_booting",
                          Value, sizeof (Value)) != 0) {
    mSfbSettings.ShowBooting = (BOOLEAN)(SfbSettingsParseU32 (Value) != 0);
  }

  if (SfbSettingsFindKey (Record + sizeof (SFB_CFG_TAG) - 1, "boot_to_menu",
                          Value, sizeof (Value)) != 0) {
    mSfbSettings.BootToMenu = (BOOLEAN)(SfbSettingsParseU32 (Value) != 0);
  }

  SfbLangSet (mSfbSettings.Lang);
  return EFI_SUCCESS;
}

VOID
SfbSettingsGet (OUT SFB_SETTINGS *Out)
{
  if (Out != NULL) {
    CopyMem (Out, &mSfbSettings, sizeof (*Out));
  }
}

EFI_STATUS
SfbSettingsSave (IN CONST SFB_SETTINGS *Settings)
{
  CHAR8   Record[SFB_STORE_SLOT_BYTES];
  CHAR8   PinA[SFB_PIN_DIGITS + 1];
  UINTN   Index;

  if (Settings != NULL) {
    mSfbSettings = *Settings;
    SfbLangSet (mSfbSettings.Lang);
  }

  for (Index = 0; Index < SFB_PIN_DIGITS; Index++) {
    PinA[Index] = (CHAR8)mSfbSettings.Pin[Index];
  }
  PinA[SFB_PIN_DIGITS] = '\0';

  AsciiSPrint (
    Record, sizeof (Record),
    SFB_CFG_TAG
    "lang=%a;pin_enable=%d;pin=%a;show_booting=%d;boot_to_menu=%d",
    (mSfbSettings.Lang == SfbLangEn) ? "en" : "zh",
    mSfbSettings.PinEnabled ? 1 : 0,
    PinA,
    mSfbSettings.ShowBooting ? 1 : 0,
    mSfbSettings.BootToMenu ? 1 : 0);

  return SfbStoreWrite (SFB_STORE_SETTINGS, Record);
}

BOOLEAN
SfbSettingsCheckPin (IN CONST CHAR16 *Pin)
{
  if (!mSfbSettings.PinEnabled) {
    return TRUE;
  }
  if (Pin == NULL) {
    return FALSE;
  }
  return (BOOLEAN)(StrnCmp (Pin, mSfbSettings.Pin, SFB_PIN_DIGITS) == 0);
}
