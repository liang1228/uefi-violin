/** @file
  BootShim -> PeilessSec handoff structure for violin (XRING O1).

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef VIOLIN_HANDOFF_H_
#define VIOLIN_HANDOFF_H_

#include <Base.h>

#define VIOLIN_HANDOFF_MAGIC  0x56494F484F423031ULL // "VIOHOB01"

typedef struct {
  UINT64    Magic;
  UINT64    FdtBase;
  UINT64    LoadPc;
  UINT64    CurrentEl;
  UINT64    Reserved;
} VIOLIN_BOOTSHIM_HANDOFF;

#endif
