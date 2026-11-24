/** @file
  BootShim -> PeilessSec handoff structures for violin (XRING O1).

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef VIOLIN_HANDOFF_H_
#define VIOLIN_HANDOFF_H_

#include <Base.h>

#define VIOLIN_HANDOFF_MAGIC      0x56494F484F423031ULL // "VIOHOB01"
#define VIOLIN_PAYLOAD_MAGIC      0x314B534944504E56ULL // "VNPDISK1" (raw FAT)
#define VIOLIN_PAYLOAD_LZ4_MAGIC  0x324B534944504E56ULL // "VNPDISK2" (LZ4-frame FAT)

typedef struct {
  UINT64    Magic;
  UINT64    FdtBase;
  UINT64    LoadPc;
  UINT64    CurrentEl;
  UINT64    PayloadBase;
  UINT64    PayloadSize;
  UINT64    PayloadMagic;
  UINT64    PayloadUncompressedSize;
} VIOLIN_BOOTSHIM_HANDOFF;

typedef struct {
  UINT64    Base;
  UINT64    Size;
  UINT64    Magic;
  UINT64    UncompressedSize;
} VIOLIN_PAYLOAD_HOB;

#endif
