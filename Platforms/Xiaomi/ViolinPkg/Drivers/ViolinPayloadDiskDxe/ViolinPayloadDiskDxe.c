/** @file
  Read-only BlockIo driver over the BootShim-relocated FAT payload disk.

  When BootShim finds a "VNPDISK1" trailer right after VIOLIN_UEFI.fd inside
  boot_b (or QEMU -kernel), it relocates the FAT volume to 0x100800000 and
  PlatformPeiLib passes {Base, Size} via gViolinPayloadHobGuid. This driver
  publishes a 512-byte BlockIo handle so PartitionDxe + EnhancedFatDxe mount
  \EFI\UniXNU\unixnu.toml and DarwinBoot payloads automatically without
  touching vendor_boot_b, init_boot_b, userdata, or slot a.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/HobLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/BlockIo.h>
#include <Protocol/DevicePath.h>
#include <ViolinHandoff.h>

#define PAYLOAD_BLOCK_SIZE  512U

#pragma pack(1)
typedef struct {
  VENDOR_DEVICE_PATH          Vendor;
  EFI_DEVICE_PATH_PROTOCOL    End;
} VIOLIN_PAYLOAD_DEVICE_PATH;
#pragma pack()

STATIC VIOLIN_PAYLOAD_DEVICE_PATH  mPayloadDevicePath = {
  {
    { HARDWARE_DEVICE_PATH, HW_VENDOR_DP, { (UINT8)sizeof (VENDOR_DEVICE_PATH), 0 } },
    { 0x9C7D4B12, 0x4F5A, 0x4E8B, { 0x8C, 0x21, 0x3D, 0x9E, 0x6F, 0x7A, 0x81, 0x41 } }
  },
  { END_DEVICE_PATH_TYPE, END_ENTIRE_DEVICE_PATH_SUBTYPE, { (UINT8)sizeof (EFI_DEVICE_PATH_PROTOCOL), 0 } }
};

STATIC UINT8               *mPayloadBase;
STATIC UINT64              mPayloadBytes;
STATIC EFI_BLOCK_IO_MEDIA  mMedia;

STATIC
EFI_STATUS
EFIAPI
PayloadBlockReset (
  IN EFI_BLOCK_IO_PROTOCOL  *This,
  IN BOOLEAN                ExtendedVerification
  )
{
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
PayloadBlockRead (
  IN  EFI_BLOCK_IO_PROTOCOL  *This,
  IN  UINT32                 MediaId,
  IN  EFI_LBA                Lba,
  IN  UINTN                  BufferSize,
  OUT VOID                   *Buffer
  )
{
  if (Buffer == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  if (MediaId != mMedia.MediaId) {
    return EFI_MEDIA_CHANGED;
  }

  if ((BufferSize % PAYLOAD_BLOCK_SIZE) != 0) {
    return EFI_BAD_BUFFER_SIZE;
  }

  if ((Lba > mMedia.LastBlock) || ((BufferSize / PAYLOAD_BLOCK_SIZE) > (mMedia.LastBlock + 1 - Lba))) {
    return EFI_INVALID_PARAMETER;
  }

  CopyMem (Buffer, mPayloadBase + Lba * PAYLOAD_BLOCK_SIZE, BufferSize);
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
PayloadBlockWrite (
  IN EFI_BLOCK_IO_PROTOCOL  *This,
  IN UINT32                 MediaId,
  IN EFI_LBA                Lba,
  IN UINTN                  BufferSize,
  IN VOID                   *Buffer
  )
{
  return EFI_WRITE_PROTECTED;
}

STATIC
EFI_STATUS
EFIAPI
PayloadBlockFlush (
  IN EFI_BLOCK_IO_PROTOCOL  *This
  )
{
  return EFI_SUCCESS;
}

STATIC EFI_BLOCK_IO_PROTOCOL  mBlockIo = {
  EFI_BLOCK_IO_PROTOCOL_REVISION,
  &mMedia,
  PayloadBlockReset,
  PayloadBlockRead,
  PayloadBlockWrite,
  PayloadBlockFlush
};

EFI_STATUS
EFIAPI
ViolinPayloadDiskDxeInitialize (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_HOB_GUID_TYPE         *Hob;
  CONST VIOLIN_PAYLOAD_HOB  *Info;
  EFI_HANDLE                Handle;
  EFI_STATUS                Status;

  Hob = GetFirstGuidHob (&gViolinPayloadHobGuid);
  if ((Hob == NULL) || (GET_GUID_HOB_DATA_SIZE (Hob) < sizeof (VIOLIN_PAYLOAD_HOB))) {
    DEBUG ((DEBUG_INFO, "[ViolinPayloadDisk] No embedded VNPDISK1 payload HOB\n"));
    return EFI_SUCCESS;
  }

  Info = (CONST VIOLIN_PAYLOAD_HOB *)GET_GUID_HOB_DATA (Hob);
  if ((Info->Base == 0) || (Info->Size < PAYLOAD_BLOCK_SIZE)) {
    DEBUG ((DEBUG_WARN, "[ViolinPayloadDisk] Invalid payload HOB Base=0x%lx Size=0x%lx\n", Info->Base, Info->Size));
    return EFI_SUCCESS;
  }

  mPayloadBase  = (UINT8 *)(UINTN)Info->Base;
  mPayloadBytes = Info->Size - (Info->Size % PAYLOAD_BLOCK_SIZE);

  mMedia.MediaId          = 1;
  mMedia.RemovableMedia   = FALSE;
  mMedia.MediaPresent     = TRUE;
  mMedia.LogicalPartition = FALSE;
  mMedia.ReadOnly         = TRUE;
  mMedia.WriteCaching     = FALSE;
  mMedia.BlockSize        = PAYLOAD_BLOCK_SIZE;
  mMedia.IoAlign          = 1;
  mMedia.LastBlock        = (mPayloadBytes / PAYLOAD_BLOCK_SIZE) - 1;

  Handle = NULL;
  Status = gBS->InstallMultipleProtocolInterfaces (
                  &Handle,
                  &gEfiDevicePathProtocolGuid,
                  &mPayloadDevicePath,
                  &gEfiBlockIoProtocolGuid,
                  &mBlockIo,
                  NULL
                  );
  DEBUG ((
    DEBUG_INFO,
    "[ViolinPayloadDisk] Published FAT payload block device Base=0x%lx Size=0x%lx (%lu blocks): %r\n",
    Info->Base,
    mPayloadBytes,
    mMedia.LastBlock + 1,
    Status
    ));
  return Status;
}
