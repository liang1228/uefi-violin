/** @file
  RAM-first SerialPortLib for violin (XRING O1) mirroring into Linux ramoops
  console-ramoops-0 (0xA0040000) and pmsg-ramoops-0 (0xA00C0000), plus optional
  QEMU PL011 output when PcdQemuPl011Base != 0.

  Adapted from UniXNU-dijun O1SerialPortLib (BSD-2-Clause-Patent).
  Physical XRING DWUART3 (0xEC118000) is disabled by default (Stage-1A proved
  it aborts with ESR=0x96000010 after EDK2-XRING handoff).

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Base.h>
#include <Library/ArmLib.h>
#include <Library/BaseLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/IoLib.h>
#include <Library/PcdLib.h>
#include <Library/SerialPortLib.h>

#include "O1RamLog.h"

STATIC
VOID
O1CommitRange (
  IN VOID   *Address,
  IN UINTN  Length
  )
{
  if (Length == 0) {
    return;
  }

  MemoryFence ();
  WriteBackDataCacheRange (Address, Length);
  ArmDataSynchronizationBarrier ();
}

STATIC
volatile O1_RAM_LOG_HEADER *
O1GetLogHeader (
  OUT UINT32  *Capacity
  )
{
  UINT64                      Base;
  UINT32                      Size;
  volatile O1_RAM_LOG_HEADER  *Header;

  Base = FixedPcdGet64 (PcdLogBase);
  Size = FixedPcdGet32 (PcdLogSize);
  if ((Base == 0) || ((Base & 63U) != 0) || (Size <= O1_RAM_LOG_HEADER_SIZE) || ((Size & 63U) != 0)) {
    return NULL;
  }

  *Capacity = Size - O1_RAM_LOG_HEADER_SIZE;
  Header    = (volatile O1_RAM_LOG_HEADER *)(UINTN)Base;
  if ((Header->Magic != O1_RAM_LOG_MAGIC) ||
      (Header->Version != O1_RAM_LOG_VERSION) ||
      (Header->HeaderSize != O1_RAM_LOG_HEADER_SIZE) ||
      (Header->WriteOffset >= *Capacity) ||
      (Header->Bytes > *Capacity))
  {
    Header->Magic       = 0;
    Header->Version     = O1_RAM_LOG_VERSION;
    Header->HeaderSize  = O1_RAM_LOG_HEADER_SIZE;
    Header->WriteOffset = 0;
    Header->Bytes       = 0;
    Header->TotalBytes  = 0;
    MemoryFence ();
    Header->Magic = O1_RAM_LOG_MAGIC;
    O1CommitRange ((VOID *)(UINTN)Header, sizeof (*Header));
  }

  return Header;
}

STATIC
VOID
O1MirrorPersistentRing (
  IN UINT64       Base,
  IN UINT32       Size,
  IN CONST UINT8  *Buffer,
  IN UINTN        NumberOfBytes
  )
{
  UINT32                             Capacity;
  UINT32                             Retained;
  UINT32                             Offset;
  UINT32                             First;
  UINT32                             Index;
  volatile O1_PERSISTENT_LOG_HEADER  *Header;
  volatile UINT8                     *Data;

  if ((Base == 0) || (Size <= O1_PERSISTENT_LOG_HEADER_SIZE) ||
      ((Base & 63U) != 0) || ((Size & 63U) != 0))
  {
    return;
  }

  Header   = (volatile O1_PERSISTENT_LOG_HEADER *)(UINTN)Base;
  Capacity = Size - O1_PERSISTENT_LOG_HEADER_SIZE;
  if ((Header->Signature != O1_PERSISTENT_LOG_MAGIC) ||
      (Header->Start >= Capacity) ||
      (Header->Size > Capacity))
  {
    return;
  }

  Data     = (volatile UINT8 *)(UINTN)Header + O1_PERSISTENT_LOG_HEADER_SIZE;
  Retained = (NumberOfBytes > Capacity) ? Capacity : (UINT32)NumberOfBytes;
  Offset   = (UINT32)(((UINT64)Header->Start + ((NumberOfBytes - Retained) % Capacity)) % Capacity);
  First    = MIN (Retained, Capacity - Offset);
  Buffer  += NumberOfBytes - Retained;

  for (Index = 0; Index < First; Index++) {
    Data[Offset + Index] = Buffer[Index];
  }

  for (Index = First; Index < Retained; Index++) {
    Data[Index - First] = Buffer[Index];
  }

  if (First != 0) {
    O1CommitRange ((VOID *)(UINTN)(Data + Offset), First);
  }

  if (Retained > First) {
    O1CommitRange ((VOID *)(UINTN)Data, Retained - First);
  }

  Header->Start = (UINT32)(((UINT64)Header->Start + (NumberOfBytes % Capacity)) % Capacity);
  if (NumberOfBytes >= Capacity - Header->Size) {
    Header->Size = Capacity;
  } else {
    Header->Size += (UINT32)NumberOfBytes;
  }

  O1CommitRange ((VOID *)(UINTN)Header, sizeof (*Header));
}

STATIC
VOID
O1MirrorQemuPl011 (
  IN CONST UINT8  *Buffer,
  IN UINTN        NumberOfBytes
  )
{
  UINT64  Base;
  UINTN   Index;

  Base = FixedPcdGet64 (PcdQemuPl011Base);
  if (Base == 0) {
    return;
  }

  for (Index = 0; Index < NumberOfBytes; Index++) {
    while ((MmioRead32 ((UINTN)Base + 0x18) & BIT5) != 0) {
    }
    MmioWrite32 ((UINTN)Base + 0x00, Buffer[Index]);
  }
}

RETURN_STATUS
EFIAPI
SerialPortInitialize (
  VOID
  )
{
  BOOLEAN  InterruptsEnabled;
  UINT32   Capacity;

  InterruptsEnabled = SaveAndDisableInterrupts ();
  (VOID)O1GetLogHeader (&Capacity);
  SetInterruptState (InterruptsEnabled);
  return RETURN_SUCCESS;
}

UINTN
EFIAPI
SerialPortWrite (
  IN UINT8  *Buffer,
  IN UINTN  NumberOfBytes
  )
{
  BOOLEAN                     InterruptsEnabled;
  UINT32                      Capacity;
  UINT32                      Offset;
  UINT32                      NewOffset;
  UINT32                      Retained;
  UINT32                      First;
  UINT32                      Index;
  volatile O1_RAM_LOG_HEADER  *Header;
  volatile UINT8              *Data;

  if ((Buffer == NULL) || (NumberOfBytes == 0)) {
    return 0;
  }

  InterruptsEnabled = SaveAndDisableInterrupts ();
  Header            = O1GetLogHeader (&Capacity);
  if (Header != NULL) {
    Data      = (volatile UINT8 *)(UINTN)Header + O1_RAM_LOG_HEADER_SIZE;
    Retained  = (NumberOfBytes > Capacity) ? Capacity : (UINT32)NumberOfBytes;
    Offset    = (UINT32)(((UINT64)Header->WriteOffset + ((NumberOfBytes - Retained) % Capacity)) % Capacity);
    NewOffset = (UINT32)(((UINT64)Header->WriteOffset + (NumberOfBytes % Capacity)) % Capacity);
    First     = MIN (Retained, Capacity - Offset);

    for (Index = 0; Index < First; Index++) {
      Data[Offset + Index] = Buffer[NumberOfBytes - Retained + Index];
    }

    for (Index = First; Index < Retained; Index++) {
      Data[Index - First] = Buffer[NumberOfBytes - Retained + Index];
    }

    if (First != 0) {
      O1CommitRange ((VOID *)(UINTN)(Data + Offset), First);
    }

    if (Retained > First) {
      O1CommitRange ((VOID *)(UINTN)Data, Retained - First);
    }

    Header->WriteOffset = NewOffset;
    if (NumberOfBytes >= Capacity - Header->Bytes) {
      Header->Bytes = Capacity;
    } else {
      Header->Bytes += (UINT32)NumberOfBytes;
    }

    Header->TotalBytes += NumberOfBytes;
    O1CommitRange ((VOID *)(UINTN)Header, sizeof (*Header));
  }

  O1MirrorPersistentRing (
    FixedPcdGet64 (PcdPersistentLogBase),
    FixedPcdGet32 (PcdPersistentLogSize),
    Buffer,
    NumberOfBytes
    );
  O1MirrorPersistentRing (
    FixedPcdGet64 (PcdPersistentPmsgBase),
    FixedPcdGet32 (PcdPersistentPmsgSize),
    Buffer,
    NumberOfBytes
    );
  O1MirrorQemuPl011 (Buffer, NumberOfBytes);

  SetInterruptState (InterruptsEnabled);
  return NumberOfBytes;
}

UINTN
EFIAPI
SerialPortRead (
  OUT UINT8  *Buffer,
  IN  UINTN  NumberOfBytes
  )
{
  return 0;
}

BOOLEAN
EFIAPI
SerialPortPoll (
  VOID
  )
{
  return FALSE;
}

RETURN_STATUS
EFIAPI
SerialPortSetControl (
  IN UINT32  Control
  )
{
  return (Control == 0) ? RETURN_SUCCESS : RETURN_UNSUPPORTED;
}

RETURN_STATUS
EFIAPI
SerialPortGetControl (
  OUT UINT32  *Control
  )
{
  if (Control == NULL) {
    return RETURN_INVALID_PARAMETER;
  }

  *Control = EFI_SERIAL_INPUT_BUFFER_EMPTY | EFI_SERIAL_OUTPUT_BUFFER_EMPTY;
  return RETURN_SUCCESS;
}

RETURN_STATUS
EFIAPI
SerialPortSetAttributes (
  IN OUT UINT64              *BaudRate,
  IN OUT UINT32              *ReceiveFifoDepth,
  IN OUT UINT32              *Timeout,
  IN OUT EFI_PARITY_TYPE     *Parity,
  IN OUT UINT8               *DataBits,
  IN OUT EFI_STOP_BITS_TYPE  *StopBits
  )
{
  if ((BaudRate == NULL) || (ReceiveFifoDepth == NULL) || (Timeout == NULL) ||
      (Parity == NULL) || (DataBits == NULL) || (StopBits == NULL))
  {
    return RETURN_INVALID_PARAMETER;
  }

  *BaudRate         = 115200;
  *ReceiveFifoDepth = 1;
  *Parity           = NoParity;
  *DataBits         = 8;
  *StopBits         = OneStopBit;
  return RETURN_SUCCESS;
}
