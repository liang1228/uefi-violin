/** @file
  PSCI HwResetSystemLib / ResetSystemLib for Xiaomi Pad 7S Pro (violin) & QEMU.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Base.h>
#include <Library/ArmMonitorLib.h>
#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include <Library/ResetSystemLib.h>

#define PSCI_SYSTEM_OFF    0x84000008U
#define PSCI_SYSTEM_RESET  0x84000009U

STATIC
VOID
PsciInvoke (
  IN UINT32  FunctionId
  )
{
  ARM_MONITOR_ARGS  Args;

  Args.Arg0 = FunctionId;
  Args.Arg1 = 0;
  Args.Arg2 = 0;
  Args.Arg3 = 0;
  ArmMonitorCall (&Args);
  CpuDeadLoop ();
}

VOID
EFIAPI
ResetCold (
  VOID
  )
{
  PsciInvoke (PSCI_SYSTEM_RESET);
}

VOID
EFIAPI
ResetWarm (
  VOID
  )
{
  PsciInvoke (PSCI_SYSTEM_RESET);
}

VOID
EFIAPI
ResetShutdown (
  VOID
  )
{
  PsciInvoke (PSCI_SYSTEM_OFF);
}

VOID
EFIAPI
ResetPlatformSpecific (
  IN UINTN  DataSize,
  IN VOID   *ResetData
  )
{
  PsciInvoke (PSCI_SYSTEM_RESET);
}
