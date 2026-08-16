/*
 * String table backing SuperFbLang.h.  Keep the array order in sync with
 * SFB_STR_ID.  Every L"..." literal in this file is also the source set for
 * the embedded bitmap font (tools/gen_sfb_font/gen_sfb_font.ps1).
 *
 * Copyright (c) 2026, contributors to the canoe ABL tree.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "SuperFbLang.h"

typedef struct {
  CONST CHAR16  *En;
  CONST CHAR16  *Zh;
} SFB_LANG_STRING;

STATIC CONST SFB_LANG_STRING  mSfbStrings[StrCount] = {
  { L"Boot Menu",                       L"启动菜单" },
  { L"Entering Boot Menu",              L"正在进入启动菜单" },
  { L"No boot entries found.",          L"未找到启动项。" },
  { L"... %u more",                     L"... 还有 %u 项" },
  { L"Vol Up/Down: move   Power: select", L"音量上/下：移动  电源键：选择" },
  { L"Vol Up/Down: move   Power: open", L"音量上/下：移动  电源键：打开" },
  { L"Vol +/-: digit   Power: OK",      L"音量 +/-：数字  电源键：确定" },
  { L"Enter Fastboot",                  L"进入 Fastboot" },
  { L"FASTBOOT MODE",                   L"FASTBOOT 模式" },
  { L"Connect a host and run fastboot commands.", L"连接电脑并执行 fastboot 命令。" },
  { L"Booting %s",                      L"正在启动 %s" },
  { L"Powering off...",                 L"正在关机..." },
  { L"Restarting...",                   L"正在重启..." },
  { L"Power Off",                       L"关机" },
  { L"Restart",                         L"重启" },
  { L"Settings",                        L"设置" },
  { L"Back",                            L"返回" },
  { L"Load",                            L"加载" },
  { L"Boot (temporary)",                L"启动（临时）" },
  { L"Add to BootMenu",                 L"添加到启动菜单" },
  { L"EFI Program Selector",            L"EFI 程序选择器" },
  { L"EFI Driver",                      L"EFI 驱动" },
  { L"EFI Application",                 L"EFI 应用程序" },
  { L"Press power to continue.",        L"按电源键继续。" },
  { L"Boot failed",                     L"启动失败" },
  { L"Driver load failed",              L"驱动加载失败" },
  { L"Driver loaded",                   L"驱动已加载" },
  { L"Could not save entry",            L"无法保存启动项" },
  { L"Added to boot menu",              L"已添加到启动菜单" },
  { L"Out of memory",                   L"内存不足" },
  { L"Cannot read directory",           L"无法读取目录" },
  { L"Cannot address that file",        L"无法定位该文件" },
  { L"Submenu too deep",                L"子菜单层级过深" },
  { L"Cannot load driver",              L"无法加载驱动" },
  { L"Language",                        L"语言" },
  { L"English",                         L"English" },
  { L"中文",                            L"中文" },
  { L"PIN Lock",                        L"PIN 锁" },
  { L"Change PIN",                      L"修改 PIN" },
  { L"Enable PIN",                      L"启用 PIN" },
  { L"Disable PIN",                     L"禁用 PIN" },
  { L"Show Booting screen",             L"显示启动提示" },
  { L"Boot to Menu on power-on",        L"开机进入启动菜单" },
  { L"On",                              L"开" },
  { L"Off",                             L"关" },
  { L"Enabled",                         L"已启用" },
  { L"Disabled",                        L"已禁用" },
  { L"Enter new PIN (4 digits)",        L"输入新 PIN（4 位）" },
  { L"Confirm PIN",                     L"确认 PIN" },
  { L"Enter PIN",                       L"输入 PIN" },
  { L"PIN mismatch",                    L"PIN 不一致" },
  { L"Wrong PIN",                       L"PIN 错误" },
  { L"PIN set",                         L"PIN 已设置" },
  { L"PIN removed",                     L"PIN 已移除" },
  { L"Settings saved",                  L"设置已保存" },
  { L"Could not save settings",         L"无法保存设置" },
  { L"Could not read settings, using defaults", L"无法读取设置，使用默认值" },
  { L"Enter PIN to unlock",             L"输入 PIN 解锁" },
  { L"PIN required",                    L"需要 PIN" },
  { L"Not an EFI application",          L"不是 EFI 应用程序" },
  { L"No FAT32 volumes found",          L"未找到 FAT32 卷" },
  { L"Volume %u",                       L"卷 %u" },
  { L"(directory has more than %u entries; rest not shown)",
    L"（目录超过 %u 项，其余未显示）" },
};

STATIC SFB_LANG  mSfbLang = SfbLangZh;

VOID
SfbLangSet (IN SFB_LANG Lang)
{
  if (Lang == SfbLangEn || Lang == SfbLangZh) {
    mSfbLang = Lang;
  }
}

SFB_LANG
SfbLangGet (VOID)
{
  return mSfbLang;
}

CONST CHAR16 *
SfbStr (IN UINTN Id)
{
  if (Id >= StrCount) {
    return L"?";
  }

  return (mSfbLang == SfbLangEn) ? mSfbStrings[Id].En : mSfbStrings[Id].Zh;
}
