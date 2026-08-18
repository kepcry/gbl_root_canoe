/*
 * Persistent settings for the BDS boot menu.
 *
 * The settings live in slot SFB_STORE_SETTINGS of the raw record store at the
 * end of the efisp (EFI System) partition, next to the default/custom boot
 * entry records.  The record is a single printable-ASCII line of key=value
 * pairs separated by ';', so it stays easy to read and edit on the device:
 *
 *   SFBCFG1;lang=zh;pin_enable=0;pin=0000;show_booting=1;boot_to_menu=1;pretentious=0;pretentious_mode=0;pretentious_art=0
 *
 * Copyright (c) 2026, contributors to the canoe ABL tree.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef __SUPER_FB_SETTINGS_H__
#define __SUPER_FB_SETTINGS_H__

#include <Uefi.h>
#include "SuperFbLang.h"

#define SFB_PIN_DIGITS  4

/* Pretentious Mode display styles. */
#define SFB_PRETENTIOUS_LOG_OPTIMIZED  0   /* transparent frame-buffer logs */
#define SFB_PRETENTIOUS_LOG_CLASSIC    1   /* SimpleFont console logs       */
#define SFB_PRETENTIOUS_ART            2   /* centered character art, no log */

typedef struct {
  SFB_LANG  Lang;
  BOOLEAN   PinEnabled;
  /* 4 digits plus terminator, ASCII '0'-'9'. */
  CHAR16    Pin[SFB_PIN_DIGITS + 1];
  BOOLEAN   ShowBooting;
  BOOLEAN   BootToMenu;
  /* "Pretentious Mode": loading screens dump hundreds of meaningless logs. */
  BOOLEAN   Pretentious;
  /* Character-art text shown on the booting prompt: 0=豪情在天, 1=嘉豪,
   * 2=嘉欣, 3=豪. */
  UINTN     PretentiousArt;
  /* Pretentious Mode style: SFB_PRETENTIOUS_LOG_OPTIMIZED / _LOG_CLASSIC /
   * _ART. */
  UINTN     PretentiousMode;
} SFB_SETTINGS;

/*
 * Load settings from the efisp partition tail record.  A missing or invalid
 * record falls back to defaults (Pin off, BootToMenu on).
 */
EFI_STATUS
SfbSettingsLoad (VOID);

/* Snapshot of the currently loaded settings. */
VOID
SfbSettingsGet (OUT SFB_SETTINGS *Out);

/* Persist Settings to the efisp partition tail record. */
EFI_STATUS
SfbSettingsSave (IN CONST SFB_SETTINGS *Settings);

/* TRUE when Pin matches the stored PIN (or when the PIN is disabled). */
BOOLEAN
SfbSettingsCheckPin (IN CONST CHAR16 *Pin);

#endif /* __SUPER_FB_SETTINGS_H__ */
