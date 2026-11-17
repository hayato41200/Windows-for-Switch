/** @file
  SwitchMenuApp: a touch boot menu for the Switch UEFI.

  Opened by holding VOL+ during boot (PlatformBootManagerLib hot key), and when
  nothing could be booted. Four large buttons, drawn with GOP in landscape:

    Boot Windows      the Windows Boot Manager option (else the first boot option)
    Return to hekate  reboot-to-payload with \bootloader\update.bin from the SD,
                      the same steps as the Windows power button driver
                      (switchpwrbtn: SCLK, IRAM 0x40010000, PMC scratch, MAIN_RST);
                      without the file, on the hekate chain: PSCI reset (bl31 R2P)
    UEFI settings     the stock Boot Manager / setup (UiApp)
    Power off         MAX77620 ONOFFCNFG2 &= ~SFT_RST_WK, ONOFFCNFG1 = SFT_RST,
                      then PWR_OFF (switchpwrbtn values)

  Input: touch (SwitchTouchDxe, EFI_ABSOLUTE_POINTER in framebuffer coordinates;
  a button acts when the finger is lifted inside it), or the keys: VOL+/VOL- (Up /
  Down) move the highlight, POWER (Enter) selects, held POWER (Esc) leaves the menu.

  The framebuffer is 720 x 1280 portrait; the menu is laid out on a 1280 x 720
  landscape canvas and turned into it. "Rotate" turns the picture by 180 degrees
  and remembers it in the SwitchMenuRotation variable.
**/

#include <Uefi.h>
#include <Library/UefiLib.h>
#include <Library/UefiApplicationEntryPoint.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include <Library/PrintLib.h>
#include <Library/IoLib.h>
#include <Library/DevicePathLib.h>
#include <Library/UefiBootManagerLib.h>
#include <Library/I2cLib.h>
#include <Library/ArmLib.h>
#include <Protocol/GraphicsOutput.h>
#include <Protocol/AbsolutePointer.h>
#include <Protocol/SimpleFileSystem.h>
#include <Guid/FileInfo.h>

#include "Font.h"

// ---------------------------------------------------------------- constants

#define LW  1280          // landscape canvas
#define LH  720

#define COLOR_BG        0x00101418
#define COLOR_HEADER    0x001A2129
#define COLOR_BUTTON    0x002A3440
#define COLOR_PRESSED   0x002F7FC0
#define COLOR_FOCUS     0x003FA9F5
#define COLOR_TEXT      0x00FFFFFF
#define COLOR_SUBTEXT   0x00A8B8C8
#define COLOR_WARN      0x00FFC040
#define COLOR_MARK      0x00FF4060

// Keep in sync with Drivers/SwitchTouchDxe (SWITCH_TOUCH_DEVICE_GUID).
STATIC EFI_GUID  mTouchDeviceGuid = { 0x3c7f2b90, 0x6d1e, 0x4a58, { 0xb2, 0x47, 0x91, 0x0c, 0xe5, 0x3a, 0x7d, 0x66 } };
// SwitchMenuRotation variable (UINT8: 0 or 1).
STATIC EFI_GUID  mMenuVarGuid     = { 0x9d41c2e7, 0x58a3, 0x4f0b, { 0x8e, 0x16, 0x2b, 0x7c, 0xd0, 0x94, 0x31, 0xa5 } };

// R2P (switchpwrbtn.c / switch-atf r2p_reboot_to_payload)
#define CAR_BASE               0x60006000UL
#define CAR_SCLK_BURST_POLICY  0x028
#define CAR_SCLK_RATE          0x030
#define SCLK_BURST_POLICY_POR  0x10000000U
#define PMC_BASE               0x7000E400UL
#define PMC_CNTRL              0x000
#define PMC_CNTRL_MAIN_RST     0x10U
#define PMC_SCRATCH0           0x050
#define PMC_SCRATCH0_WARMBOOT  0x01U
#define PMC_SCRATCH33          0x120
#define PMC_SCRATCH40          0x13C
#define PMC_SCRATCH45          0x234
#define PMC_SCRATCH46          0x238
#define MISC_GP_HIDREV         0x70000804UL
#define IRAM_PAYLOAD           0x40010000UL
#define IRAM_PAYLOAD_SIZE      0x30000U
#define PAYLOAD_MIN_SIZE       0x200U
#define PAYLOAD_BOOTCFG        0x94
#define PAYLOAD_BOOTCFG_SIZE   32

// MAX77620 power off
#define MAX77620_ADDR          0x3C
#define MAX77620_ONOFFCNFG1    0x41
#define MAX77620_ONOFFCNFG2    0x42
#define ONOFFCNFG1_SFT_RST     0x80
#define ONOFFCNFG1_PWR_OFF     0x02
#define ONOFFCNFG2_SFT_RST_WK  0x80

// ---------------------------------------------------------------- layout

typedef enum { ActBootWindows, ActHekate, ActSettings, ActPowerOff, ActRotate, ActCount } MENU_ACTION;

typedef struct {
  UINTN          X, Y, W, H;
  CONST CHAR16   *Label;
  CONST CHAR16   *Sub;
  UINTN          Scale;
} MENU_BUTTON;

STATIC MENU_BUTTON mButtons[ActCount] = {
  {   40, 120, 580, 230, L"Boot Windows",     L"Windows Boot Manager",       4 },
  {  660, 120, 580, 230, L"Return to hekate", L"Reboot to the hekate menu",  4 },
  {   40, 380, 580, 230, L"UEFI settings",    L"Boot Manager and setup",     4 },
  {  660, 380, 580, 230, L"Power off",        L"Turn the Switch off",        4 },
  { 1060,  14, 190,  64, L"Rotate",           NULL,                          2 },
};

// ---------------------------------------------------------------- state

STATIC EFI_GRAPHICS_OUTPUT_PROTOCOL    *mGop;
STATIC EFI_ABSOLUTE_POINTER_PROTOCOL   *mTouch;
STATIC UINT32                          *mFb;          // framebuffer-shaped back buffer
STATIC UINTN                           mFbW, mFbH;
STATIC BOOLEAN                         mPortrait;     // framebuffer taller than wide
STATIC UINT8                           mRotation;     // 0 or 1 (180 degrees)
STATIC UINTN                           mFocus;
STATIC INTN                            mPressed = -1;
STATIC BOOLEAN                         mTouching;
STATIC INTN                            mTouchX = -1, mTouchY = -1;   // landscape
STATIC CHAR16                          mStatus[128];
STATIC UINT32                          mStatusColor = COLOR_SUBTEXT;

// ---------------------------------------------------------------- drawing

// Landscape (Lx, Ly) -> framebuffer index. Portrait framebuffer: the landscape
// top edge is the framebuffer's left column (rotation 0), as the touch mapping.
STATIC
UINTN
FbIndex (
  IN UINTN  Lx,
  IN UINTN  Ly
  )
{
  UINTN  Fx, Fy;

  if (mPortrait) {
    if (mRotation == 0) {
      Fx = Ly;
      Fy = mFbH - 1 - Lx;
    } else {
      Fx = mFbW - 1 - Ly;
      Fy = Lx;
    }
  } else {
    if (mRotation == 0) {
      Fx = Lx;
      Fy = Ly;
    } else {
      Fx = mFbW - 1 - Lx;
      Fy = mFbH - 1 - Ly;
    }
  }
  return Fy * mFbW + Fx;
}

// Framebuffer (Fx, Fy) -> landscape; the inverse of FbIndex.
STATIC
VOID
FbToLandscape (
  IN  UINTN  Fx,
  IN  UINTN  Fy,
  OUT INTN   *Lx,
  OUT INTN   *Ly
  )
{
  if (mPortrait) {
    if (mRotation == 0) {
      *Lx = (INTN)(mFbH - 1 - Fy);
      *Ly = (INTN)Fx;
    } else {
      *Lx = (INTN)Fy;
      *Ly = (INTN)(mFbW - 1 - Fx);
    }
  } else {
    if (mRotation == 0) {
      *Lx = (INTN)Fx;
      *Ly = (INTN)Fy;
    } else {
      *Lx = (INTN)(mFbW - 1 - Fx);
      *Ly = (INTN)(mFbH - 1 - Fy);
    }
  }
}

STATIC
VOID
FillRect (
  IN UINTN   X,
  IN UINTN   Y,
  IN UINTN   W,
  IN UINTN   H,
  IN UINT32  Color
  )
{
  UINTN  Lx, Ly;

  for (Ly = Y; Ly < Y + H && Ly < LH; Ly++) {
    for (Lx = X; Lx < X + W && Lx < LW; Lx++) {
      mFb[FbIndex (Lx, Ly)] = Color;
    }
  }
}

STATIC
VOID
FrameRect (
  IN UINTN   X,
  IN UINTN   Y,
  IN UINTN   W,
  IN UINTN   H,
  IN UINTN   T,
  IN UINT32  Color
  )
{
  FillRect (X, Y, W, T, Color);
  FillRect (X, Y + H - T, W, T, Color);
  FillRect (X, Y, T, H, Color);
  FillRect (X + W - T, Y, T, H, Color);
}

STATIC
UINTN
TextWidth (
  IN CONST CHAR16  *Text,
  IN UINTN         Scale
  )
{
  return StrLen (Text) * FONT_WIDTH * Scale;
}

STATIC
VOID
DrawText (
  IN UINTN         X,
  IN UINTN         Y,
  IN UINTN         Scale,
  IN UINT32        Color,
  IN CONST CHAR16  *Text
  )
{
  UINTN   Row, Col, Sx, Sy;
  CHAR16  C;
  UINT8   Bits;

  for (; *Text != L'\0'; Text++, X += FONT_WIDTH * Scale) {
    C = *Text;
    if ((C < FONT_FIRST) || (C > FONT_LAST)) {
      C = L'?';
    }
    for (Row = 0; Row < FONT_HEIGHT; Row++) {
      Bits = mFont[C - FONT_FIRST][Row];
      for (Col = 0; Col < FONT_WIDTH; Col++) {
        if ((Bits & (0x80 >> Col)) == 0) {
          continue;
        }
        for (Sy = 0; Sy < Scale; Sy++) {
          for (Sx = 0; Sx < Scale; Sx++) {
            if ((X + Col * Scale + Sx < LW) && (Y + Row * Scale + Sy < LH)) {
              mFb[FbIndex (X + Col * Scale + Sx, Y + Row * Scale + Sy)] = Color;
            }
          }
        }
      }
    }
  }
}

STATIC
VOID
DrawButton (
  IN UINTN  Index
  )
{
  MENU_BUTTON  *B = &mButtons[Index];
  UINT32       Fill;
  UINTN        TextH, SubH, Top;

  Fill = ((INTN)Index == mPressed) ? COLOR_PRESSED : COLOR_BUTTON;
  FillRect (B->X, B->Y, B->W, B->H, Fill);
  if (Index == mFocus) {
    FrameRect (B->X, B->Y, B->W, B->H, 6, COLOR_FOCUS);
  }
  TextH = FONT_HEIGHT * B->Scale;
  SubH  = (B->Sub != NULL) ? FONT_HEIGHT * 2 + 16 : 0;
  Top   = B->Y + (B->H - TextH - SubH) / 2;
  DrawText (B->X + (B->W - TextWidth (B->Label, B->Scale)) / 2, Top, B->Scale, COLOR_TEXT, B->Label);
  if (B->Sub != NULL) {
    DrawText (B->X + (B->W - TextWidth (B->Sub, 2)) / 2, Top + TextH + 16, 2, COLOR_SUBTEXT, B->Sub);
  }
}

STATIC
VOID
Render (
  VOID
  )
{
  UINTN   Index;
  CHAR16  Line[96];

  FillRect (0, 0, LW, LH, COLOR_BG);
  FillRect (0, 0, LW, 92, COLOR_HEADER);
  DrawText (40, 18, 3, COLOR_TEXT, L"Switch boot menu");
  for (Index = 0; Index < ActCount; Index++) {
    DrawButton (Index);
  }

  DrawText (40, 636, 2, COLOR_SUBTEXT, L"Touch a button, or VOL+ / VOL- to move and POWER to select.");
  if (mStatus[0] != L'\0') {
    DrawText (40, 676, 2, mStatusColor, mStatus);
  } else if (mTouch == NULL) {
    DrawText (40, 676, 2, COLOR_SUBTEXT, L"Touch screen: no driver.");
  } else if (mTouchX >= 0) {
    UnicodeSPrint (Line, sizeof (Line), L"Touch: %d, %d%s", mTouchX, mTouchY, mTouching ? L" (down)" : L"");
    DrawText (40, 676, 2, COLOR_SUBTEXT, Line);
  } else {
    DrawText (40, 676, 2, COLOR_SUBTEXT, L"Touch: waiting (works once the panel is up, ~10 s after power on).");
  }

  // Touch marker (check of the touch mapping).
  if (mTouchX >= 0) {
    FillRect ((mTouchX >= 20) ? (UINTN)mTouchX - 20 : 0, (UINTN)mTouchY, 41, 3, COLOR_MARK);
    FillRect ((UINTN)mTouchX, (mTouchY >= 20) ? (UINTN)mTouchY - 20 : 0, 3, 41, COLOR_MARK);
  }

  mGop->Blt (mGop, (EFI_GRAPHICS_OUTPUT_BLT_PIXEL *)mFb, EfiBltBufferToVideo, 0, 0, 0, 0, mFbW, mFbH, 0);
}

// Something else (the UsbPowerAuditDxe screens, UiApp) drew over the menu?
STATIC
BOOLEAN
Overwritten (
  VOID
  )
{
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL  Px[4];
  UINTN                          Index, Lx, Ly;
  CONST UINTN                    Probe[4][2] = { { 8, 8 }, { 1270, 100 }, { 20, 700 }, { 640, 360 } };
  UINT32                         Want, Have;

  for (Index = 0; Index < 4; Index++) {
    Lx = Probe[Index][0];
    Ly = Probe[Index][1];
    Want = mFb[FbIndex (Lx, Ly)];
    if (EFI_ERROR (mGop->Blt (mGop, &Px[Index], EfiBltVideoToBltBuffer, FbIndex (Lx, Ly) % mFbW,
                              FbIndex (Lx, Ly) / mFbW, 0, 0, 1, 1, 0))) {
      return FALSE;
    }
    Have = (*(UINT32 *)&Px[Index]) & 0x00FFFFFF;
    if (Have != (Want & 0x00FFFFFF)) {
      return TRUE;
    }
  }
  return FALSE;
}

STATIC
VOID
SetStatus (
  IN UINT32        Color,
  IN CONST CHAR16  *Format,
  ...
  )
{
  VA_LIST  Args;

  VA_START (Args, Format);
  UnicodeVSPrint (mStatus, sizeof (mStatus), Format, Args);
  VA_END (Args);
  mStatusColor = Color;
}

// ---------------------------------------------------------------- actions

STATIC
VOID
ClearForText (
  VOID
  )
{
  gST->ConOut->SetAttribute (gST->ConOut, EFI_LIGHTGRAY | EFI_BACKGROUND_BLACK);
  gST->ConOut->ClearScreen (gST->ConOut);
}

STATIC
VOID
BootWindows (
  VOID
  )
{
  EFI_BOOT_MANAGER_LOAD_OPTION  *Options;
  UINTN                         Count, Index, Pick;

  Options = EfiBootManagerGetLoadOptions (&Count, LoadOptionTypeBoot);
  Pick    = Count;
  // 1. "Windows Boot Manager"; 2. the first active boot option that is not the shell.
  for (Index = 0; Index < Count && Pick == Count; Index++) {
    if (((Options[Index].Attributes & LOAD_OPTION_ACTIVE) != 0) &&
        ((Options[Index].Attributes & LOAD_OPTION_CATEGORY) == LOAD_OPTION_CATEGORY_BOOT) &&
        (Options[Index].Description != NULL) &&
        (StrStr (Options[Index].Description, L"Windows Boot Manager") != NULL)) {
      Pick = Index;
    }
  }
  for (Index = 0; Index < Count && Pick == Count; Index++) {
    if (((Options[Index].Attributes & LOAD_OPTION_ACTIVE) != 0) &&
        ((Options[Index].Attributes & LOAD_OPTION_HIDDEN) == 0) &&
        ((Options[Index].Attributes & LOAD_OPTION_CATEGORY) == LOAD_OPTION_CATEGORY_BOOT) &&
        ((Options[Index].Description == NULL) || (StrStr (Options[Index].Description, L"Shell") == NULL))) {
      Pick = Index;
    }
  }
  if (Pick == Count) {
    SetStatus (COLOR_WARN, L"No boot option found.");
    EfiBootManagerFreeLoadOptions (Options, Count);
    return;
  }
  ClearForText ();
  EfiBootManagerBoot (&Options[Pick]);
  SetStatus (COLOR_WARN, L"%s: %r", Options[Pick].Description != NULL ? Options[Pick].Description : L"Boot",
             Options[Pick].Status);
  EfiBootManagerFreeLoadOptions (Options, Count);
}

STATIC
VOID
OpenSettings (
  VOID
  )
{
  EFI_BOOT_MANAGER_LOAD_OPTION  Menu;

  if (EFI_ERROR (EfiBootManagerGetBootManagerMenu (&Menu))) {
    SetStatus (COLOR_WARN, L"UEFI settings (UiApp) not found.");
    return;
  }
  ClearForText ();
  EfiBootManagerBoot (&Menu);
  EfiBootManagerFreeLoadOption (&Menu);
  mStatus[0] = L'\0';
}

// \bootloader\update.bin from any FAT volume (hekate keeps a copy of itself there).
STATIC
VOID *
LoadPayload (
  OUT UINTN  *Size
  )
{
  EFI_HANDLE                       *Handles;
  UINTN                            Count, Index, InfoSize;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *Fs;
  EFI_FILE_PROTOCOL                *Root, *File;
  EFI_FILE_INFO                    *Info;
  VOID                             *Buffer = NULL;
  UINTN                            Read;

  if (EFI_ERROR (gBS->LocateHandleBuffer (ByProtocol, &gEfiSimpleFileSystemProtocolGuid, NULL, &Count, &Handles))) {
    return NULL;
  }
  for (Index = 0; Index < Count && Buffer == NULL; Index++) {
    if (EFI_ERROR (gBS->HandleProtocol (Handles[Index], &gEfiSimpleFileSystemProtocolGuid, (VOID **)&Fs))) {
      continue;
    }
    if (EFI_ERROR (Fs->OpenVolume (Fs, &Root))) {
      continue;
    }
    if (!EFI_ERROR (Root->Open (Root, &File, L"\\bootloader\\update.bin", EFI_FILE_MODE_READ, 0))) {
      InfoSize = SIZE_OF_EFI_FILE_INFO + 512;
      Info     = AllocateZeroPool (InfoSize);
      if ((Info != NULL) && !EFI_ERROR (File->GetInfo (File, &gEfiFileInfoGuid, &InfoSize, Info)) &&
          (Info->FileSize >= PAYLOAD_MIN_SIZE) && (Info->FileSize <= IRAM_PAYLOAD_SIZE)) {
        Read   = (UINTN)Info->FileSize;
        Buffer = AllocateZeroPool ((Read + 3) & ~(UINTN)3);
        if ((Buffer != NULL) && (EFI_ERROR (File->Read (File, &Read, Buffer)) || (Read != Info->FileSize))) {
          FreePool (Buffer);
          Buffer = NULL;
        } else if (Buffer != NULL) {
          *Size = Read;
        }
      }
      if (Info != NULL) {
        FreePool (Info);
      }
      File->Close (File);
    }
    Root->Close (Root);
  }
  FreePool (Handles);
  return Buffer;
}

STATIC
VOID
ReturnToHekate (
  VOID
  )
{
  UINT32  Hidrev;
  UINT8   *Payload;
  UINTN   Size = 0, Offset;

  Hidrev = MmioRead32 (MISC_GP_HIDREV);
  if ((((Hidrev >> 8) & 0xFF) != 0x21) || (((Hidrev >> 4) & 0xF) >= 2)) {
    SetStatus (COLOR_WARN, L"Not an Erista Switch (HIDREV %08x): reboot-to-payload is not possible.", Hidrev);
    return;
  }

  Payload = LoadPayload (&Size);
  if (Payload == NULL) {
#ifdef SWITCH_BOOT_CHAIN_HEKATE
    // switchroot's bl31 turns PSCI SYSTEM_RESET into reboot-to-payload (hekate).
    gRT->ResetSystem (EfiResetCold, EFI_SUCCESS, 0, NULL);
#endif
    SetStatus (COLOR_WARN, L"\\bootloader\\update.bin not found on the SD.");
    return;
  }

  gBS->RaiseTPL (TPL_HIGH_LEVEL);

  // SCLK to its POR default so the PMC APB clock source does not change on reset.
  MmioWrite32 (CAR_BASE + CAR_SCLK_BURST_POLICY, SCLK_BURST_POLICY_POR);
  MmioWrite32 (CAR_BASE + CAR_SCLK_RATE, 0);
  ArmDataSynchronizationBarrier ();
  gBS->Stall (1000);

  for (Offset = 0; Offset < IRAM_PAYLOAD_SIZE; Offset += 4) {
    MmioWrite32 (IRAM_PAYLOAD + Offset, 0);
  }
  for (Offset = 0; Offset < Size; Offset += 4) {
    MmioWrite32 (IRAM_PAYLOAD + Offset, *(UINT32 *)(Payload + Offset));
  }
  // hekate boot_cfg_t at +0x94: zero = show the hekate menu.
  for (Offset = 0; Offset < PAYLOAD_BOOTCFG_SIZE; Offset += 4) {
    MmioWrite32 (IRAM_PAYLOAD + PAYLOAD_BOOTCFG + Offset, 0);
  }
  ArmDataSynchronizationBarrier ();

  MmioOr32 (PMC_BASE + PMC_SCRATCH0, PMC_SCRATCH0_WARMBOOT);
  // BootROM SDRAM init patched to an SVC right after the second write ...
  MmioWrite32 (PMC_BASE + PMC_SCRATCH45, 0x2E38DFFF);
  MmioWrite32 (PMC_BASE + PMC_SCRATCH46, 0x6001DC28);
  // ... and the SVC handler jumps to the payload in IRAM.
  MmioWrite32 (PMC_BASE + PMC_SCRATCH33, (UINT32)IRAM_PAYLOAD);
  MmioWrite32 (PMC_BASE + PMC_SCRATCH40, 0x6000F208);
  ArmDataSynchronizationBarrier ();
  MmioOr32 (PMC_BASE + PMC_CNTRL, PMC_CNTRL_MAIN_RST);
  ArmDataSynchronizationBarrier ();

  gBS->Stall (100000);
  CpuDeadLoop ();
}

STATIC
VOID
PowerOff (
  VOID
  )
{
  UINT8    Cfg2 = 0;
  EFI_TPL  OldTpl;

  // TPL_CALLBACK: no I2C5 user (SidebandButtonDxe, UsbPowerAuditDxe) cuts in.
  OldTpl = gBS->RaiseTPL (TPL_CALLBACK);
  if (i2c_recv_buf_small (&Cfg2, 1, I2C_5, MAX77620_ADDR, MAX77620_ONOFFCNFG2) == 1) {
    i2c_send_byte (I2C_5, MAX77620_ADDR, MAX77620_ONOFFCNFG2, (UINT8)(Cfg2 & ~ONOFFCNFG2_SFT_RST_WK));
  }
  i2c_send_byte (I2C_5, MAX77620_ADDR, MAX77620_ONOFFCNFG1, ONOFFCNFG1_SFT_RST);
  gBS->Stall (300000);
  i2c_send_byte (I2C_5, MAX77620_ADDR, MAX77620_ONOFFCNFG1, ONOFFCNFG1_PWR_OFF);
  gBS->Stall (300000);
  gBS->RestoreTPL (OldTpl);
  SetStatus (COLOR_WARN, L"Power off did not happen (MAX77620 ONOFFCNFG2 was %02x).", Cfg2);
}

STATIC
VOID
Rotate (
  VOID
  )
{
  mRotation ^= 1;
  gRT->SetVariable (L"SwitchMenuRotation", &mMenuVarGuid,
                    EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS, sizeof (mRotation), &mRotation);
}

STATIC
VOID
Activate (
  IN UINTN  Index
  )
{
  mStatus[0] = L'\0';
  switch (Index) {
    case ActBootWindows: BootWindows ();    break;
    case ActHekate:      ReturnToHekate (); break;
    case ActSettings:    OpenSettings ();   break;
    case ActPowerOff:    PowerOff ();       break;
    case ActRotate:      Rotate ();         break;
  }
}

// ---------------------------------------------------------------- input

STATIC
INTN
HitTest (
  IN INTN  Lx,
  IN INTN  Ly
  )
{
  UINTN  Index;

  for (Index = 0; Index < ActCount; Index++) {
    if ((Lx >= (INTN)mButtons[Index].X) && (Lx < (INTN)(mButtons[Index].X + mButtons[Index].W)) &&
        (Ly >= (INTN)mButtons[Index].Y) && (Ly < (INTN)(mButtons[Index].Y + mButtons[Index].H))) {
      return (INTN)Index;
    }
  }
  return -1;
}

STATIC
VOID
FindTouch (
  VOID
  )
{
  EFI_HANDLE  Handle;
  EFI_STATUS  Status;
  struct {
    VENDOR_DEVICE_PATH        Vendor;
    EFI_DEVICE_PATH_PROTOCOL  End;
  } Path;
  EFI_DEVICE_PATH_PROTOCOL  *Dp;

  Path.Vendor.Header.Type    = HARDWARE_DEVICE_PATH;
  Path.Vendor.Header.SubType = HW_VENDOR_DP;
  SetDevicePathNodeLength (&Path.Vendor.Header, sizeof (VENDOR_DEVICE_PATH));
  CopyGuid (&Path.Vendor.Guid, &mTouchDeviceGuid);
  SetDevicePathEndNode (&Path.End);

  Dp     = (EFI_DEVICE_PATH_PROTOCOL *)&Path;
  Status = gBS->LocateDevicePath (&gEfiAbsolutePointerProtocolGuid, &Dp, &Handle);
  if (!EFI_ERROR (Status) && IsDevicePathEnd (Dp)) {
    gBS->HandleProtocol (Handle, &gEfiAbsolutePointerProtocolGuid, (VOID **)&mTouch);
  }
}

// Returns TRUE when the picture needs redrawing.
STATIC
BOOLEAN
PollTouch (
  OUT INTN  *Released
  )
{
  EFI_ABSOLUTE_POINTER_STATE  State;
  BOOLEAN                     Down;
  INTN                        Lx, Ly;

  *Released = -1;
  if ((mTouch == NULL) || EFI_ERROR (mTouch->GetState (mTouch, &State))) {
    return FALSE;
  }
  Down = (State.ActiveButtons & EFI_ABSP_TouchActive) != 0;
  FbToLandscape ((UINTN)State.CurrentX, (UINTN)State.CurrentY, &Lx, &Ly);
  mTouchX = Lx;
  mTouchY = Ly;
  if (Down) {
    if (!mTouching) {
      mPressed = HitTest (Lx, Ly);
      if ((mPressed >= 0) && (mPressed < ActRotate)) {
        mFocus = (UINTN)mPressed;
      }
    } else if ((mPressed >= 0) && (HitTest (Lx, Ly) != mPressed)) {
      mPressed = -1;           // slid off the button: cancel
    }
  } else if (mTouching) {
    *Released = (HitTest (Lx, Ly) == mPressed) ? mPressed : -1;
    mPressed  = -1;
  }
  mTouching = Down;
  return TRUE;
}

// ---------------------------------------------------------------- main

EFI_STATUS
EFIAPI
SwitchMenuMain (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS     Status;
  EFI_EVENT      Timer, Events[2];
  EFI_INPUT_KEY  Key;
  UINTN          Index, Size, Ticks = 0;
  BOOLEAN        Dirty = TRUE;
  INTN           Released;

  Status = gBS->HandleProtocol (gST->ConsoleOutHandle, &gEfiGraphicsOutputProtocolGuid, (VOID **)&mGop);
  if (EFI_ERROR (Status)) {
    Status = gBS->LocateProtocol (&gEfiGraphicsOutputProtocolGuid, NULL, (VOID **)&mGop);
  }
  if (EFI_ERROR (Status)) {
    return Status;
  }
  mFbW      = mGop->Mode->Info->HorizontalResolution;
  mFbH      = mGop->Mode->Info->VerticalResolution;
  mPortrait = mFbH > mFbW;
  if ((mPortrait ? mFbH : mFbW) < LW || (mPortrait ? mFbW : mFbH) < LH) {
    return EFI_UNSUPPORTED;      // the layout needs 1280 x 720
  }
  mFb = AllocateZeroPool (mFbW * mFbH * sizeof (UINT32));
  if (mFb == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Size = sizeof (mRotation);
  if (EFI_ERROR (gRT->GetVariable (L"SwitchMenuRotation", &mMenuVarGuid, NULL, &Size, &mRotation)) || (mRotation > 1)) {
    mRotation = 0;
  }

  FindTouch ();
  gST->ConIn->Reset (gST->ConIn, FALSE);
  gST->ConOut->EnableCursor (gST->ConOut, FALSE);

  gBS->CreateEvent (EVT_TIMER, 0, NULL, NULL, &Timer);
  gBS->SetTimer (Timer, TimerPeriodic, 200000);   // 20 ms
  Events[0] = gST->ConIn->WaitForKey;
  Events[1] = Timer;

  for (;;) {
    if (Dirty) {
      Render ();
      Dirty = FALSE;
    }
    gBS->WaitForEvent (2, Events, &Index);

    if (Index == 0) {
      while (!EFI_ERROR (gST->ConIn->ReadKeyStroke (gST->ConIn, &Key))) {
        if ((Key.ScanCode == SCAN_UP) || (Key.ScanCode == SCAN_LEFT)) {
          mFocus = (mFocus + ActRotate - 1) % ActRotate;
        } else if ((Key.ScanCode == SCAN_DOWN) || (Key.ScanCode == SCAN_RIGHT)) {
          mFocus = (mFocus + 1) % ActRotate;
        } else if ((Key.UnicodeChar == CHAR_CARRIAGE_RETURN) || (Key.UnicodeChar == L' ')) {
          Activate (mFocus);
        } else if (Key.ScanCode == SCAN_ESC) {
          // Leave the menu: BDS goes on with the normal boot order.
          ClearForText ();
          gBS->CloseEvent (Timer);
          FreePool (mFb);
          return EFI_SUCCESS;
        }
        Dirty = TRUE;
      }
      continue;
    }

    // 20 ms tick: touch, and every 0.5 s whether someone drew over the menu.
    if (mTouch == NULL && (Ticks % 50) == 0) {
      FindTouch ();
      Dirty |= (mTouch != NULL);
    }
    if (PollTouch (&Released)) {
      Dirty = TRUE;
      if (Released >= 0) {
        Render ();
        Activate ((UINTN)Released);
      }
    }
    if ((++Ticks % 25) == 0 && !Dirty && Overwritten ()) {
      Dirty = TRUE;
    }
  }
}
