/** @file
  Graphics Output Protocol driver for Xiaomi Pad 7S Pro (violin / XRING O1).

  Dynamically discovers the active DPU continuous-video linear scanout buffer
  from dpu@0xE7000000 (verified in Stage-1A as RCH9 @ 0x29BFF000, 3200x2136,
  stride 12800 B, XRGB_8888 == UEFI PixelBlueGreenRedReserved8BitPerColor).
  In QEMU_TEST mode, allocates a 1280x800 RAM framebuffer for end-to-end testing.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/ArmLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PcdLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/DevicePath.h>
#include <Protocol/GraphicsOutput.h>

#define DPU_SCENE0_RCH_EN_OFF   0x00300U
#define DPU_MIXER0_DST_WH_OFF   0x18004U
#define DPU_RCH_MODE_OFF        0x00U
#define DPU_RCH_LBASE_LO_OFF    0x34U
#define DPU_RCH_LBASE_HI_OFF    0x38U
#define DPU_RCH_LSTRIDE0_OFF    0x4CU
#define DPU_RCH_PIXEL_FMT_OFF   0x84U

#define DPU_LOGO_MEM_MIN        0x24200000ULL
#define DPU_LOGO_MEM_MAX        0x34200000ULL

STATIC CONST UINT32  mRchOffsets[10] = {
  0x01000U, 0x02000U, 0x0B000U, 0x0C000U, 0x0D000U,
  0x0E000U, 0x10000U, 0x11000U, 0x12000U, 0x13000U
};

#pragma pack(1)
typedef struct {
  VENDOR_DEVICE_PATH          Vendor;
  EFI_DEVICE_PATH_PROTOCOL    End;
} VIOLIN_GOP_DEVICE_PATH;
#pragma pack()

STATIC VIOLIN_GOP_DEVICE_PATH  mGopDevicePath = {
  {
    { HARDWARE_DEVICE_PATH, HW_VENDOR_DP, { (UINT8)sizeof (VENDOR_DEVICE_PATH), 0 } },
    { 0x9C7D4B12, 0x4F5A, 0x4E8B, { 0x8C, 0x21, 0x3D, 0x9E, 0x6F, 0x7A, 0x81, 0x20 } }
  },
  { END_DEVICE_PATH_TYPE, END_ENTIRE_DEVICE_PATH_SUBTYPE, { (UINT8)sizeof (EFI_DEVICE_PATH_PROTOCOL), 0 } }
};

STATIC EFI_GRAPHICS_OUTPUT_MODE_INFORMATION  mModeInfo;
STATIC EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE     mGopMode;
STATIC EFI_HANDLE                            mGopHandle;

STATIC
EFI_STATUS
EFIAPI
ViolinGopQueryMode (
  IN  EFI_GRAPHICS_OUTPUT_PROTOCOL          *This,
  IN  UINT32                                ModeNumber,
  OUT UINTN                                 *SizeOfInfo,
  OUT EFI_GRAPHICS_OUTPUT_MODE_INFORMATION  **Info
  )
{
  if ((This == NULL) || (SizeOfInfo == NULL) || (Info == NULL) || (ModeNumber >= This->Mode->MaxMode)) {
    return EFI_INVALID_PARAMETER;
  }

  *SizeOfInfo = sizeof (EFI_GRAPHICS_OUTPUT_MODE_INFORMATION);
  *Info       = AllocateCopyPool (*SizeOfInfo, &mModeInfo);
  if (*Info == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
ViolinGopSetMode (
  IN EFI_GRAPHICS_OUTPUT_PROTOCOL  *This,
  IN UINT32                        ModeNumber
  )
{
  if ((This == NULL) || (ModeNumber >= This->Mode->MaxMode)) {
    return EFI_INVALID_PARAMETER;
  }

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
ViolinGopBlt (
  IN EFI_GRAPHICS_OUTPUT_PROTOCOL       *This,
  IN OUT EFI_GRAPHICS_OUTPUT_BLT_PIXEL  *BltBuffer OPTIONAL,
  IN EFI_GRAPHICS_OUTPUT_BLT_OPERATION  BltOperation,
  IN UINTN                              SourceX,
  IN UINTN                              SourceY,
  IN UINTN                              DestinationX,
  IN UINTN                              DestinationY,
  IN UINTN                              Width,
  IN UINTN                              Height,
  IN UINTN                              Delta OPTIONAL
  )
{
  UINT32  *Fb;
  UINTN   Pitch;
  UINTN   ScreenW;
  UINTN   ScreenH;
  UINTN   BufStridePixels;
  UINTN   Row;
  UINTN   Col;

  if ((This == NULL) || (This->Mode == NULL) || (This->Mode->FrameBufferBase == 0)) {
    return EFI_INVALID_PARAMETER;
  }

  if ((Width == 0) || (Height == 0)) {
    return EFI_SUCCESS;
  }

  Fb      = (UINT32 *)(UINTN)This->Mode->FrameBufferBase;
  Pitch   = This->Mode->Info->PixelsPerScanLine;
  ScreenW = This->Mode->Info->HorizontalResolution;
  ScreenH = This->Mode->Info->VerticalResolution;

  if (Delta == 0) {
    BufStridePixels = Width;
  } else {
    BufStridePixels = Delta / sizeof (EFI_GRAPHICS_OUTPUT_BLT_PIXEL);
  }

  switch (BltOperation) {
    case EfiBltVideoFill:
      if ((BltBuffer == NULL) || (DestinationX + Width > ScreenW) || (DestinationY + Height > ScreenH)) {
        return EFI_INVALID_PARAMETER;
      }

      {
        UINT32  Pixel = *(CONST UINT32 *)BltBuffer;
        for (Row = 0; Row < Height; Row++) {
          UINT32  *Dst = Fb + (DestinationY + Row) * Pitch + DestinationX;
          for (Col = 0; Col < Width; Col++) {
            Dst[Col] = Pixel;
          }
        }
      }
      break;

    case EfiBltBufferToVideo:
      if ((BltBuffer == NULL) || (DestinationX + Width > ScreenW) || (DestinationY + Height > ScreenH)) {
        return EFI_INVALID_PARAMETER;
      }

      for (Row = 0; Row < Height; Row++) {
        CONST UINT32  *Src = (CONST UINT32 *)BltBuffer + (SourceY + Row) * BufStridePixels + SourceX;
        UINT32        *Dst = Fb + (DestinationY + Row) * Pitch + DestinationX;
        CopyMem (Dst, Src, Width * sizeof (UINT32));
      }
      break;

    case EfiBltVideoToBltBuffer:
      if ((BltBuffer == NULL) || (SourceX + Width > ScreenW) || (SourceY + Height > ScreenH)) {
        return EFI_INVALID_PARAMETER;
      }

      for (Row = 0; Row < Height; Row++) {
        CONST UINT32  *Src = Fb + (SourceY + Row) * Pitch + SourceX;
        UINT32        *Dst = (UINT32 *)BltBuffer + (DestinationY + Row) * BufStridePixels + DestinationX;
        CopyMem (Dst, Src, Width * sizeof (UINT32));
      }
      break;

    case EfiBltVideoToVideo:
      if ((SourceX + Width > ScreenW) || (SourceY + Height > ScreenH) ||
          (DestinationX + Width > ScreenW) || (DestinationY + Height > ScreenH))
      {
        return EFI_INVALID_PARAMETER;
      }

      if (DestinationY <= SourceY) {
        for (Row = 0; Row < Height; Row++) {
          CopyMem (
            Fb + (DestinationY + Row) * Pitch + DestinationX,
            Fb + (SourceY + Row) * Pitch + SourceX,
            Width * sizeof (UINT32)
            );
        }
      } else {
        for (Row = Height; Row > 0; Row--) {
          CopyMem (
            Fb + (DestinationY + Row - 1) * Pitch + DestinationX,
            Fb + (SourceY + Row - 1) * Pitch + SourceX,
            Width * sizeof (UINT32)
            );
        }
      }
      break;

    default:
      return EFI_INVALID_PARAMETER;
  }

  ArmDataSynchronizationBarrier ();
  return EFI_SUCCESS;
}

STATIC EFI_GRAPHICS_OUTPUT_PROTOCOL  mGop = {
  ViolinGopQueryMode,
  ViolinGopSetMode,
  ViolinGopBlt,
  &mGopMode
};

STATIC
VOID
DiscoverLiveDpuFramebuffer (
  OUT EFI_PHYSICAL_ADDRESS  *FbBase,
  OUT UINT32                *Width,
  OUT UINT32                *Height,
  OUT UINT32                *PixelsPerScanLine,
  OUT UINT32                *HwFormat
  )
{
  UINTN   DpuBase;
  UINT32  SceneRchEn;
  UINT32  MixerWh;
  UINT32  Idx;

  *FbBase            = FixedPcdGet64 (PcdViolinDefaultFbBase);
  *Width             = FixedPcdGet32 (PcdViolinDefaultFbWidth);
  *Height            = FixedPcdGet32 (PcdViolinDefaultFbHeight);
  *PixelsPerScanLine = *Width;
  *HwFormat          = 8; // RCH_FORMAT_XRGB_8888

  DpuBase    = (UINTN)FixedPcdGet64 (PcdViolinDpuBase);
  SceneRchEn = MmioRead32 (DpuBase + DPU_SCENE0_RCH_EN_OFF);
  MixerWh    = MmioRead32 (DpuBase + DPU_MIXER0_DST_WH_OFF);
  if (((MixerWh & 0xFFFFU) >= 640U) && (((MixerWh >> 16) & 0xFFFFU) >= 480U)) {
    *Width  = MixerWh & 0xFFFFU;
    *Height = (MixerWh >> 16) & 0xFFFFU;
  }

  for (Idx = 0; Idx < 10; Idx++) {
    UINTN   RchBase;
    UINT32  Mode;
    UINT32  Lo;
    UINT32  Hi;
    UINT64  Addr;
    UINT32  StrideBytes;
    UINT32  Fmt;

    if ((SceneRchEn & (1U << Idx)) == 0) {
      continue;
    }

    RchBase     = DpuBase + mRchOffsets[Idx];
    Mode        = MmioRead32 (RchBase + DPU_RCH_MODE_OFF);
    Lo          = MmioRead32 (RchBase + DPU_RCH_LBASE_LO_OFF);
    Hi          = MmioRead32 (RchBase + DPU_RCH_LBASE_HI_OFF) & 0xFFFU;
    Addr        = ((UINT64)Hi << 32) | (UINT64)Lo;
    StrideBytes = MmioRead32 (RchBase + DPU_RCH_LSTRIDE0_OFF) & 0xFFFFU;
    Fmt         = MmioRead32 (RchBase + DPU_RCH_PIXEL_FMT_OFF) & 0x3FU;

    DEBUG ((
      DEBUG_INFO,
      "[ViolinGop] Active RCH%u: Mode=0x%x Base=0x%lx Stride=%uB Fmt=%u Mixer=%ux%u\n",
      Idx,
      Mode,
      Addr,
      StrideBytes,
      Fmt,
      *Width,
      *Height
      ));

    if (((Mode & 1U) == 0) && (Addr >= DPU_LOGO_MEM_MIN) && (Addr < DPU_LOGO_MEM_MAX) &&
        (StrideBytes >= (*Width * 4U)) && ((StrideBytes % 4U) == 0))
    {
      *FbBase            = Addr;
      *PixelsPerScanLine = StrideBytes / 4U;
      *HwFormat          = Fmt;
      break;
    }
  }
}

EFI_STATUS
EFIAPI
ViolinGopDxeInitialize (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS            Status;
  EFI_PHYSICAL_ADDRESS  FbBase;
  UINT32                Width;
  UINT32                Height;
  UINT32                Pitch;
  UINT32                HwFmt;
  UINTN                 FbSize;

  if (FixedPcdGetBool (PcdViolinQemuTest)) {
    Width  = 1280;
    Height = 800;
    Pitch  = 1280;
    HwFmt  = 8;
    FbSize = (UINTN)Pitch * (UINTN)Height * 4U;
    Status = gBS->AllocatePages (AllocateAnyPages, EfiReservedMemoryType, EFI_SIZE_TO_PAGES (FbSize), &FbBase);
    if (EFI_ERROR (Status)) {
      return Status;
    }
    ZeroMem ((VOID *)(UINTN)FbBase, FbSize);
  } else {
    DiscoverLiveDpuFramebuffer (&FbBase, &Width, &Height, &Pitch, &HwFmt);
    FbSize = (UINTN)Pitch * (UINTN)Height * 4U;
  }

  mModeInfo.Version              = 0;
  mModeInfo.HorizontalResolution = Width;
  mModeInfo.VerticalResolution   = Height;
  mModeInfo.PixelFormat          = PixelBlueGreenRedReserved8BitPerColor;
  mModeInfo.PixelsPerScanLine    = Pitch;
  ZeroMem (&mModeInfo.PixelInformation, sizeof (mModeInfo.PixelInformation));

  mGopMode.MaxMode         = 1;
  mGopMode.Mode            = 0;
  mGopMode.Info            = &mModeInfo;
  mGopMode.SizeOfInfo      = sizeof (mModeInfo);
  mGopMode.FrameBufferBase = FbBase;
  mGopMode.FrameBufferSize = FbSize;

  Status = gBS->InstallMultipleProtocolInterfaces (
                  &mGopHandle,
                  &gEfiGraphicsOutputProtocolGuid,
                  &mGop,
                  &gEfiDevicePathProtocolGuid,
                  &mGopDevicePath,
                  NULL
                  );
  DEBUG ((
    DEBUG_INFO,
    "[ViolinGop] Installed GOP: Base=0x%lx Size=0x%lx %ux%u Pitch=%u HwFmt=%u Status=%r\n",
    FbBase,
    (UINT64)FbSize,
    Width,
    Height,
    Pitch,
    HwFmt,
    Status
    ));

  return Status;
}
