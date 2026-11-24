/** @file
  PlatformPeiLib for violin: builds FvHob, reserves BootShim/O1 log memory,
  and publishes UniXNU EntryArg HOB (FDT physical address from BootShim).

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <PiPei.h>
#include <Library/DebugLib.h>
#include <Library/HobLib.h>
#include <Library/PcdLib.h>
#include <ViolinHandoff.h>

// Matches UniXNURamDiskDxe.c mEntryArgHobGuid
STATIC CONST EFI_GUID  mUniXnuEntryArgHobGuid = {
  0xDAEE82E6, 0x8090, 0x4E72, { 0xBF, 0xB9, 0x78, 0xC1, 0x3C, 0xD4, 0xE3, 0x59 }
};

EFI_STATUS
EFIAPI
PlatformPeim (
  VOID
  )
{
  CONST VIOLIN_BOOTSHIM_HANDOFF  *Handoff;
  UINT64                         *EntryArgHob;
  VIOLIN_PAYLOAD_HOB             *PayloadHob;

  BuildFvHob (PcdGet64 (PcdFvBaseAddress), PcdGet32 (PcdFvSize));

  // Reserve O1 primary RAM log (0x100400000, 512 KiB) + BootShim handoff page (0x100480000, 512 KiB)
  BuildMemoryAllocationHob (
    FixedPcdGet64 (PcdLogBase),
    0x00100000ULL,
    EfiReservedMemoryType
    );

  // Reserve pstore/ramoops region so UEFI never treats it as allocatable RAM
  BuildResourceDescriptorHob (
    EFI_RESOURCE_MEMORY_RESERVED,
    EFI_RESOURCE_ATTRIBUTE_PRESENT | EFI_RESOURCE_ATTRIBUTE_INITIALIZED | EFI_RESOURCE_ATTRIBUTE_UNCACHEABLE,
    0xA0000000ULL,
    0x00100000ULL
    );

  Handoff = (CONST VIOLIN_BOOTSHIM_HANDOFF *)(UINTN)FixedPcdGet64 (PcdViolinHandoffBase);
  if ((Handoff != NULL) && (Handoff->Magic == VIOLIN_HANDOFF_MAGIC)) {
    DEBUG ((
      DEBUG_INFO,
      "[ViolinPei] BootShim handoff: FdtBase=0x%lx LoadPc=0x%lx CurrentEL=0x%lx Payload=0x%lx+0x%lx\n",
      Handoff->FdtBase,
      Handoff->LoadPc,
      Handoff->CurrentEl,
      Handoff->PayloadBase,
      Handoff->PayloadSize
      ));
    if (Handoff->FdtBase != 0) {
      EntryArgHob = BuildGuidHob (&mUniXnuEntryArgHobGuid, sizeof (*EntryArgHob));
      if (EntryArgHob != NULL) {
        *EntryArgHob = Handoff->FdtBase;
      }
    }

    if ((Handoff->PayloadBase != 0) && (Handoff->PayloadSize != 0)) {
      UINT64  AlignedSize = ALIGN_VALUE (Handoff->PayloadSize, EFI_PAGE_SIZE);
      BuildMemoryAllocationHob (
        Handoff->PayloadBase,
        AlignedSize,
        EfiBootServicesData
        );
      PayloadHob = BuildGuidHob (&gViolinPayloadHobGuid, sizeof (*PayloadHob));
      if (PayloadHob != NULL) {
        PayloadHob->Base             = Handoff->PayloadBase;
        PayloadHob->Size             = Handoff->PayloadSize;
        PayloadHob->Magic            = Handoff->PayloadMagic;
        PayloadHob->UncompressedSize = Handoff->PayloadUncompressedSize;
      }
    }
  } else {
    DEBUG ((DEBUG_WARN, "[ViolinPei] BootShim handoff magic not found at 0x%lx\n", FixedPcdGet64 (PcdViolinHandoffBase)));
  }

  return EFI_SUCCESS;
}
