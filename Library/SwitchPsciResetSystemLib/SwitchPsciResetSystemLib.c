/** @file
  ResetSystem through PSCI, for the hekate (L4T) boot chain.

  Same as ArmPkg ArmPsciResetSystemLib, plus a visible marker: before the
  PSCI call, the backlight PWM (PWM0, 0x7000A000) is switched off. The
  switchroot bl31 turns a PSCI SYSTEM_RESET into reboot-to-payload (hekate)
  on Erista, so:
    - backlight goes off, hekate comes back   -> everything works
    - backlight goes off, no hekate           -> ResetSystem was called,
                                                 bl31 / BootROM side fails
    - backlight stays on                      -> the OS never called the
                                                 EFI ResetSystem

  The PWM page is registered as runtime MMIO so the marker also works after
  SetVirtualAddressMap (Windows).

  Copyright (c) 2013-2015, ARM Ltd. All rights reserved.<BR>
  Copyright (c) 2014, Linaro Ltd. All rights reserved.<BR>

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <PiDxe.h>

#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include <Library/DxeServicesTableLib.h>
#include <Library/EfiResetSystemLib.h>
#include <Library/ArmSmcLib.h>
#include <Library/IoLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>

#include <Guid/EventGroup.h>
#include <IndustryStandard/ArmStdSmc.h>

#define SWITCH_PWM_BASE   0x7000A000UL
#define SWITCH_PWM_SIZE   0x1000UL
#define SWITCH_PWM_CSR_0  0x00

STATIC UINTN      mPwmBase = SWITCH_PWM_BASE;
STATIC BOOLEAN    mPwmRuntime;
STATIC EFI_EVENT  mVirtualAddressChangeEvent;
STATIC EFI_EVENT  mReadyToBootEvent;

STATIC
VOID
EFIAPI
SwitchResetVirtualAddressChange (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  if (mPwmRuntime) {
    gRT->ConvertPointer (0, (VOID **)&mPwmBase);
  } else {
    // Not mapped for the OS: never touch it after SetVirtualAddressMap.
    mPwmBase = 0;
  }
}

/**
  Register the PWM page as runtime MMIO. Done at ReadyToBoot, when the CPU
  arch protocol is surely there (SetMemorySpaceAttributes needs it).
**/
STATIC
VOID
EFIAPI
SwitchResetReadyToBoot (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  EFI_STATUS                       Status;
  EFI_GCD_MEMORY_SPACE_DESCRIPTOR  Desc;

  gBS->CloseEvent (Event);

  Status = gDS->GetMemorySpaceDescriptor (SWITCH_PWM_BASE, &Desc);
  if (EFI_ERROR (Status)) {
    return;
  }

  if (Desc.GcdMemoryType == EfiGcdMemoryTypeNonExistent) {
    Status = gDS->AddMemorySpace (
                    EfiGcdMemoryTypeMemoryMappedIo,
                    SWITCH_PWM_BASE,
                    SWITCH_PWM_SIZE,
                    EFI_MEMORY_UC | EFI_MEMORY_RUNTIME
                    );
    if (EFI_ERROR (Status)) {
      return;
    }
  } else if (Desc.GcdMemoryType != EfiGcdMemoryTypeMemoryMappedIo) {
    return;
  } else if ((Desc.Capabilities & EFI_MEMORY_RUNTIME) == 0) {
    Status = gDS->SetMemorySpaceCapabilities (
                    SWITCH_PWM_BASE,
                    SWITCH_PWM_SIZE,
                    Desc.Capabilities | EFI_MEMORY_UC | EFI_MEMORY_RUNTIME
                    );
    if (EFI_ERROR (Status)) {
      return;
    }
  }

  Status = gDS->SetMemorySpaceAttributes (
                  SWITCH_PWM_BASE,
                  SWITCH_PWM_SIZE,
                  EFI_MEMORY_UC | EFI_MEMORY_RUNTIME
                  );
  if (!EFI_ERROR (Status)) {
    mPwmRuntime = TRUE;
  }
}

STATIC
VOID
SwitchResetMarker (
  VOID
  )
{
  if (mPwmBase != 0) {
    // Backlight off: duty 0, PWM disabled.
    MmioWrite32 (mPwmBase + SWITCH_PWM_CSR_0, 0);
  }
}

EFI_STATUS
EFIAPI
LibResetSystem (
  IN EFI_RESET_TYPE   ResetType,
  IN EFI_STATUS       ResetStatus,
  IN UINTN            DataSize,
  IN CHAR16           *ResetData OPTIONAL
  )
{
  ARM_SMC_ARGS  ArmSmcArgs;

  switch (ResetType) {
  case EfiResetPlatformSpecific:
  case EfiResetWarm:
  case EfiResetCold:
    ArmSmcArgs.Arg0 = ARM_SMC_ID_PSCI_SYSTEM_RESET;
    break;
  case EfiResetShutdown:
    ArmSmcArgs.Arg0 = ARM_SMC_ID_PSCI_SYSTEM_OFF;
    break;
  default:
    return EFI_UNSUPPORTED;
  }

  SwitchResetMarker ();

  ArmCallSmc (&ArmSmcArgs);

  CpuDeadLoop ();
  return EFI_UNSUPPORTED;
}

EFI_STATUS
EFIAPI
LibInitializeResetSystem (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;

  Status = gBS->CreateEventEx (
                  EVT_NOTIFY_SIGNAL,
                  TPL_NOTIFY,
                  SwitchResetVirtualAddressChange,
                  NULL,
                  &gEfiEventVirtualAddressChangeGuid,
                  &mVirtualAddressChangeEvent
                  );
  ASSERT_EFI_ERROR (Status);

  Status = EfiCreateEventReadyToBootEx (
             TPL_CALLBACK,
             SwitchResetReadyToBoot,
             NULL,
             &mReadyToBootEvent
             );
  ASSERT_EFI_ERROR (Status);

  return EFI_SUCCESS;
}
