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

  The framebuffer is 720 x 1280 portrait; the menu is drawn (Draw.c) on a
  1280 x 720 landscape canvas and turned into it. "Rotate" turns the picture by
  180 degrees and remembers it in the SwitchMenuRotation variable.
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

#include "Draw.h"

// ---------------------------------------------------------------- constants

#define COLOR_WARN      0x00FFC857

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

// ---------------------------------------------------------------- state

STATIC EFI_GRAPHICS_OUTPUT_PROTOCOL    *mGop;
STATIC EFI_ABSOLUTE_POINTER_PROTOCOL   *mTouch;
STATIC UINT32                          *mFb;          // framebuffer-shaped copy of the screen
STATIC UINT32                          *mCanvas;      // landscape LW x LH
STATIC UINT32                          *mBackground;  // landscape, drawn once
STATIC UINTN                           mFbW, mFbH;
STATIC BOOLEAN                         mPortrait;     // framebuffer taller than wide
STATIC UINT8                           mRotation;     // 0 or 1 (180 degrees)
STATIC MENU_VIEW                       mView;
STATIC CHAR16                          mStatus[128];

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
Render (
  VOID
  )
{
  UINTN  Lx, Ly;

  mView.TouchDriver = (mTouch != NULL);
  mView.Status      = (mStatus[0] != L'\0') ? mStatus : NULL;
  MenuRender (mCanvas, mBackground, &mView);
  for (Ly = 0; Ly < LH; Ly++) {
    for (Lx = 0; Lx < LW; Lx++) {
      mFb[FbIndex (Lx, Ly)] = mCanvas[Ly * LW + Lx];
    }
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
  IN UINT32        Color,     // kept for the call sites; one colour for messages
  IN CONST CHAR16  *Format,
  ...
  )
{
  VA_LIST  Args;

  VA_START (Args, Format);
  UnicodeVSPrint (mStatus, sizeof (mStatus), Format, Args);
  VA_END (Args);
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
  mView.TouchX = Lx;
  mView.TouchY = Ly;
  if (Down) {
    if (!mView.Touching) {
      mView.Pressed = MenuHitTest (Lx, Ly);
      if ((mView.Pressed >= 0) && (mView.Pressed < ActRotate)) {
        mView.Focus = (UINTN)mView.Pressed;
      }
    } else if ((mView.Pressed >= 0) && (MenuHitTest (Lx, Ly) != mView.Pressed)) {
      mView.Pressed = -1;      // slid off the button: cancel
    }
  } else if (mView.Touching) {
    *Released     = (MenuHitTest (Lx, Ly) == mView.Pressed) ? mView.Pressed : -1;
    mView.Pressed = -1;
  }
  mView.Touching = Down;
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
  mFb         = AllocateZeroPool (mFbW * mFbH * sizeof (UINT32));
  mCanvas     = AllocatePool (LW * LH * sizeof (UINT32));
  mBackground = AllocatePool (LW * LH * sizeof (UINT32));
  if ((mFb == NULL) || (mCanvas == NULL)) {
    return EFI_OUT_OF_RESOURCES;
  }
  if (mBackground != NULL) {
    MenuRenderBackground (mBackground);
  }
  mView.Pressed   = -1;
  mView.TouchX    = -1;
  mView.TouchY    = -1;
  mView.ShowTouch = FALSE;    // R423: the touch marker was for checking the mapping (R421H/R422H)
#ifdef SWITCH_BOOT_CHAIN_HEKATE
  mView.Version = L"R422H  -  UEFI  -  hekate chain";
#else
  mView.Version = L"R422  -  UEFI  -  coreboot chain";
#endif

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
          mView.Focus = (mView.Focus + ActRotate - 1) % ActRotate;
        } else if ((Key.ScanCode == SCAN_DOWN) || (Key.ScanCode == SCAN_RIGHT)) {
          mView.Focus = (mView.Focus + 1) % ActRotate;
        } else if ((Key.UnicodeChar == CHAR_CARRIAGE_RETURN) || (Key.UnicodeChar == L' ')) {
          Activate (mView.Focus);
        } else if (Key.ScanCode == SCAN_ESC) {
          // Leave the menu: BDS goes on with the normal boot order.
          ClearForText ();
          gBS->CloseEvent (Timer);
          FreePool (mFb);
          FreePool (mCanvas);
          if (mBackground != NULL) {
            FreePool (mBackground);
          }
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
