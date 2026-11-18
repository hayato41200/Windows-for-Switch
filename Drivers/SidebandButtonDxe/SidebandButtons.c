/** @file
  SidebandButtonDxe: the Switch's side buttons as a UEFI keyboard.

  VOL+  (GPIO X6, active low)            -> SCAN_UP   (auto-repeat while held)
  VOL-  (GPIO X7, active low)            -> SCAN_DOWN (auto-repeat while held)
  POWER (MAX77620 ONOFFSTAT EN0, level)  -> short press: Enter (on release)
                                            held >= 1 s: Escape (once)

  Produces EFI_SIMPLE_TEXT_INPUT_PROTOCOL and EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL on
  a handle with the vendor device path below. PlatformBootManagerLib adds that
  path to ConIn, so ConSplitter merges it with the USB keyboard, and registers
  SCAN_UP as a boot-menu hot key (VOL+ opens the menu).

  A button already held when the driver starts is ignored until it is released
  (the power button that started the launch in hekate, VOL- that is hekate's own
  menu key), except VOL+: holding VOL+ through boot is how the menu is opened, and
  the auto-repeat makes sure the hot key sees it once BDS has registered it.
**/

#include <PiDxe.h>
#include <Uefi.h>
#include <Library/UefiLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/DebugLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/BaseLib.h>
#include <Library/DevicePathLib.h>
#include <Protocol/SimpleTextIn.h>
#include <Protocol/SimpleTextInEx.h>
#include <Protocol/DevicePath.h>

#include <Foundation/Types.h>
#include <Device/T210.h>
#include <Library/I2cLib.h>
#include <Device/Max77620.h>
#include <Library/Max7762xLib.h>
#include <Library/GpioLib.h>
#include <Protocol/Pmic.h>
#include <Device/StaticDevices.h>

// 20 ms tick; the PMIC (I2C5) is read every 5th tick.
#define SBB_POLL_INTERVAL     200000
#define SBB_PMIC_DIVIDER      5
#define SBB_DEBOUNCE_TICKS    2      // 40 ms stable
#define SBB_REPEAT_DELAY      25     // 500 ms
#define SBB_REPEAT_RATE       8      // 160 ms
#define SBB_POWER_LONG_TICKS  50     // 1 s

#define SBB_QUEUE_SIZE        16

// Keep in sync with Library/PlatformBootManagerLib/PlatformBm.c (mSidebandButtons).
#define SIDEBAND_BUTTON_DEVICE_GUID \
  { 0x8e2d6a14, 0x3b5f, 0x4c71, { 0x9a, 0x0e, 0x5d, 0x2c, 0x61, 0x7b, 0x48, 0xf3 } }

#pragma pack(1)
typedef struct {
  VENDOR_DEVICE_PATH        Vendor;
  EFI_DEVICE_PATH_PROTOCOL  End;
} SBB_DEVICE_PATH;
#pragma pack()

STATIC SBB_DEVICE_PATH mDevicePath = {
  {
    { HARDWARE_DEVICE_PATH, HW_VENDOR_DP, { sizeof (VENDOR_DEVICE_PATH), 0 } },
    SIDEBAND_BUTTON_DEVICE_GUID
  },
  { END_DEVICE_PATH_TYPE, END_ENTIRE_DEVICE_PATH_SUBTYPE, { sizeof (EFI_DEVICE_PATH_PROTOCOL), 0 } }
};

#define SBB_NOTIFY_SIGNATURE  SIGNATURE_32 ('s', 'b', 'b', 'n')

typedef struct {
  UINTN                    Signature;
  LIST_ENTRY               Link;
  EFI_KEY_DATA             KeyData;
  EFI_KEY_NOTIFY_FUNCTION  Notify;
} SBB_NOTIFY;

typedef struct {
  BOOLEAN  Raw;        // last sample
  UINT8    Stable;     // ticks the raw value has not changed
  BOOLEAN  Pressed;    // debounced state
  BOOLEAN  Armed;      // FALSE until seen released (held at start)
  UINT32   HeldTicks;  // ticks since the debounced press
} SBB_BUTTON;

enum { BTN_VOL_UP, BTN_VOL_DOWN, BTN_POWER, BTN_COUNT };

STATIC EFI_EVENT       mPollTimer;
STATIC PMIC_PROTOCOL   *mPmicProtocol;
STATIC SBB_BUTTON      mButtons[BTN_COUNT];
STATIC UINT32          mTick;
STATIC BOOLEAN         mPowerLongSent;

STATIC EFI_KEY_DATA    mQueue[SBB_QUEUE_SIZE];
STATIC UINTN           mQueueHead;
STATIC UINTN           mQueueCount;
STATIC LIST_ENTRY      mNotifyList = INITIALIZE_LIST_HEAD_VARIABLE (mNotifyList);

STATIC EFI_SIMPLE_TEXT_INPUT_PROTOCOL     mTextIn;
STATIC EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL  mTextInEx;

STATIC
BOOLEAN
QueueEmpty (
  VOID
  )
{
  EFI_TPL  OldTpl;
  BOOLEAN  Empty;

  OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  Empty  = (mQueueCount == 0);
  gBS->RestoreTPL (OldTpl);
  return Empty;
}

STATIC
VOID
EmitKey (
  IN UINT16  ScanCode,
  IN CHAR16  UnicodeChar
  )
{
  EFI_KEY_DATA  Key;
  EFI_TPL       OldTpl;
  LIST_ENTRY    *Link;
  SBB_NOTIFY    *Entry;

  ZeroMem (&Key, sizeof (Key));
  Key.Key.ScanCode    = ScanCode;
  Key.Key.UnicodeChar = UnicodeChar;

  OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  if (mQueueCount < SBB_QUEUE_SIZE) {
    CopyMem (&mQueue[(mQueueHead + mQueueCount) % SBB_QUEUE_SIZE], &Key, sizeof (Key));
    mQueueCount++;
  }
  gBS->RestoreTPL (OldTpl);

  // Called from the poll timer at TPL_CALLBACK (as BmHotkeyCallback expects).
  for (Link = GetFirstNode (&mNotifyList); !IsNull (&mNotifyList, Link); Link = GetNextNode (&mNotifyList, Link)) {
    Entry = CR (Link, SBB_NOTIFY, Link, SBB_NOTIFY_SIGNATURE);
    if ((Entry->KeyData.Key.ScanCode == ScanCode) && (Entry->KeyData.Key.UnicodeChar == UnicodeChar)) {
      Entry->Notify (&Key);
    }
  }
}

STATIC
EFI_STATUS
Dequeue (
  OUT EFI_KEY_DATA  *Key
  )
{
  EFI_TPL     OldTpl;
  EFI_STATUS  Status;

  OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  if (mQueueCount == 0) {
    Status = EFI_NOT_READY;
  } else {
    CopyMem (Key, &mQueue[mQueueHead], sizeof (*Key));
    mQueueHead = (mQueueHead + 1) % SBB_QUEUE_SIZE;
    mQueueCount--;
    Status = EFI_SUCCESS;
  }
  gBS->RestoreTPL (OldTpl);
  return Status;
}

// Debounce one sample. Returns TRUE when the debounced state changed.
STATIC
BOOLEAN
Debounce (
  IN OUT SBB_BUTTON  *Button,
  IN     BOOLEAN     Raw,
  IN     UINT8       Needed
  )
{
  if (Raw != Button->Raw) {
    Button->Raw    = Raw;
    Button->Stable = 0;
  } else if (Button->Stable < 0xFF) {
    Button->Stable++;
  }

  if ((Button->Stable + 1 >= Needed) && (Button->Pressed != Raw)) {
    Button->Pressed   = Raw;
    Button->HeldTicks = 0;
    if (!Raw) {
      Button->Armed = TRUE;
    }
    return TRUE;
  }

  if (Button->Pressed) {
    Button->HeldTicks++;
  }
  return FALSE;
}

STATIC
VOID
VolumeKey (
  IN SBB_BUTTON  *Button,
  IN BOOLEAN     Changed,
  IN UINT16      ScanCode
  )
{
  if (!Button->Pressed || !Button->Armed) {
    return;
  }
  if (Changed) {
    EmitKey (ScanCode, CHAR_NULL);
  } else if ((Button->HeldTicks >= SBB_REPEAT_DELAY) &&
             (((Button->HeldTicks - SBB_REPEAT_DELAY) % SBB_REPEAT_RATE) == 0)) {
    EmitKey (ScanCode, CHAR_NULL);
  }
}

STATIC
VOID
EFIAPI
SidebandButtonsPoll (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  BOOLEAN  Changed;
  BOOLEAN  PowerArmed;

  mTick++;

  Changed = Debounce (&mButtons[BTN_VOL_UP], !gpio_read (GPIO_BY_NAME (BUTTON_VOL_UP)), SBB_DEBOUNCE_TICKS);
  VolumeKey (&mButtons[BTN_VOL_UP], Changed, SCAN_UP);

  Changed = Debounce (&mButtons[BTN_VOL_DOWN], !gpio_read (GPIO_BY_NAME (BUTTON_VOL_DOWN)), SBB_DEBOUNCE_TICKS);
  VolumeKey (&mButtons[BTN_VOL_DOWN], Changed, SCAN_DOWN);

  if ((mTick % SBB_PMIC_DIVIDER) == 0) {
    // The power button is sampled every 100 ms: one stable repeat is enough.
    // The release of a press held since start only arms it (no Enter).
    PowerArmed = mButtons[BTN_POWER].Armed;
    Changed    = Debounce (&mButtons[BTN_POWER], mPmicProtocol->QueryPowerButton () ? TRUE : FALSE, 2);
    if (PowerArmed) {
      if (Changed && mButtons[BTN_POWER].Pressed) {
        mPowerLongSent = FALSE;
      } else if (Changed && !mButtons[BTN_POWER].Pressed) {
        if (!mPowerLongSent) {
          EmitKey (SCAN_NULL, CHAR_CARRIAGE_RETURN);
        }
        mPowerLongSent = FALSE;
      } else if (mButtons[BTN_POWER].Pressed && !mPowerLongSent &&
                 (mButtons[BTN_POWER].HeldTicks * SBB_PMIC_DIVIDER >= SBB_POWER_LONG_TICKS)) {
        mPowerLongSent = TRUE;
        EmitKey (SCAN_ESC, CHAR_NULL);
      }
    }
  }
}

//
// EFI_SIMPLE_TEXT_INPUT_PROTOCOL
//

STATIC
EFI_STATUS
EFIAPI
SbbReset (
  IN EFI_SIMPLE_TEXT_INPUT_PROTOCOL  *This,
  IN BOOLEAN                         ExtendedVerification
  )
{
  EFI_TPL  OldTpl;

  OldTpl      = gBS->RaiseTPL (TPL_NOTIFY);
  mQueueCount = 0;
  gBS->RestoreTPL (OldTpl);
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
SbbReadKeyStroke (
  IN  EFI_SIMPLE_TEXT_INPUT_PROTOCOL  *This,
  OUT EFI_INPUT_KEY                   *Key
  )
{
  EFI_KEY_DATA  KeyData;
  EFI_STATUS    Status;

  if (Key == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  Status = Dequeue (&KeyData);
  if (!EFI_ERROR (Status)) {
    CopyMem (Key, &KeyData.Key, sizeof (*Key));
  }
  return Status;
}

STATIC
VOID
EFIAPI
SbbWaitForKey (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  if (!QueueEmpty ()) {
    gBS->SignalEvent (Event);
  }
}

//
// EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL
//

STATIC
EFI_STATUS
EFIAPI
SbbResetEx (
  IN EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL  *This,
  IN BOOLEAN                            ExtendedVerification
  )
{
  return SbbReset (&mTextIn, ExtendedVerification);
}

STATIC
EFI_STATUS
EFIAPI
SbbReadKeyStrokeEx (
  IN  EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL  *This,
  OUT EFI_KEY_DATA                       *KeyData
  )
{
  if (KeyData == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  return Dequeue (KeyData);
}

STATIC
EFI_STATUS
EFIAPI
SbbSetState (
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
SbbRegisterKeyNotify (
  IN  EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL  *This,
  IN  EFI_KEY_DATA                       *KeyData,
  IN  EFI_KEY_NOTIFY_FUNCTION            KeyNotificationFunction,
  OUT VOID                               **NotifyHandle
  )
{
  SBB_NOTIFY  *Entry;
  EFI_TPL     OldTpl;

  if ((KeyData == NULL) || (KeyNotificationFunction == NULL) || (NotifyHandle == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  Entry = AllocateZeroPool (sizeof (*Entry));
  if (Entry == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }
  Entry->Signature = SBB_NOTIFY_SIGNATURE;
  Entry->Notify    = KeyNotificationFunction;
  CopyMem (&Entry->KeyData, KeyData, sizeof (*KeyData));

  OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  InsertTailList (&mNotifyList, &Entry->Link);
  gBS->RestoreTPL (OldTpl);

  *NotifyHandle = Entry;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
SbbUnregisterKeyNotify (
  IN EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL  *This,
  IN VOID                               *NotificationHandle
  )
{
  LIST_ENTRY  *Link;
  SBB_NOTIFY  *Entry;
  EFI_TPL     OldTpl;

  if (NotificationHandle == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  for (Link = GetFirstNode (&mNotifyList); !IsNull (&mNotifyList, Link); Link = GetNextNode (&mNotifyList, Link)) {
    Entry = CR (Link, SBB_NOTIFY, Link, SBB_NOTIFY_SIGNATURE);
    if (Entry == NotificationHandle) {
      RemoveEntryList (&Entry->Link);
      gBS->RestoreTPL (OldTpl);
      FreePool (Entry);
      return EFI_SUCCESS;
    }
  }
  gBS->RestoreTPL (OldTpl);
  return EFI_INVALID_PARAMETER;
}

EFI_STATUS
EFIAPI
SidebandButtonDxeInitialize (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;
  EFI_HANDLE  Handle;
  UINTN       Index;

  Status = gBS->LocateProtocol (&gPmicProtocolGuid, NULL, (VOID **)&mPmicProtocol);
  if (EFI_ERROR (Status)) {
    goto exit;
  }

  // A button held right now stays ignored until released, except VOL+.
  mButtons[BTN_VOL_UP].Raw   = !gpio_read (GPIO_BY_NAME (BUTTON_VOL_UP));
  mButtons[BTN_VOL_DOWN].Raw = !gpio_read (GPIO_BY_NAME (BUTTON_VOL_DOWN));
  mButtons[BTN_POWER].Raw    = mPmicProtocol->QueryPowerButton () ? TRUE : FALSE;
  for (Index = 0; Index < BTN_COUNT; Index++) {
    mButtons[Index].Pressed = mButtons[Index].Raw;
    mButtons[Index].Armed   = !mButtons[Index].Raw;
  }
  mButtons[BTN_VOL_UP].Pressed = FALSE;   // a held VOL+ is a fresh press
  mButtons[BTN_VOL_UP].Armed   = TRUE;
  DEBUG ((DEBUG_INFO, "SidebandButtonDxe: at start VOL+=%d VOL-=%d POWER=%d\n",
    mButtons[BTN_VOL_UP].Raw, mButtons[BTN_VOL_DOWN].Raw, mButtons[BTN_POWER].Raw));

  mTextIn.Reset              = SbbReset;
  mTextIn.ReadKeyStroke      = SbbReadKeyStroke;
  mTextInEx.Reset            = SbbResetEx;
  mTextInEx.ReadKeyStrokeEx  = SbbReadKeyStrokeEx;
  mTextInEx.SetState         = SbbSetState;
  mTextInEx.RegisterKeyNotify   = SbbRegisterKeyNotify;
  mTextInEx.UnregisterKeyNotify = SbbUnregisterKeyNotify;

  Status = gBS->CreateEvent (EVT_NOTIFY_WAIT, TPL_NOTIFY, SbbWaitForKey, NULL, &mTextIn.WaitForKey);
  if (EFI_ERROR (Status)) {
    goto exit;
  }
  Status = gBS->CreateEvent (EVT_NOTIFY_WAIT, TPL_NOTIFY, SbbWaitForKey, NULL, &mTextInEx.WaitForKeyEx);
  if (EFI_ERROR (Status)) {
    goto exit;
  }

  Status = gBS->CreateEvent (EVT_NOTIFY_SIGNAL | EVT_TIMER, TPL_CALLBACK, SidebandButtonsPoll, NULL, &mPollTimer);
  if (EFI_ERROR (Status)) {
    goto exit;
  }
  Status = gBS->SetTimer (mPollTimer, TimerPeriodic, SBB_POLL_INTERVAL);
  if (EFI_ERROR (Status)) {
    goto exit;
  }

  Handle = NULL;
  Status = gBS->InstallMultipleProtocolInterfaces (
                  &Handle,
                  &gEfiDevicePathProtocolGuid, &mDevicePath,
                  &gEfiSimpleTextInProtocolGuid, &mTextIn,
                  &gEfiSimpleTextInputExProtocolGuid, &mTextInEx,
                  NULL
                  );

exit:
  ASSERT_EFI_ERROR (Status);
  return Status;
}
