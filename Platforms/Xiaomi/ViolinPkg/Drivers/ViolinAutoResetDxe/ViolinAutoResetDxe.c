/** @file
  Bounded auto-reset safety driver for violin Stage-1 bring-up.

  Arms a one-shot UEFI timer on ReadyToBoot (default 5s on violin, 1s on QEMU)
  plus a hard fallback timer (18s / 6s) so Stage-1 UEFI/UniXNU bring-up runs on
  inactive boot_b always trigger PSCI SYSTEM_RESET and auto-return to untouched
  slot a Android without requiring manual button holds.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Guid/EventGroup.h>
#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include <Library/PcdLib.h>
#include <Library/SerialPortLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>

STATIC EFI_EVENT  mReadyToBootEvent;
STATIC EFI_EVENT  mPostBootTimerEvent;
STATIC EFI_EVENT  mFallbackTimerEvent;
STATIC BOOLEAN    mPostBootArmed;

STATIC
VOID
EFIAPI
OnAutoResetTimer (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  CONST CHAR8  *Reason = (CONST CHAR8 *)Context;
  CHAR8        Msg[]   = "\r\n[ViolinAutoReset] Bounded timer expired -> EfiResetCold (PSCI SYSTEM_RESET)\r\n";

  DEBUG ((DEBUG_INFO, "[ViolinAutoReset] Firing EfiResetCold (%a)\n", Reason != NULL ? Reason : "timer"));
  SerialPortWrite ((UINT8 *)Msg, sizeof (Msg) - 1);
  gRT->ResetSystem (EfiResetCold, EFI_SUCCESS, 0, NULL);
}

STATIC
VOID
EFIAPI
OnReadyToBoot (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  UINT32      Seconds;
  EFI_STATUS  Status;

  if (mPostBootArmed) {
    return;
  }

  mPostBootArmed = TRUE;
  Seconds        = FixedPcdGet32 (PcdViolinAutoResetSeconds);
  if (Seconds == 0) {
    return;
  }

  Status = gBS->SetTimer (mPostBootTimerEvent, TimerRelative, MultU64x32 (10000000ULL, Seconds));
  DEBUG ((DEBUG_INFO, "[ViolinAutoReset] ReadyToBoot: armed %u s reset timer (%r)\n", Seconds, Status));
}

EFI_STATUS
EFIAPI
ViolinAutoResetDxeInitialize (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;
  UINT32      FallbackSec;

  Status = gBS->CreateEvent (
                  EVT_TIMER | EVT_NOTIFY_SIGNAL,
                  TPL_CALLBACK,
                  OnAutoResetTimer,
                  (VOID *)"ready-to-boot",
                  &mPostBootTimerEvent
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = gBS->CreateEvent (
                  EVT_TIMER | EVT_NOTIFY_SIGNAL,
                  TPL_CALLBACK,
                  OnAutoResetTimer,
                  (VOID *)"hard-fallback",
                  &mFallbackTimerEvent
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  FallbackSec = FixedPcdGet32 (PcdViolinFallbackResetSeconds);
  if (FallbackSec > 0) {
    gBS->SetTimer (mFallbackTimerEvent, TimerRelative, MultU64x32 (10000000ULL, FallbackSec));
  }

  Status = gBS->CreateEventEx (
                  EVT_NOTIFY_SIGNAL,
                  TPL_CALLBACK,
                  OnReadyToBoot,
                  NULL,
                  &gEfiEventReadyToBootGuid,
                  &mReadyToBootEvent
                  );
  DEBUG ((
    DEBUG_INFO,
    "[ViolinAutoReset] Armed (ReadyToBoot=%u s, Fallback=%u s, Status=%r)\n",
    FixedPcdGet32 (PcdViolinAutoResetSeconds),
    FallbackSec,
    Status
    ));

  return EFI_SUCCESS;
}
