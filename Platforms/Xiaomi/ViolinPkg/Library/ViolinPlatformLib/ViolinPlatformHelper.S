//
//  ArmPlatformLib assembly helpers for violin (XRING O1).
//  SPDX-License-Identifier: BSD-2-Clause-Patent
//

#include <AsmMacroLib.h>

ASM_FUNC(ArmPlatformPeiBootAction)
  ret

ASM_FUNC(ArmPlatformIsPrimaryCore)
  mov   x0, #1
  ret

ASM_FUNC(ArmPlatformGetPrimaryCoreMpId)
  mov   x0, #0
  ret

ASM_FUNC(ArmPlatformGetCorePosition)
  mov   x0, #0
  ret
