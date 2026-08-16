/*
 * Copyright (c) 2009, Google Inc.
 * All rights reserved.
 *
 * Copyright (c) 2009-2021, The Linux Foundation. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or materials provided with the distribution.
 *     * Neither the name of The Linux Foundation nor
 *       the names of its contributors may be used to endorse or promote
 *       products derived from this software without specific prior written
 *       permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NON-INFRINGEMENT ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 * OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
 * ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

/*
 *  Changes from Qualcomm Innovation Center are provided under the following license:
 *
 *  Copyright (c) 2022 - 2025 Qualcomm Innovation Center, Inc. All rights
 *  reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted (subject to the limitations in the
 *  disclaimer below) provided that the following conditions are met:
 *
 *      * Redistributions of source code must retain the above copyright
 *        notice, this list of conditions and the following disclaimer.
 *
 *      * Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials provided
 *        with the distribution.
 *
 *      * Neither the name of Qualcomm Innovation Center, Inc. nor the names of its
 *        contributors may be used to endorse or promote products derived
 *        from this software without specific prior written permission.
 *
 *  NO EXPRESS OR IMPLIED LICENSES TO ANY PARTY'S PATENT RIGHTS ARE
 *  GRANTED BY THIS LICENSE. THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT
 *  HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED
 *  WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 *  MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 *  IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
 *  ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 *  DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE
 *  GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 *  HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 *  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 *  OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH
 *  DAMAGE.
 */

#include "AutoGen.h"
#include "LinuxLoaderLib.h"
#include <FastbootLib/FastbootMain.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PartitionTableUpdate.h>
#include <Library/ShutdownServices.h>
#include <Library/StackCanary.h>
#include "Library/ThreadStack.h"
#include <Protocol/EFICardInfo.h>
#include <Protocol/SimpleTextIn.h>
#include "SuperFbMenu.h"
#include "SuperFbGfx.h"
#include "SuperFbSettings.h"

#define MAX_APP_STR_LEN 64
#define MAX_NUM_FS 10
#define DEFAULT_STACK_CHK_GUARD 0xc0c0c0c0

/**
  Linux Loader Application EntryPoint

  @param[in] ImageHandle    The firmware allocated handle for the EFI image.
  @param[in] SystemTable    A pointer to the EFI System Table.

  @retval EFI_SUCCESS       The entry point is executed successfully.
  @retval other             Some error occurs when executing this entry point.

 **/
/*
 * 开机时扫描音量键（恢复被 Slim 重构移除的 WaitForVolumeDownKey）。
 *
 * 先清空输入缓冲区，再用 WaitForEvent 在超时窗口内等待一次真正的音量键。
 * 音量上键（recovery 槽位）和音量下键（fastboot/BDS 槽位）都会打开 BDS
 * 菜单；非目标按键（尤其是开机时按住、随后松开的电源键）会被跳过并继续
 * 等待，而不是结束扫描——所以电源键既不会被误当成输入，也不会遮挡音量键。
 *
 * @param TimeoutMs   扫描窗口（毫秒）
 * @return 1          检测到音量上键
 * @return 2          检测到音量下键
 * @return 0          超时未检测到
 */
STATIC UINT8
WaitForVolumeKey (IN UINT32 TimeoutMs)
{
  EFI_STATUS    Status;
  EFI_EVENT     TimerEvent;
  EFI_EVENT     WaitList[2];
  UINTN         EventIndex;
  EFI_INPUT_KEY Key;
  UINT8         KeyDetected = 0;

  /* 先清空输入缓冲区 */
  gST->ConIn->Reset (gST->ConIn, FALSE);

  /* 创建定时器事件 */
  Status = gBS->CreateEvent (
                  EVT_TIMER,
                  TPL_CALLBACK,
                  NULL,
                  NULL,
                  &TimerEvent
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "CreateEvent Timer failed: %r\n", Status));
    return FALSE;
  }

  /* 设置定时器：一次性触发，单位为 100ns */
  Status = gBS->SetTimer (
                  TimerEvent,
                  TimerRelative,
                  (UINT64)TimeoutMs * 10000   /* ms -> 100ns */
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "SetTimer failed: %r\n", Status));
    gBS->CloseEvent (TimerEvent);
    return FALSE;
  }

  /* 等待事件列表：按键事件 或 定时器超时 */
  WaitList[0] = gST->ConIn->WaitForKey;
  WaitList[1] = TimerEvent;

  while (TRUE) {
    Status = gBS->WaitForEvent (2, WaitList, &EventIndex);
    if (EFI_ERROR (Status)) {
      DEBUG ((EFI_D_ERROR, "WaitForEvent failed: %r\n", Status));
      break;
    }

    if (EventIndex == 0) {
      /* 按键事件触发 */
      Status = gST->ConIn->ReadKeyStroke (gST->ConIn, &Key);
      if (!EFI_ERROR (Status)) {
        DEBUG ((EFI_D_INFO, "Key detected: ScanCode=0x%x, UnicodeChar=0x%x\n",
                Key.ScanCode, Key.UnicodeChar));

        if (Key.ScanCode == SCAN_UP) { /* recovery / boot menu key */
          /* 检测到音量上键 */
          KeyDetected = 1;
          break;
        }
        if (Key.ScanCode == SCAN_DOWN) { /* fastboot / BDS key */
          /* 检测到音量下键 */
          KeyDetected = 2;
          break;
        }
        /* 不是目标按键（电源键等），忽略并继续等待 */
        DEBUG ((EFI_D_INFO, "Not a volume key, continue waiting...\n"));
      }
    } else {
      /* 定时器超时 */
      DEBUG ((EFI_D_INFO, "Timeout: %d ms expired, no volume key\n",
              TimeoutMs));
      break;
    }
  }

  /* 清理定时器事件 */
  gBS->CloseEvent (TimerEvent);

  return KeyDetected;
}

EFI_STATUS EFIAPI  __attribute__ ( (no_sanitize ("safe-stack")))
LinuxLoaderEntry (IN EFI_HANDLE ImageHandle, IN EFI_SYSTEM_TABLE *SystemTable)
{

  EFI_STATUS Status;

   /* Update stack check guard with random value for better security */
  /* SilentMode Boot */
  /* MultiSlot Boot */
  /* Flashless Boot */
  EFI_MEM_CARDINFO_PROTOCOL *CardInfo = NULL;
  /* set ROT, BootState and VBH only once per boot*/

  /* RED = entry point reached */

  DEBUG ((EFI_D_INFO, "Loader Build Info: %a %a\n", __DATE__, __TIME__));
  DEBUG ((EFI_D_VERBOSE, "LinuxLoader Load Address to debug ABL: 0x%llx\n",
         (UINTN)LinuxLoaderEntry & (~ (0xFFF))));
  DEBUG ((EFI_D_VERBOSE, "LinuxLoaderEntry Address: 0x%llx\n",
         (UINTN)LinuxLoaderEntry));

  Status = InitThreadUnsafeStack ();

  if (Status != EFI_SUCCESS) {
    DEBUG ((EFI_D_ERROR, "Unable to Allocate memory for Unsafe Stack: %r\n",
            Status));
    goto stack_guard_update_default;
  }


  /* Check if memory card is present; goto flashless if not */
  Status = gBS->LocateProtocol (&gEfiMemCardInfoProtocolGuid, NULL,
                                  (VOID **)&CardInfo);

  Status = EnumeratePartitions ();

  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "LinuxLoader: Could not enumerate partitions: %r\n",
            Status));
    /* Leave the partition table alone; it was never populated. */
  } else {
    UpdatePartitionEntries ();
  }

  {
    UINT8         MenuRequested;
    SFB_SETTINGS  Settings;

    /*
     * Scan for the volume keys held at power-on FIRST, before any other init
     * disturbs the console input.  WaitForVolumeKey flushes stale input and
     * then waits for a genuine volume press, skipping every other key (notably
     * the power key used to switch the device on) rather than being fooled by
     * it.  Volume Up (the official recovery key slot) or Volume Down (the
     * documented BDS/fastboot slot) opens the boot menu; no volume press
     * within the window launches the saved default entry.
     */
    MenuRequested = WaitForVolumeKey (3000);
    DEBUG ((EFI_D_INFO, "SFB: power-on volume key=%u\n", MenuRequested));

    /*
     * Now bring up the embedded FAT/USB stack so both the default entry and the
     * menu can see every FAT32 volume, including one on a USB drive.
     */
    Status = SfbStartFatStack ();
    if (EFI_ERROR (Status)) {
      /* Not fatal: the menu still offers fastboot and the program selector. */
      DEBUG ((EFI_D_ERROR, "Unable to start the FAT stack: %r\n", Status));
    }

    /* Load persistent settings from the efisp tail record (best effort). */
    Status = SfbSettingsLoad ();
    if (EFI_ERROR (Status)) {
      DEBUG ((EFI_D_INFO, "SFB: settings unavailable, using defaults: %r\n",
              Status));
    }

    /* Bring up the graphical UI when the platform offers a GOP; the text
     * console UI is used otherwise. */
    SfbGfxInit ();

    SfbSettingsGet (&Settings);
    if (Settings.BootToMenu) {
      MenuRequested = 1;
    }

    if (!MenuRequested) {
      /* No menu key: boot the saved default. This does not return on success;
       * it only comes back if there is no saved default or the launch failed,
       * in which case the menu is shown so the user is never stranded. */
      SfbLaunchDefaultEntry ();
    }

    /*
     * Reached here only when the menu is about to be shown.  Enforce the PIN
     * lock before letting anyone in; the gate loops until it is satisfied (or
     * the PIN is disabled), so it only returns once entry is authorised.
     */
    SfbRunPinGate ();

    /*
     * Reached here because the menu was requested, or there was no default to
     * boot. Announce it and hold briefly so a still-held volume key is released
     * before the menu takes input, then run the menu. It only returns TRUE when
     * the user picked fastboot.
     */
    SfbShowEnteringMenu ();
    if (!SfbRunBootMenu ()) {
      Status = EFI_SUCCESS;
      goto stack_guard_update_default;
    }

    SfbShowFastbootMode ();
    DEBUG ((EFI_D_INFO, "Boot menu requested fastboot\n"));
  }

#ifdef AUTO_VIRT_ABL
  DEBUG ((EFI_D_INFO, "Rebooting the device.\n"));
  RebootDevice (NORMAL_MODE);
#endif
  DEBUG ((EFI_D_INFO, "Launching fastboot\n"));
  Status = FastbootInitialize ();
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "Failed to Launch Fastboot App: %d\n", Status));
    goto stack_guard_update_default;
  }

stack_guard_update_default:
  /*Update stack check guard with defualt value then return*/
  __stack_chk_guard = DEFAULT_STACK_CHK_GUARD;

  DeInitThreadUnsafeStack ();

  return Status;
}
