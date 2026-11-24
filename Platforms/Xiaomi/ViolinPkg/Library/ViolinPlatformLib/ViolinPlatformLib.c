/** @file
  ArmPlatformLib implementation for Xiaomi Pad 7S Pro (violin / XRING O1).

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Library/ArmLib.h>
#include <Library/ArmPlatformLib.h>
#include <Library/DebugLib.h>
#include <Library/PcdLib.h>
#include <Ppi/ArmMpCoreInfo.h>

STATIC ARM_CORE_INFO  mViolinCoreInfoTable[] = {
  {
    0x0, // MPIDR Aff0=0 (boot core cpu0)
    0x0  // Mailbox (unused)
  }
};

EFI_BOOT_MODE
ArmPlatformGetBootMode (
  VOID
  )
{
  return BOOT_WITH_FULL_CONFIGURATION;
}

RETURN_STATUS
ArmPlatformInitialize (
  IN UINTN  MpId
  )
{
  return RETURN_SUCCESS;
}

STATIC
EFI_STATUS
PrePeiCoreGetMpCoreInfo (
  OUT UINTN          *CoreCount,
  OUT ARM_CORE_INFO  **ArmCoreTable
  )
{
  *CoreCount    = ARRAY_SIZE (mViolinCoreInfoTable);
  *ArmCoreTable = mViolinCoreInfoTable;
  return EFI_SUCCESS;
}

STATIC ARM_MP_CORE_INFO_PPI  mMpCoreInfoPpi = {
  PrePeiCoreGetMpCoreInfo
};

STATIC EFI_PEI_PPI_DESCRIPTOR  gPlatformPpiTable[] = {
  {
    EFI_PEI_PPI_DESCRIPTOR_PPI,
    &gArmMpCoreInfoPpiGuid,
    &mMpCoreInfoPpi
  }
};

VOID
ArmPlatformGetPlatformPpiList (
  OUT UINTN                   *PpiListSize,
  OUT EFI_PEI_PPI_DESCRIPTOR  **PpiList
  )
{
  *PpiListSize = sizeof (gPlatformPpiTable);
  *PpiList     = gPlatformPpiTable;
}
