/** @file
  Virtual memory map for Xiaomi Pad 7S Pro (violin / XRING O1) and QEMU_TEST.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Library/ArmPlatformLib.h>
#include <Library/DebugLib.h>
#include <Library/HobLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PcdLib.h>

#define MAX_VIRTUAL_MEMORY_MAP_DESCRIPTORS  12

VOID
ArmPlatformGetVirtualMemoryMap (
  OUT ARM_MEMORY_REGION_DESCRIPTOR  **VirtualMemoryMap
  )
{
  ARM_MEMORY_REGION_DESCRIPTOR  *Map;
  UINTN                         Index;

  ASSERT (VirtualMemoryMap != NULL);
  Map = AllocatePages (EFI_SIZE_TO_PAGES (sizeof (ARM_MEMORY_REGION_DESCRIPTOR) * MAX_VIRTUAL_MEMORY_MAP_DESCRIPTORS));
  ASSERT (Map != NULL);
  Index = 0;

  // 1. Primary UEFI System Memory (Bank 1: 0x100000000 + PcdSystemMemorySize; 4/10 GiB QEMU / 10 GiB live violin)
  Map[Index].PhysicalBase = PcdGet64 (PcdSystemMemoryBase);
  Map[Index].VirtualBase  = PcdGet64 (PcdSystemMemoryBase);
  Map[Index].Length       = PcdGet64 (PcdSystemMemorySize);
  Map[Index].Attributes   = ARM_MEMORY_REGION_ATTRIBUTE_WRITE_BACK;
  Index++;

  // 2. Linux ramoops / pstore (0xA0000000 + 1 MiB) — Non-Cacheable so logs commit immediately
  Map[Index].PhysicalBase = 0xA0000000ULL;
  Map[Index].VirtualBase  = 0xA0000000ULL;
  Map[Index].Length       = 0x00100000ULL;
  Map[Index].Attributes   = ARM_MEMORY_REGION_ATTRIBUTE_UNCACHED_UNBUFFERED;
  Index++;

  if (FixedPcdGetBool (PcdViolinQemuTest)) {
    // QEMU virt RAM low window (0x40000000..0x60000000) for FDT/kernel handoff
    Map[Index].PhysicalBase = 0x40000000ULL;
    Map[Index].VirtualBase  = 0x40000000ULL;
    Map[Index].Length       = 0x20000000ULL;
    Map[Index].Attributes   = ARM_MEMORY_REGION_ATTRIBUTE_WRITE_BACK;
    Index++;

    // QEMU virt MMIO (GICD 0x08000000, GICR 0x080A0000, PL011 0x09000000)
    Map[Index].PhysicalBase = 0x08000000ULL;
    Map[Index].VirtualBase  = 0x08000000ULL;
    Map[Index].Length       = 0x02000000ULL;
    Map[Index].Attributes   = ARM_MEMORY_REGION_ATTRIBUTE_DEVICE;
    Index++;
  } else {
    // 3. Bootloader FDT (0x08200000) + Initrd (0x0A200000..0x0BFFFFFF)
    Map[Index].PhysicalBase = 0x08000000ULL;
    Map[Index].VirtualBase  = 0x08000000ULL;
    Map[Index].Length       = 0x04000000ULL;
    Map[Index].Attributes   = ARM_MEMORY_REGION_ATTRIBUTE_WRITE_BACK;
    Index++;

    // 4. rsv_mem_dpu_logo (0x24200000 + 256 MiB, contains active RCH9 FB @ 0x29BFF000)
    //    Mapped Non-Cacheable so GOP writes reach DPU AXI scanout instantaneously!
    Map[Index].PhysicalBase = 0x24200000ULL;
    Map[Index].VirtualBase  = 0x24200000ULL;
    Map[Index].Length       = 0x10000000ULL;
    Map[Index].Attributes   = ARM_MEMORY_REGION_ATTRIBUTE_UNCACHED_UNBUFFERED;
    Index++;

    // 5. DPU MMIO (0xE7000000 + 1 MiB, covers 0x5a000 register map)
    Map[Index].PhysicalBase = 0xE7000000ULL;
    Map[Index].VirtualBase  = 0xE7000000ULL;
    Map[Index].Length       = 0x00100000ULL;
    Map[Index].Attributes   = ARM_MEMORY_REGION_ATTRIBUTE_DEVICE;
    Index++;

    // 6. GICv3 Distributor + Redistributors (0xEBC00000 + 4 MiB)
    Map[Index].PhysicalBase = 0xEBC00000ULL;
    Map[Index].VirtualBase  = 0xEBC00000ULL;
    Map[Index].Length       = 0x00400000ULL;
    Map[Index].Attributes   = ARM_MEMORY_REGION_ATTRIBUTE_DEVICE;
    Index++;

    // 7. SPMI Controller (0xE0001000 inside 0xE0000000 + 1 MiB, Stage-2A verified EL1 Ch1)
    Map[Index].PhysicalBase = 0xE0000000ULL;
    Map[Index].VirtualBase  = 0xE0000000ULL;
    Map[Index].Length       = 0x00100000ULL;
    Map[Index].Attributes   = ARM_MEMORY_REGION_ATTRIBUTE_DEVICE;
    Index++;
  }

  // Terminator
  Map[Index].PhysicalBase = 0;
  Map[Index].VirtualBase  = 0;
  Map[Index].Length       = 0;
  Map[Index].Attributes   = (ARM_MEMORY_REGION_ATTRIBUTES)0;

  ASSERT (Index < MAX_VIRTUAL_MEMORY_MAP_DESCRIPTORS);
  *VirtualMemoryMap = Map;
}
