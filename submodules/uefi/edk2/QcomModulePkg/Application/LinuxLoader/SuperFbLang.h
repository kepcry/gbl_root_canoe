/*
 * Bilingual (English / Simplified Chinese) strings for the super-fastboot
 * boot menu.
 *
 * Only static UI chrome is translated.  Dynamic data - boot entry names,
 * file names, volume labels - is never translated, it is displayed verbatim
 * from the media.
 *
 * Copyright (c) 2026, contributors to the canoe ABL tree.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef __SUPER_FB_LANG_H__
#define __SUPER_FB_LANG_H__

#include <Uefi.h>

typedef enum {
  SfbLangEn = 0,
  SfbLangZh = 1
} SFB_LANG;

typedef enum {
  StrBootMenu = 0,
  StrEnteringBootMenu,
  StrNoEntries,
  StrMore,
  StrKeyNavSelect,
  StrKeyNavOpen,
  StrKeyNavPin,
  StrEnterFastboot,
  StrFastbootMode,
  StrFastbootHint,
  StrBooting,
  StrPoweringOff,
  StrRestarting,
  StrPowerOff,
  StrRestart,
  StrSettings,
  StrBack,
  StrLoad,
  StrBootTemporary,
  StrAddToBootMenu,
  StrEfiProgramSelector,
  StrEfiDriver,
  StrEfiApplication,
  StrPressPower,
  StrBootFailed,
  StrDriverLoadFailed,
  StrDriverLoaded,
  StrCouldNotSaveEntry,
  StrAddedToBootMenu,
  StrOutOfMemory,
  StrCannotReadDir,
  StrCannotAddressFile,
  StrSubmenuTooDeep,
  StrCannotLoadDriver,
  StrLanguage,
  StrLangEnglish,
  StrLangChinese,
  StrPinLock,
  StrChangePin,
  StrEnablePin,
  StrDisablePin,
  StrShowBooting,
  StrBootToMenu,
  StrOn,
  StrOff,
  StrEnabled,
  StrDisabled,
  StrEnterNewPin,
  StrConfirmPin,
  StrEnterPin,
  StrPinMismatch,
  StrWrongPin,
  StrPinSet,
  StrPinRemoved,
  StrConfigSaved,
  StrConfigFailed,
  StrConfigReadFailed,
  StrPinLocked,
  StrPinRequired,
  StrNotEfiApp,
  StrNoFatVolumes,
  StrVolumeFmt,
  StrDirTruncated,
  StrPretentious,
  StrPretentiousArt,
  StrPretentiousMode,
  StrLogOptimized,
  StrLogClassic,
  StrArtMode,
  StrArtHao,
  StrArtTian,
  StrArtNiu,
  StrCount
} SFB_STR_ID;

/* Select the active UI language. */
VOID
SfbLangSet (IN SFB_LANG Lang);

SFB_LANG
SfbLangGet (VOID);

/* Static UI string for Id in the active language. Never NULL. */
CONST CHAR16 *
SfbStr (IN UINTN Id);

#endif /* __SUPER_FB_LANG_H__ */
