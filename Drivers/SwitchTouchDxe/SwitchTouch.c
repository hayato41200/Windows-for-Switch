/** @file
  SwitchTouchDxe: the Switch touch screen as EFI_ABSOLUTE_POINTER_PROTOCOL.

  Panel: STM FTM4 (chip ID 0x3670) at 0x49 on I2C3 (0x7000C500), IRQ PX1 (low =
  events pending). UsbPowerAuditDxe R373 powers it (LDO6 2.9 V), sets the pins and
  the I2C3 clock, puts the controller in packet mode, resets the chip and turns
  finger sensing on, about 10 s into boot (its RunTest timer). This driver only
  reads events, so it waits until the chip answers its ID, and it never changes
  the panel configuration (Windows switchtouch.sys takes over as before).

  Events (hekate bdk/input/touch.c, Windows switchtouch.c): pending count = HW reg
  0x0023 byte 1 >> 1; one event = command 0x85, 8 bytes; raw[0] low nibble 3 enter,
  4 leave, 5 motion, high nibble contact id; landscape x = raw[1] << 4 | raw[3] >> 4
  (15..1264), y = raw[2] << 4 | raw[3] & 0xF (15..704).

  Coordinates are reported in the GOP framebuffer space, 720 x 1280 portrait, with
  the mapping Windows uses on this hardware (switchtouch.c, CfgInvertX 0 /
  CfgInvertY 1): X = landscape y, Y = 1279 - landscape x. Only the first contact
  is reported; ActiveButtons bit 0 = touching.

  I2C accesses run at TPL_CALLBACK, so the R373 code (also TPL_CALLBACK) can never
  interrupt a transfer and the reverse.
**/

#include <Uefi.h>
#include <Library/UefiLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/DebugLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/IoLib.h>
#include <Protocol/AbsolutePointer.h>
#include <Protocol/DevicePath.h>

#define I2C3_BASE           0x7000C500UL
#define CAR_BASE            0x60006000UL
#define GPIO_BASE           0x6000D000UL
#define CAR_RST_DEVICES_U   0x00CU
#define CAR_CLK_OUT_ENB_U   0x018U
#define CLK_U_I2C3_BIT      (1U << 3)
#define GPIO_PX_IN          (GPIO_BASE + 0x50CU + 0x30U)
#define GPIO_PX1_BIT        (1U << 1)

// Tegra I2C registers (word index) and packet-mode bits, as UsbPowerAuditDxe.
#define I2C_CNFG            0x00
#define I2C_TX_FIFO         0x14
#define I2C_RX_FIFO         0x15
#define I2C_FIFO_CONTROL    0x17
#define I2C_FIFO_STATUS     0x18
#define I2C_INT_STATUS      0x1A
#define I2C_ERR_MASK        0x3C
#define I2C_ALL_COMPLETE    0x40
#define I2C_HDR0            0x10010
#define I2C_REPEAT_START    0x10000
#define I2C_READ_FLAG       0x80000
#define I2C_CNFG_PACKET     0x400U

#define TOUCH_ADDR          0x49
#define EV_ENTER            0x03
#define EV_LEAVE            0x04
#define EV_MOTION           0x05
#define X_REAL_MAX          1264
#define Y_REAL_MAX          704
#define EDGE_OFFSET         15
#define FB_WIDTH            720
#define FB_HEIGHT           1280
#define MAX_CONTACTS        10
#define PROBE_INTERVAL_TICKS  10000000ULL   // 1 s between chip ID tries
#define IDLE_POLL_TICKS       1000000ULL    // 100 ms: read the count even without IRQ

// Keep in sync with Application/SwitchMenuApp (SWITCH_TOUCH_DEVICE_GUID).
#define SWITCH_TOUCH_DEVICE_GUID \
  { 0x3c7f2b90, 0x6d1e, 0x4a58, { 0xb2, 0x47, 0x91, 0x0c, 0xe5, 0x3a, 0x7d, 0x66 } }

#pragma pack(1)
typedef struct {
  VENDOR_DEVICE_PATH        Vendor;
  EFI_DEVICE_PATH_PROTOCOL  End;
} TOUCH_DEVICE_PATH;
#pragma pack()

STATIC TOUCH_DEVICE_PATH mDevicePath = {
  {
    { HARDWARE_DEVICE_PATH, HW_VENDOR_DP, { sizeof (VENDOR_DEVICE_PATH), 0 } },
    SWITCH_TOUCH_DEVICE_GUID
  },
  { END_DEVICE_PATH_TYPE, END_ENTIRE_DEVICE_PATH_SUBTYPE, { sizeof (EFI_DEVICE_PATH_PROTOCOL), 0 } }
};

typedef struct {
  BOOLEAN  Active;
  UINT16   X;
  UINT16   Y;
} CONTACT;

STATIC EFI_ABSOLUTE_POINTER_PROTOCOL  mPointer;
STATIC EFI_ABSOLUTE_POINTER_MODE      mMode;
STATIC BOOLEAN                        mReady;
STATIC UINT64                         mLastProbe;
STATIC UINT64                         mLastPoll;
STATIC UINT64                         mNow;         // 100 ns ticks, from the timer below
STATIC EFI_EVENT                      mClock;
STATIC CONTACT                        mContacts[MAX_CONTACTS];
STATIC UINT8                          mPrimary = 0xFF;
STATIC BOOLEAN                        mChanged;
STATIC EFI_ABSOLUTE_POINTER_STATE     mState;

STATIC
VOID
EFIAPI
ClockTick (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  mNow += 100000ULL;   // 10 ms
}

// Packet-mode transfer on I2C3 (UsbPowerAuditDxe R373I2cXfer): optional write
// of WLen (<= 8) bytes, then optional repeated-start read of RLen (<= 32) bytes.
STATIC
BOOLEAN
I2cXfer (
  IN  CONST UINT8  *W,
  IN  UINTN        WLen,
  OUT UINT8        *R,
  IN  UINTN        RLen
  )
{
  volatile UINT32  *B = (volatile UINT32 *)I2C3_BASE;
  UINTN            I, Words;
  UINT32           Word = 0, S = 0;

  B[I2C_INT_STATUS]   = 0xFFFFFFFFU;
  B[I2C_FIFO_CONTROL] = B[I2C_FIFO_CONTROL] | 3U;
  for (I = 0; I < 10000 && (B[I2C_FIFO_CONTROL] & 3U) != 0; ++I) {
    gBS->Stall (10);
  }
  if (WLen != 0) {
    B[I2C_TX_FIFO] = I2C_HDR0;
    B[I2C_TX_FIFO] = (UINT32)(WLen - 1);
    B[I2C_TX_FIFO] = ((UINT32)TOUCH_ADDR << 1) | (RLen != 0 ? I2C_REPEAT_START : 0);
    for (I = 0; I < WLen; I += 4) {
      Word = 0;
      for (Words = 0; Words < 4 && I + Words < WLen; ++Words) {
        Word |= (UINT32)W[I + Words] << (8 * Words);
      }
      B[I2C_TX_FIFO] = Word;
    }
  }
  if (RLen != 0) {
    B[I2C_TX_FIFO] = I2C_HDR0;
    B[I2C_TX_FIFO] = (UINT32)(RLen - 1);
    B[I2C_TX_FIFO] = I2C_READ_FLAG | ((UINT32)TOUCH_ADDR << 1);
  }
  for (I = 0; I < 2000; ++I) {
    S = B[I2C_INT_STATUS];
    if ((S & (I2C_ERR_MASK | I2C_ALL_COMPLETE)) != 0) {
      break;
    }
    gBS->Stall (10);
  }
  if (((S & I2C_ERR_MASK) != 0) || ((S & I2C_ALL_COMPLETE) == 0)) {
    B[I2C_INT_STATUS] = 0xFFFFFFFFU;
    return FALSE;
  }
  Words = (RLen + 3) / 4;
  if ((RLen != 0) && ((B[I2C_FIFO_STATUS] & 0xFU) < Words)) {
    return FALSE;
  }
  for (I = 0; I < RLen; ++I) {
    if ((I % 4) == 0) {
      Word = B[I2C_RX_FIFO];
    }
    R[I] = (UINT8)(Word >> (8 * (I % 4)));
  }
  B[I2C_INT_STATUS] = S;
  return TRUE;
}

// The chip answers once R373 has powered it and set I2C3 up (clock on, out of
// reset, packet mode). Until then do not touch the controller at all.
STATIC
BOOLEAN
Probe (
  VOID
  )
{
  CONST UINT8  ChipCmd[3] = { 0xB6, 0x00, 0x04 };
  UINT8        Info[7];

  if ((MmioRead32 (CAR_BASE + CAR_CLK_OUT_ENB_U) & CLK_U_I2C3_BIT) == 0) {
    return FALSE;
  }
  if ((MmioRead32 (CAR_BASE + CAR_RST_DEVICES_U) & CLK_U_I2C3_BIT) != 0) {
    return FALSE;
  }
  if ((MmioRead32 (I2C3_BASE + I2C_CNFG * 4) & I2C_CNFG_PACKET) == 0) {
    return FALSE;
  }
  ZeroMem (Info, sizeof (Info));
  if (!I2cXfer (ChipCmd, sizeof (ChipCmd), Info, sizeof (Info))) {
    return FALSE;
  }
  DEBUG ((DEBUG_INFO, "SwitchTouchDxe: chip %02x%02x fw %02x%02x\n", Info[1], Info[2], Info[3], Info[4]));
  return (Info[1] == 0x36) && (Info[2] == 0x70);
}

STATIC
UINT16
Scale (
  IN UINT32  Raw,
  IN UINT32  RealMax,
  IN UINT32  Size
  )
{
  if (Raw < EDGE_OFFSET) {
    Raw = EDGE_OFFSET;
  }
  if (Raw > RealMax) {
    Raw = RealMax;
  }
  Raw = (Raw - EDGE_OFFSET) * Size / (RealMax - EDGE_OFFSET);
  return (UINT16)((Raw >= Size) ? Size - 1 : Raw);
}

STATIC
VOID
ParseEvent (
  IN CONST UINT8  *Ev
  )
{
  UINT8    Type = Ev[0] & 0x0F;
  UINT8    Id   = Ev[0] >> 4;
  UINT16   Lx, Ly;
  UINTN    Index;

  if (((Type != EV_ENTER) && (Type != EV_MOTION) && (Type != EV_LEAVE)) || (Id >= MAX_CONTACTS)) {
    return;
  }
  if (Type == EV_LEAVE) {
    mContacts[Id].Active = FALSE;
    if (mPrimary == Id) {
      // Hand over to another finger that is still down, if any.
      mPrimary = 0xFF;
      for (Index = 0; Index < MAX_CONTACTS; Index++) {
        if (mContacts[Index].Active) {
          mPrimary = (UINT8)Index;
          break;
        }
      }
    }
    mChanged = TRUE;
    return;
  }
  Lx = Scale (((UINT32)Ev[1] << 4) | (Ev[3] >> 4), X_REAL_MAX, FB_HEIGHT);
  Ly = Scale (((UINT32)Ev[2] << 4) | (Ev[3] & 0x0F), Y_REAL_MAX, FB_WIDTH);
  mContacts[Id].X      = Ly;                              // CfgInvertX 0
  mContacts[Id].Y      = (UINT16)(FB_HEIGHT - 1 - Lx);    // CfgInvertY 1
  mContacts[Id].Active = TRUE;
  if (mPrimary == 0xFF) {
    mPrimary = Id;
  }
  mChanged = TRUE;
}

// Drain the controller's event stack. Called at TPL_CALLBACK.
STATIC
VOID
Service (
  VOID
  )
{
  CONST UINT8  CountCmd[3] = { 0xB6, 0x00, 0x23 };
  CONST UINT8  ReadCmd     = 0x85;
  UINT8        CountBuf[2];
  UINT8        Ev[8];
  UINTN        Count, Index;
  BOOLEAN      Pending;

  if (!mReady) {
    if ((mNow - mLastProbe) < PROBE_INTERVAL_TICKS && mLastProbe != 0) {
      return;
    }
    mLastProbe = (mNow == 0) ? 1 : mNow;
    mReady     = Probe ();
    if (!mReady) {
      return;
    }
  }

  Pending = (MmioRead32 (GPIO_PX_IN) & GPIO_PX1_BIT) == 0;
  if (!Pending && ((mNow - mLastPoll) < IDLE_POLL_TICKS)) {
    return;
  }
  mLastPoll = mNow;

  CountBuf[0] = CountBuf[1] = 0;
  if (!I2cXfer (CountCmd, sizeof (CountCmd), CountBuf, sizeof (CountBuf))) {
    return;
  }
  Count = CountBuf[1] >> 1;
  if (Count > 32) {
    Count = 32;
  }
  for (Index = 0; Index < Count; Index++) {
    if (!I2cXfer (&ReadCmd, 1, Ev, sizeof (Ev))) {
      break;
    }
    ParseEvent (Ev);
  }
}

STATIC
EFI_STATUS
EFIAPI
TouchReset (
  IN EFI_ABSOLUTE_POINTER_PROTOCOL  *This,
  IN BOOLEAN                        ExtendedVerification
  )
{
  EFI_TPL  OldTpl;

  OldTpl = gBS->RaiseTPL (TPL_CALLBACK);
  ZeroMem (mContacts, sizeof (mContacts));
  mPrimary = 0xFF;
  mChanged = FALSE;
  ZeroMem (&mState, sizeof (mState));
  gBS->RestoreTPL (OldTpl);
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
TouchGetState (
  IN     EFI_ABSOLUTE_POINTER_PROTOCOL  *This,
  IN OUT EFI_ABSOLUTE_POINTER_STATE     *State
  )
{
  EFI_TPL     OldTpl;
  EFI_STATUS  Status;

  if (State == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  OldTpl = gBS->RaiseTPL (TPL_CALLBACK);
  Service ();
  if (mChanged) {
    mChanged = FALSE;
    if (mPrimary != 0xFF) {
      mState.CurrentX      = mContacts[mPrimary].X;
      mState.CurrentY      = mContacts[mPrimary].Y;
      mState.ActiveButtons = EFI_ABSP_TouchActive;
    } else {
      mState.ActiveButtons = 0;      // keep the last position on release
    }
    mState.CurrentZ = 0;
    CopyMem (State, &mState, sizeof (*State));
    Status = EFI_SUCCESS;
  } else {
    Status = EFI_NOT_READY;
  }
  gBS->RestoreTPL (OldTpl);
  return Status;
}

STATIC
VOID
EFIAPI
TouchWaitForInput (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  // Cheap: the IRQ line only. GetState still polls without it.
  if (mReady && ((MmioRead32 (GPIO_PX_IN) & GPIO_PX1_BIT) == 0)) {
    gBS->SignalEvent (Event);
  }
}

EFI_STATUS
EFIAPI
SwitchTouchDxeInitialize (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;
  EFI_HANDLE  Handle;

  mMode.AbsoluteMinX = 0;
  mMode.AbsoluteMinY = 0;
  mMode.AbsoluteMinZ = 0;
  mMode.AbsoluteMaxX = FB_WIDTH - 1;
  mMode.AbsoluteMaxY = FB_HEIGHT - 1;
  mMode.AbsoluteMaxZ = 0;
  mMode.Attributes   = 0;

  mPointer.Reset    = TouchReset;
  mPointer.GetState = TouchGetState;
  mPointer.Mode     = &mMode;

  Status = gBS->CreateEvent (EVT_NOTIFY_WAIT, TPL_NOTIFY, TouchWaitForInput, NULL, &mPointer.WaitForInput);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  // A 10 ms clock for the probe / idle-poll intervals (no I2C in it).
  Status = gBS->CreateEvent (EVT_TIMER | EVT_NOTIFY_SIGNAL, TPL_NOTIFY, ClockTick, NULL, &mClock);
  if (!EFI_ERROR (Status)) {
    gBS->SetTimer (mClock, TimerPeriodic, 100000);
  }

  Handle = NULL;
  Status = gBS->InstallMultipleProtocolInterfaces (
                  &Handle,
                  &gEfiDevicePathProtocolGuid, &mDevicePath,
                  &gEfiAbsolutePointerProtocolGuid, &mPointer,
                  NULL
                  );
  ASSERT_EFI_ERROR (Status);
  return Status;
}
