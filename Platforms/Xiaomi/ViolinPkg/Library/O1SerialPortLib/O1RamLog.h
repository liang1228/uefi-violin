/** @file
  Fixed little-endian ABI for O1 RAM log and Linux persistent_ram rings.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef O1_RAM_LOG_H_
#define O1_RAM_LOG_H_

#include <Base.h>

#define O1_RAM_LOG_MAGIC               0x474F4C4D4152314FULL // "O1RAMLOG"
#define O1_RAM_LOG_VERSION             1U
#define O1_RAM_LOG_HEADER_SIZE         32U
#define O1_PERSISTENT_LOG_MAGIC        0x43474244U           // Linux persistent_ram "DBGC"
#define O1_PERSISTENT_LOG_HEADER_SIZE  12U

typedef struct {
  UINT32    Signature;
  UINT32    Start;
  UINT32    Size;
} O1_PERSISTENT_LOG_HEADER;

STATIC_ASSERT (sizeof (O1_PERSISTENT_LOG_HEADER) == O1_PERSISTENT_LOG_HEADER_SIZE, "Linux console log ABI");

typedef struct {
  UINT64    Magic;
  UINT32    Version;
  UINT32    HeaderSize;
  UINT32    WriteOffset;
  UINT32    Bytes;
  UINT64    TotalBytes;
} O1_RAM_LOG_HEADER;

STATIC_ASSERT (sizeof (O1_RAM_LOG_HEADER) == O1_RAM_LOG_HEADER_SIZE, "O1 log ABI size");

#endif
