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

#define LZ4F_MAGIC              0x184D2204U
#define MAX_UNCOMPRESSED_DISK   (256ULL * 1024ULL * 1024ULL)

STATIC
EFI_STATUS
DecompressLz4Frame (
  IN  CONST UINT8  *Src,
  IN  UINTN        SrcSize,
  OUT UINT8        *Dst,
  IN  UINTN        DstCap,
  OUT UINTN        *OutSize
  )
{
  UINTN    SrcPos;
  UINTN    DstPos;
  UINT32   Magic;
  UINT8    Flg;
  BOOLEAN  HasBlockChecksum;
  BOOLEAN  HasContentSize;
  BOOLEAN  HasDictId;

  if ((Src == NULL) || (Dst == NULL) || (OutSize == NULL) || (SrcSize < 11)) {
    return EFI_INVALID_PARAMETER;
  }

  CopyMem (&Magic, Src, sizeof (Magic));
  if (Magic != LZ4F_MAGIC) {
    return EFI_UNSUPPORTED;
  }

  Flg = Src[4];
  if ((Flg & 0xC0U) != 0x40U) {
    return EFI_UNSUPPORTED;
  }

  HasBlockChecksum = (Flg & 0x10U) != 0;
  HasContentSize   = (Flg & 0x08U) != 0;
  HasDictId        = (Flg & 0x01U) != 0;

  SrcPos = 6; // magic(4) + FLG(1) + BD(1)
  if (HasContentSize) {
    SrcPos += 8;
  }

  if (HasDictId) {
    SrcPos += 4;
  }

  SrcPos += 1; // HC(1)
  if (SrcPos > SrcSize) {
    return EFI_COMPROMISED_DATA;
  }

  DstPos = 0;
  while (TRUE) {
    UINT32   BlockHdr;
    UINTN    BlockSize;
    UINTN    BlockEnd;
    BOOLEAN  Uncompressed;

    if (SrcPos + 4 > SrcSize) {
      return EFI_COMPROMISED_DATA;
    }

    CopyMem (&BlockHdr, Src + SrcPos, sizeof (BlockHdr));
    SrcPos += 4;
    if (BlockHdr == 0) {
      break; // EndMark
    }

    Uncompressed = (BlockHdr & 0x80000000U) != 0;
    BlockSize    = (UINTN)(BlockHdr & 0x7FFFFFFFU);
    if (BlockSize > SrcSize - SrcPos) {
      return EFI_COMPROMISED_DATA;
    }

    BlockEnd = SrcPos + BlockSize;
    if (Uncompressed) {
      if (BlockSize > DstCap - DstPos) {
        return EFI_BUFFER_TOO_SMALL;
      }

      CopyMem (Dst + DstPos, Src + SrcPos, BlockSize);
      DstPos += BlockSize;
      SrcPos  = BlockEnd;
    } else {
      while (SrcPos < BlockEnd) {
        UINT8  Token;
        UINTN  LitLen;
        UINTN  Offset;
        UINTN  MatchLen;
        UINTN  Idx;

        Token  = Src[SrcPos++];
        LitLen = (UINTN)(Token >> 4);
        if (LitLen == 15) {
          UINT8  Extra;
          do {
            if (SrcPos >= BlockEnd) {
              return EFI_COMPROMISED_DATA;
            }

            Extra   = Src[SrcPos++];
            LitLen += Extra;
          } while (Extra == 255);
        }

        if ((LitLen > BlockEnd - SrcPos) || (LitLen > DstCap - DstPos)) {
          return EFI_COMPROMISED_DATA;
        }

        if (LitLen > 0) {
          CopyMem (Dst + DstPos, Src + SrcPos, LitLen);
          SrcPos += LitLen;
          DstPos += LitLen;
        }

        if (SrcPos == BlockEnd) {
          break;
        }

        if (BlockEnd - SrcPos < 2) {
          return EFI_COMPROMISED_DATA;
        }

        Offset  = (UINTN)Src[SrcPos] | ((UINTN)Src[SrcPos + 1] << 8);
        SrcPos += 2;
        if ((Offset == 0) || (Offset > DstPos)) {
          return EFI_COMPROMISED_DATA;
        }

        MatchLen = (UINTN)(Token & 0x0FU) + 4;
        if ((Token & 0x0FU) == 15) {
          UINT8  Extra;
          do {
            if (SrcPos >= BlockEnd) {
              return EFI_COMPROMISED_DATA;
            }

            Extra     = Src[SrcPos++];
            MatchLen += Extra;
          } while (Extra == 255);
        }

        if (MatchLen > DstCap - DstPos) {
          return EFI_BUFFER_TOO_SMALL;
        }

        if (Offset >= MatchLen) {
          CopyMem (Dst + DstPos, Dst + DstPos - Offset, MatchLen);
          DstPos += MatchLen;
        } else {
          for (Idx = 0; Idx < MatchLen; Idx++) {
            Dst[DstPos] = Dst[DstPos - Offset];
            DstPos++;
          }
        }
      }
    }

    if (HasBlockChecksum) {
      if (SrcSize - SrcPos < 4) {
        return EFI_COMPROMISED_DATA;
      }

      SrcPos += 4;
    }
  }

  *OutSize = DstPos;
  return EFI_SUCCESS;
}

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
    DEBUG ((DEBUG_INFO, "[ViolinPayloadDisk] No embedded VNPDISK1/2 payload HOB\n"));
    return EFI_SUCCESS;
  }

  Info = (CONST VIOLIN_PAYLOAD_HOB *)GET_GUID_HOB_DATA (Hob);
  if ((Info->Base == 0) || (Info->Size < PAYLOAD_BLOCK_SIZE)) {
    DEBUG ((DEBUG_WARN, "[ViolinPayloadDisk] Invalid payload HOB Base=0x%lx Size=0x%lx\n", Info->Base, Info->Size));
    return EFI_SUCCESS;
  }

  if (Info->Magic == VIOLIN_PAYLOAD_LZ4_MAGIC) {
    EFI_PHYSICAL_ADDRESS  DecompAddr;
    UINTN                 OutBytes;

    if ((Info->UncompressedSize < PAYLOAD_BLOCK_SIZE) ||
        (Info->UncompressedSize > MAX_UNCOMPRESSED_DISK) ||
        ((Info->UncompressedSize % PAYLOAD_BLOCK_SIZE) != 0))
    {
      DEBUG ((DEBUG_ERROR, "[ViolinPayloadDisk] Invalid LZ4 UncompressedSize=0x%lx\n", Info->UncompressedSize));
      return EFI_INVALID_PARAMETER;
    }

    DecompAddr = 0;
    Status     = gBS->AllocatePages (
                        AllocateAnyPages,
                        EfiBootServicesData,
                        EFI_SIZE_TO_PAGES ((UINTN)Info->UncompressedSize),
                        &DecompAddr
                        );
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "[ViolinPayloadDisk] AllocatePages(0x%lx) failed: %r\n", Info->UncompressedSize, Status));
      return Status;
    }

    OutBytes = 0;
    Status   = DecompressLz4Frame (
                 (CONST UINT8 *)(UINTN)Info->Base,
                 (UINTN)Info->Size,
                 (UINT8 *)(UINTN)DecompAddr,
                 (UINTN)Info->UncompressedSize,
                 &OutBytes
                 );
    if (EFI_ERROR (Status) || (OutBytes != (UINTN)Info->UncompressedSize)) {
      DEBUG ((
        DEBUG_ERROR,
        "[ViolinPayloadDisk] LZ4 decompress failed: %r (out=0x%lx expected=0x%lx)\n",
        Status,
        (UINT64)OutBytes,
        Info->UncompressedSize
        ));
      gBS->FreePages (DecompAddr, EFI_SIZE_TO_PAGES ((UINTN)Info->UncompressedSize));
      return EFI_COMPROMISED_DATA;
    }

    mPayloadBase  = (UINT8 *)(UINTN)DecompAddr;
    mPayloadBytes = OutBytes;
    DEBUG ((
      DEBUG_INFO,
      "[ViolinPayloadDisk] LZ4 decompressed 0x%lx -> 0x%lx bytes at 0x%lx\n",
      Info->Size,
      mPayloadBytes,
      (UINT64)(UINTN)mPayloadBase
      ));
  } else {
    mPayloadBase  = (UINT8 *)(UINTN)Info->Base;
    mPayloadBytes = Info->Size - (Info->Size % PAYLOAD_BLOCK_SIZE);
  }

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
