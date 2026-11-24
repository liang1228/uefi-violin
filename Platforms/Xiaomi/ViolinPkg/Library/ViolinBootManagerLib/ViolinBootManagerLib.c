/** @file
  Minimal PlatformBootManagerLib for violin (XRING O1): connects consoles and
  firmware drivers so UniXNUDxe's registered Boot0000 (UniXNU.efi) boots directly.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/DebugLib.h>
#include <Library/DevicePathLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PlatformBootManagerLib.h>
#include <Library/UefiBootManagerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Protocol/GraphicsOutput.h>
#include <Protocol/SerialIo.h>
#include <Protocol/SimpleTextIn.h>

STATIC
VOID
AddConsoleVariables (
  VOID
  )
{
  EFI_STATUS                Status;
  EFI_HANDLE                *Handles;
  UINTN                     Count;
  UINTN                     Idx;
  EFI_DEVICE_PATH_PROTOCOL  *Dp;

  // Register GOP handle as ConOut so GraphicsConsoleDxe / ConSplitter attach cleanly
  Status = gBS->LocateHandleBuffer (ByProtocol, &gEfiGraphicsOutputProtocolGuid, NULL, &Count, &Handles);
  if (!EFI_ERROR (Status)) {
    for (Idx = 0; Idx < Count; Idx++) {
      Dp = DevicePathFromHandle (Handles[Idx]);
      if (Dp != NULL) {
        EfiBootManagerUpdateConsoleVariable (ConOut, Dp, NULL);
        EfiBootManagerUpdateConsoleVariable (ErrOut, Dp, NULL);
      }
    }
    FreePool (Handles);
  }

  // Register physical SimpleTextIn handles (those carrying a DevicePath) as ConIn
  // so ConSplitterDxe binds ViolinPmicKeyDxe into gST->ConIn and UniXNU sees input=yes.
  Status = gBS->LocateHandleBuffer (ByProtocol, &gEfiSimpleTextInProtocolGuid, NULL, &Count, &Handles);
  if (!EFI_ERROR (Status)) {
    for (Idx = 0; Idx < Count; Idx++) {
      Dp = DevicePathFromHandle (Handles[Idx]);
      if (Dp != NULL) {
        EfiBootManagerUpdateConsoleVariable (ConIn, Dp, NULL);
      }
    }
    FreePool (Handles);
  }
}

VOID
EFIAPI
PlatformBootManagerBeforeConsole (
  VOID
  )
{
  DEBUG ((DEBUG_INFO, "[ViolinBds] BeforeConsole: registering GOP console variables\n"));
  AddConsoleVariables ();
}

VOID
EFIAPI
PlatformBootManagerAfterConsole (
  VOID
  )
{
  DEBUG ((DEBUG_INFO, "[ViolinBds] AfterConsole: connecting consoles and controllers\n"));
  EfiBootManagerConnectAllDefaultConsoles ();
  EfiBootManagerConnectAll ();
  EfiBootManagerRefreshAllBootOption ();
}

VOID
EFIAPI
PlatformBootManagerWaitCallback (
  UINT16  TimeoutRemain
  )
{
  // No-op during countdown
}

VOID
EFIAPI
PlatformBootManagerUnableToBoot (
  VOID
  )
{
  DEBUG ((DEBUG_INFO, "[ViolinBds] UnableToBoot / all boot options returned -> EfiResetCold\n"));
  gRT->ResetSystem (EfiResetCold, EFI_SUCCESS, 0, NULL);
}

VOID
EFIAPI
PlatformBootManagerBdsEntry (
  VOID
  )
{
  DEBUG ((DEBUG_INFO, "[ViolinBds] BdsEntry\n"));
}

VOID
EFIAPI
PlatformBootManagerProcessBootCompletion (
  IN EFI_BOOT_MANAGER_LOAD_OPTION  *BootOption
  )
{
  if (BootOption != NULL) {
    DEBUG ((DEBUG_INFO, "[ViolinBds] Boot option returned: %s (%r)\n", BootOption->Description, BootOption->Status));
  }
}

VOID
EFIAPI
PlatformBootManagerPriorityBoot (
  UINT16  **BootNext
  )
{
}

VOID
EFIAPI
PlatformBootManagerDeadloop (
  VOID
  )
{
  DEBUG ((DEBUG_INFO, "[ViolinBds] Deadloop -> EfiResetCold\n"));
  gRT->ResetSystem (EfiResetCold, EFI_SUCCESS, 0, NULL);
}

VOID
EFIAPI
PlatformBootManagerOnDemandConInConnect (
  VOID
  )
{
}
