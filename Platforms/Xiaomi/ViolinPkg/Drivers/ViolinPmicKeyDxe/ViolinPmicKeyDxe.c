/** @file
  Physical key input driver for Xiaomi Pad 7S Pro (violin / XRING O1).

  On real violin hardware (!PcdViolinQemuTest):
    Polls PMIC SID 0xF register TOP_REG_PCTRL_STATUS3 (0x0E04) over non-secure
    SPMI Channel 1 (0xE00011B0 / 0xE00011A0, verified in Stage-2A with 0 traps
    and idle reading 0x39). Bits [5:3] are active-LOW:
      - Bit 4 (0x10): Volume Up   -> SCAN_UP
      - Bit 3 (0x08): Volume Down -> SCAN_DOWN
      - Bit 5 (0x20): Power Key   -> CHAR_CARRIAGE_RETURN (menu boot + confirm)

  On QEMU_TEST:
    Polls PL011 UART RX (PcdQemuPl011Base) and decodes VT100 arrow sequences
    plus k/j/u/d/Enter/1..9/Esc for automated and interactive QEMU tests.

  Publishes EFI_DEVICE_PATH_PROTOCOL + EFI_SIMPLE_TEXT_INPUT_PROTOCOL +
  EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL on one handle so ConSplitterDxe binds it to
  gST->ConIn and UniXNU detects physical input ("input yes").

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Guid/EventGroup.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PcdLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/DevicePath.h>
#include <Protocol/SimpleTextIn.h>
#include <Protocol/SimpleTextInEx.h>

#define SPMI_DEMO_VERSION_OFF         0x0000U
#define SPMI_CHANNEL_STRIDE           0x0020U
#define SPMI_BURST0_DATA0_OFF         0x0180U
#define SPMI_BURST0_CMD_OFF           0x0190U
#define SPMI_CMD_EN                   BIT31
#define SPMI_TRANS_FAIL               BIT30
#define SPMI_TRANS_DONE               BIT29
#define SPMI_CMD_EXT_READL            0x38U
#define PMIC_SLAVE_ID                 0x0FU

#define TOP_REG_PCTRL_STATUS3         0x0E04U
#define KEY_BIT_VOL_DOWN              BIT3
#define KEY_BIT_VOL_UP                BIT4
#define KEY_BIT_POWER                 BIT5
#define KEY_BITS_ALL                  (KEY_BIT_VOL_DOWN | KEY_BIT_VOL_UP | KEY_BIT_POWER)

#define POLL_PERIOD_100NS             200000ULL /* 20 ms */
#define DEBOUNCE_TICKS                2U        /* 40 ms stable press */
#define REPEAT_DELAY_TICKS            30U       /* 600 ms hold before repeat */
#define REPEAT_RATE_TICKS             10U       /* 200 ms repeat interval */

#define KEY_QUEUE_CAP                 32U
#define MAX_KEY_NOTIFIES              8U

#pragma pack(1)
typedef struct {
  VENDOR_DEVICE_PATH          Vendor;
  EFI_DEVICE_PATH_PROTOCOL    End;
} VIOLIN_KEY_DEVICE_PATH;
#pragma pack()

STATIC VIOLIN_KEY_DEVICE_PATH  mKeyDevicePath = {
  {
    { HARDWARE_DEVICE_PATH, HW_VENDOR_DP, { (UINT8)sizeof (VENDOR_DEVICE_PATH), 0 } },
    { 0x9C7D4B12, 0x4F5A, 0x4E8B, { 0x8C, 0x21, 0x3D, 0x9E, 0x6F, 0x7A, 0x81, 0x30 } }
  },
  { END_DEVICE_PATH_TYPE, END_ENTIRE_DEVICE_PATH_SUBTYPE, { (UINT8)sizeof (EFI_DEVICE_PATH_PROTOCOL), 0 } }
};

typedef struct {
  BOOLEAN               Used;
  EFI_KEY_DATA          KeyData;
  EFI_KEY_NOTIFY_FUNCTION  Callback;
} KEY_NOTIFY_ENTRY;

STATIC EFI_INPUT_KEY     mKeyQueue[KEY_QUEUE_CAP];
STATIC UINTN             mQueueHead;
STATIC UINTN             mQueueTail;
STATIC KEY_NOTIFY_ENTRY  mNotifies[MAX_KEY_NOTIFIES];

STATIC EFI_HANDLE        mKeyHandle;
STATIC EFI_EVENT         mPollTimerEvent;
STATIC EFI_EVENT         mExitBootServicesEvent;
STATIC BOOLEAN           mSpmiReady;

// Per-key debounce/repeat state: index 0 = VolUp, 1 = VolDown, 2 = Power
STATIC UINT8             mKeyHoldTicks[3];
STATIC BOOLEAN           mKeyLatchDown[3];
STATIC BOOLEAN           mKeyIgnoreUntilRelease[3];

// QEMU PL011 ANSI escape decoder state: 0 = idle, 1 = saw ESC, 2 = saw ESC '['
STATIC UINT8             mEscState;

STATIC
EFI_STATUS
SpmiReadReg1 (
  IN  UINT16  RegAddr,
  OUT UINT8   *Value
  )
{
  UINTN   Base;
  UINT32  ChOff;
  UINT32  Cmd;
  UINT32  Status;
  UINTN   Tries;

  if (Value == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  Base  = (UINTN)FixedPcdGet64 (PcdViolinSpmiBase);
  ChOff = SPMI_CHANNEL_STRIDE * FixedPcdGet32 (PcdViolinSpmiChannel);
  Cmd   = SPMI_CMD_EN | ((UINT32)(PMIC_SLAVE_ID & 0xFU) << 24) | ((UINT32)RegAddr << 8) | SPMI_CMD_EXT_READL;

  MmioWrite32 (Base + ChOff + SPMI_BURST0_CMD_OFF, Cmd);

  for (Tries = 0; Tries < 200; Tries++) {
    Status = MmioRead32 (Base + ChOff + SPMI_BURST0_CMD_OFF);
    if (((Status & SPMI_TRANS_FAIL) == 0) && ((Status & SPMI_TRANS_DONE) == 0)) {
      MicroSecondDelay (5);
      continue;
    }

    if (((Status & SPMI_TRANS_FAIL) == 0) && ((Status & SPMI_TRANS_DONE) != 0)) {
      Status = MmioRead32 (Base + ChOff + SPMI_BURST0_CMD_OFF);
      if (((Status & SPMI_TRANS_FAIL) == 0) && ((Status & SPMI_TRANS_DONE) == 0)) {
        *Value = (UINT8)(MmioRead32 (Base + ChOff + SPMI_BURST0_DATA0_OFF) & 0xFFU);
        return EFI_SUCCESS;
      }

      return EFI_DEVICE_ERROR;
    }

    if (((Status & SPMI_TRANS_FAIL) != 0) && ((Status & SPMI_TRANS_DONE) == 0)) {
      (VOID)MmioRead32 (Base + ChOff + SPMI_BURST0_CMD_OFF);
    }

    return EFI_DEVICE_ERROR;
  }

  return EFI_TIMEOUT;
}

STATIC
BOOLEAN
QueueIsEmpty (
  VOID
  )
{
  return (mQueueHead == mQueueTail);
}

STATIC
VOID
EnqueueKey (
  IN UINT16       ScanCode,
  IN CHAR16       UnicodeChar,
  IN CONST CHAR8  *Label
  )
{
  UINTN         Next;
  UINTN         Idx;
  EFI_KEY_DATA  Data;

  Next = (mQueueTail + 1U) % KEY_QUEUE_CAP;
  if (Next == mQueueHead) {
    return;
  }

  mKeyQueue[mQueueTail].ScanCode    = ScanCode;
  mKeyQueue[mQueueTail].UnicodeChar = UnicodeChar;
  mQueueTail                        = Next;

  if (Label != NULL) {
    DEBUG ((DEBUG_INFO, "[ViolinPmicKey] Enqueued %a (Scan=0x%x Char=0x%x)\n", Label, ScanCode, UnicodeChar));
  }

  ZeroMem (&Data, sizeof (Data));
  Data.Key.ScanCode               = ScanCode;
  Data.Key.UnicodeChar            = UnicodeChar;
  Data.KeyState.KeyShiftState     = EFI_SHIFT_STATE_VALID;
  Data.KeyState.KeyToggleState    = EFI_TOGGLE_STATE_VALID;

  for (Idx = 0; Idx < MAX_KEY_NOTIFIES; Idx++) {
    if (!mNotifies[Idx].Used || (mNotifies[Idx].Callback == NULL)) {
      continue;
    }

    if (((mNotifies[Idx].KeyData.Key.ScanCode == 0) || (mNotifies[Idx].KeyData.Key.ScanCode == ScanCode)) &&
        ((mNotifies[Idx].KeyData.Key.UnicodeChar == 0) || (mNotifies[Idx].KeyData.Key.UnicodeChar == UnicodeChar)))
    {
      mNotifies[Idx].Callback (&Data);
    }
  }
}

STATIC
VOID
PollPmicKeys (
  VOID
  )
{
  STATIC CONST UINT8   Masks[3]  = { KEY_BIT_VOL_UP, KEY_BIT_VOL_DOWN, KEY_BIT_POWER };
  STATIC CONST UINT16  Scans[3]  = { SCAN_UP, SCAN_DOWN, SCAN_NULL };
  STATIC CONST CHAR16  Chars[3]  = { 0, 0, CHAR_CARRIAGE_RETURN };
  STATIC CONST CHAR8   *Names[3] = { "VolUp", "VolDown", "Power" };

  UINT8  Raw;
  UINT8  Pressed;
  UINTN  Idx;

  if (!mSpmiReady) {
    return;
  }

  if (EFI_ERROR (SpmiReadReg1 (TOP_REG_PCTRL_STATUS3, &Raw))) {
    return;
  }

  // Stage-2A proved idle PCTRL_STATUS3 = 0x39 (bits 3..5 HIGH when released, LOW when pressed).
  Pressed = (UINT8)((~Raw) & KEY_BITS_ALL);

  for (Idx = 0; Idx < 3; Idx++) {
    BOOLEAN  Down = ((Pressed & Masks[Idx]) != 0);

    if (!Down) {
      mKeyHoldTicks[Idx]          = 0;
      mKeyLatchDown[Idx]          = FALSE;
      mKeyIgnoreUntilRelease[Idx] = FALSE;
      continue;
    }

    if (mKeyIgnoreUntilRelease[Idx]) {
      continue;
    }

    if (mKeyHoldTicks[Idx] < 255U) {
      mKeyHoldTicks[Idx]++;
    }

    if (!mKeyLatchDown[Idx] && (mKeyHoldTicks[Idx] >= DEBOUNCE_TICKS)) {
      mKeyLatchDown[Idx] = TRUE;
      EnqueueKey (Scans[Idx], Chars[Idx], Names[Idx]);
    } else if (mKeyLatchDown[Idx] && (Idx < 2) && (mKeyHoldTicks[Idx] >= REPEAT_DELAY_TICKS)) {
      // Allow repeat only on Volume Up/Down, never on Power key.
      if (((mKeyHoldTicks[Idx] - REPEAT_DELAY_TICKS) % REPEAT_RATE_TICKS) == 0) {
        EnqueueKey (Scans[Idx], Chars[Idx], Names[Idx]);
      }
    }
  }
}

STATIC
VOID
PollQemuPl011Rx (
  VOID
  )
{
  UINTN  Base;
  UINT8  Ch;

  Base = (UINTN)FixedPcdGet64 (PcdQemuPl011Base);
  if (Base == 0) {
    return;
  }

  while ((MmioRead32 (Base + 0x18U) & BIT4) == 0) {
    Ch = (UINT8)(MmioRead32 (Base + 0x00U) & 0xFFU);

    if (mEscState == 1) {
      if (Ch == '[') {
        mEscState = 2;
        continue;
      }

      mEscState = 0;
      EnqueueKey (SCAN_ESC, 0, "QemuEsc");
    } else if (mEscState == 2) {
      mEscState = 0;
      if (Ch == 'A') {
        EnqueueKey (SCAN_UP, 0, "QemuUp");
      } else if (Ch == 'B') {
        EnqueueKey (SCAN_DOWN, 0, "QemuDown");
      } else if (Ch == 'C') {
        EnqueueKey (SCAN_RIGHT, 0, "QemuRight");
      } else if (Ch == 'D') {
        EnqueueKey (SCAN_LEFT, 0, "QemuLeft");
      }

      continue;
    }

    if (Ch == 0x1B) {
      mEscState = 1;
    } else if ((Ch == 'k') || (Ch == 'K') || (Ch == 'u') || (Ch == 'U')) {
      EnqueueKey (SCAN_UP, 0, "QemuUp");
    } else if ((Ch == 'j') || (Ch == 'J') || (Ch == 'd') || (Ch == 'D')) {
      EnqueueKey (SCAN_DOWN, 0, "QemuDown");
    } else if ((Ch == '\r') || (Ch == '\n') || (Ch == ' ')) {
      EnqueueKey (SCAN_NULL, CHAR_CARRIAGE_RETURN, "QemuEnter");
    } else if ((Ch >= '1') && (Ch <= '9')) {
      EnqueueKey (SCAN_NULL, (CHAR16)Ch, "QemuDigit");
    }
  }

  // If a lone ESC arrived with no follow-up '[' on the same poll, flush it as SCAN_ESC on next poll.
}

STATIC
VOID
PollHardware (
  VOID
  )
{
  EFI_TPL  OldTpl;

  OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  if (FixedPcdGetBool (PcdViolinQemuTest)) {
    PollQemuPl011Rx ();
  } else {
    PollPmicKeys ();
  }
  gBS->RestoreTPL (OldTpl);
}

STATIC
VOID
EFIAPI
OnPollTimer (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  PollHardware ();
}

STATIC
VOID
EFIAPI
OnWaitForKey (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  if (!QueueIsEmpty ()) {
    gBS->SignalEvent (Event);
  }
}

STATIC
VOID
EFIAPI
OnExitBootServices (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  if (mPollTimerEvent != NULL) {
    gBS->SetTimer (mPollTimerEvent, TimerCancel, 0);
  }

  mSpmiReady = FALSE;
}

STATIC
EFI_STATUS
EFIAPI
ViolinKeyReset (
  IN EFI_SIMPLE_TEXT_INPUT_PROTOCOL  *This,
  IN BOOLEAN                         ExtendedVerification
  )
{
  mQueueHead = 0;
  mQueueTail = 0;
  mEscState  = 0;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
ViolinKeyReadKeyStroke (
  IN  EFI_SIMPLE_TEXT_INPUT_PROTOCOL  *This,
  OUT EFI_INPUT_KEY                   *Key
  )
{
  EFI_TPL  OldTpl;

  if (Key == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  PollHardware ();
  if (QueueIsEmpty ()) {
    gBS->RestoreTPL (OldTpl);
    return EFI_NOT_READY;
  }

  *Key       = mKeyQueue[mQueueHead];
  mQueueHead = (mQueueHead + 1U) % KEY_QUEUE_CAP;
  gBS->RestoreTPL (OldTpl);
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
ViolinKeyResetEx (
  IN EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL  *This,
  IN BOOLEAN                            ExtendedVerification
  )
{
  return ViolinKeyReset (NULL, ExtendedVerification);
}

STATIC
EFI_STATUS
EFIAPI
ViolinKeyReadKeyStrokeEx (
  IN  EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL  *This,
  OUT EFI_KEY_DATA                       *KeyData
  )
{
  EFI_STATUS  Status;

  if (KeyData == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  ZeroMem (KeyData, sizeof (*KeyData));
  Status = ViolinKeyReadKeyStroke (NULL, &KeyData->Key);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  KeyData->KeyState.KeyShiftState  = EFI_SHIFT_STATE_VALID;
  KeyData->KeyState.KeyToggleState = EFI_TOGGLE_STATE_VALID;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
ViolinKeySetState (
  IN EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL  *This,
  IN EFI_KEY_TOGGLE_STATE               *KeyToggleState
  )
{
  if (KeyToggleState == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
ViolinKeyRegisterKeyNotify (
  IN  EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL  *This,
  IN  EFI_KEY_DATA                       *KeyData,
  IN  EFI_KEY_NOTIFY_FUNCTION            KeyNotificationFunction,
  OUT VOID                               **NotifyHandle
  )
{
  UINTN  Idx;

  if ((KeyData == NULL) || (KeyNotificationFunction == NULL) || (NotifyHandle == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  for (Idx = 0; Idx < MAX_KEY_NOTIFIES; Idx++) {
    if (!mNotifies[Idx].Used) {
      mNotifies[Idx].Used     = TRUE;
      mNotifies[Idx].KeyData  = *KeyData;
      mNotifies[Idx].Callback = KeyNotificationFunction;
      *NotifyHandle           = &mNotifies[Idx];
      return EFI_SUCCESS;
    }
  }

  return EFI_OUT_OF_RESOURCES;
}

STATIC
EFI_STATUS
EFIAPI
ViolinKeyUnregisterKeyNotify (
  IN EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL  *This,
  IN VOID                               *NotificationHandle
  )
{
  UINTN  Idx;

  for (Idx = 0; Idx < MAX_KEY_NOTIFIES; Idx++) {
    if (mNotifies[Idx].Used && (NotificationHandle == &mNotifies[Idx])) {
      ZeroMem (&mNotifies[Idx], sizeof (mNotifies[Idx]));
      return EFI_SUCCESS;
    }
  }

  return EFI_INVALID_PARAMETER;
}

STATIC EFI_SIMPLE_TEXT_INPUT_PROTOCOL  mTextIn = {
  ViolinKeyReset,
  ViolinKeyReadKeyStroke,
  NULL
};

STATIC EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL  mTextInEx = {
  ViolinKeyResetEx,
  ViolinKeyReadKeyStrokeEx,
  NULL,
  ViolinKeySetState,
  ViolinKeyRegisterKeyNotify,
  ViolinKeyUnregisterKeyNotify
};

EFI_STATUS
EFIAPI
ViolinPmicKeyDxeInitialize (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;

  if (FixedPcdGetBool (PcdViolinQemuTest)) {
    DEBUG ((DEBUG_INFO, "[ViolinPmicKey] QEMU PL011 RX input mode (base=0x%lx)\n", FixedPcdGet64 (PcdQemuPl011Base)));
  } else {
    UINT32  SpmiVer;
    UINT8   Raw;
    UINT8   Pressed;

    SpmiVer = MmioRead32 ((UINTN)FixedPcdGet64 (PcdViolinSpmiBase) + SPMI_DEMO_VERSION_OFF);
    Status  = SpmiReadReg1 (TOP_REG_PCTRL_STATUS3, &Raw);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "[ViolinPmicKey] SPMI ver=0x%08x PCTRL_STATUS3 read failed: %r\n", SpmiVer, Status));
      return Status;
    }

    mSpmiReady = TRUE;
    Pressed    = (UINT8)((~Raw) & KEY_BITS_ALL);

    // Ignore any key already held at UEFI boot until it is released once.
    mKeyIgnoreUntilRelease[0] = ((Pressed & KEY_BIT_VOL_UP) != 0);
    mKeyIgnoreUntilRelease[1] = ((Pressed & KEY_BIT_VOL_DOWN) != 0);
    mKeyIgnoreUntilRelease[2] = ((Pressed & KEY_BIT_POWER) != 0);

    DEBUG ((
      DEBUG_INFO,
      "[ViolinPmicKey] SPMI OK: ver=0x%08x ch=%u PCTRL_STATUS3(0x0E04)=0x%02x (VolUp=%u VolDn=%u Pwr=%u)\n",
      SpmiVer,
      FixedPcdGet32 (PcdViolinSpmiChannel),
      Raw,
      mKeyIgnoreUntilRelease[0] ? 1U : 0U,
      mKeyIgnoreUntilRelease[1] ? 1U : 0U,
      mKeyIgnoreUntilRelease[2] ? 1U : 0U
      ));
  }

  Status = gBS->CreateEvent (EVT_NOTIFY_WAIT, TPL_NOTIFY, OnWaitForKey, NULL, &mTextIn.WaitForKey);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = gBS->CreateEvent (EVT_NOTIFY_WAIT, TPL_NOTIFY, OnWaitForKey, NULL, &mTextInEx.WaitForKeyEx);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = gBS->CreateEvent (EVT_TIMER | EVT_NOTIFY_SIGNAL, TPL_CALLBACK, OnPollTimer, NULL, &mPollTimerEvent);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = gBS->SetTimer (mPollTimerEvent, TimerPeriodic, POLL_PERIOD_100NS);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  gBS->CreateEventEx (
         EVT_NOTIFY_SIGNAL,
         TPL_NOTIFY,
         OnExitBootServices,
         NULL,
         &gEfiEventExitBootServicesGuid,
         &mExitBootServicesEvent
         );

  mKeyHandle = NULL;
  Status     = gBS->InstallMultipleProtocolInterfaces (
                      &mKeyHandle,
                      &gEfiDevicePathProtocolGuid,
                      &mKeyDevicePath,
                      &gEfiConsoleInDeviceGuid,
                      NULL,
                      &gEfiSimpleTextInProtocolGuid,
                      &mTextIn,
                      &gEfiSimpleTextInputExProtocolGuid,
                      &mTextInEx,
                      NULL
                      );
  DEBUG ((DEBUG_INFO, "[ViolinPmicKey] Installed SimpleTextIn/Ex + ConsoleIn + DevicePath: %r\n", Status));
  return Status;
}
