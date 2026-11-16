/** @file
  realme GT5 (RMX3823) power and volume keys as a SimpleTextIn device.

  Ported from Platforms/Xiaomi/shengPkg/Drivers/ShengKeyInputDxe (Xiaomi Pad 6S
  Pro, also SM8550). The register facts live in RmxSpmiKeys.h; everything here is
  the console glue, unchanged in behaviour, plus diagnostics.

  Why this driver instead of ButtonsDxe:
    The vendor chain (EnvDxe/SmemDxe/ButtonsDxe) needs SMEM, and SMEM is empty in
    the fastboot-boot path (measured: 0x81D00000 2MB all zero, magic != "SIII").
    This driver talks to the SPMI PMIC arbiter directly, so it needs no SMEM and
    no vendor driver at all.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/DevicePathLib.h>
#include <Library/UefiLib.h>
#include <Protocol/DevicePath.h>
#include <Protocol/SimpleTextIn.h>
#include <Guid/EventGroup.h>
#include <Guid/GlobalVariable.h>

#include "RmxSpmiKeys.h"

#define POLL_MS         20
#define REPEAT_DELAY    25     // polls (500 ms) before a held volume key repeats
#define REPEAT_PERIOD   7      // polls (140 ms) between repeats
#define QUEUE_SIZE      16
#define MAX_FAILURES    50     // consecutive failed samples before polling stops

//
// SM8550 SPMI arbiter: CORE at 0x0C400000, observer channels at 0x0C400000+0x40000.
// Same SoC as the Xiaomi port, so the same two constants apply.
//
#define SPMI_CORE_BASE    0x0C400000UL
#define SPMI_OBSRVR_BASE  0x0C440000UL

typedef struct {
  VENDOR_DEVICE_PATH          Vendor;
  EFI_DEVICE_PATH_PROTOCOL    End;
} RMX_KEY_DEVICE_PATH;

STATIC RMX_KEY_DEVICE_PATH  mPath = {
  {
    { HARDWARE_DEVICE_PATH, HW_VENDOR_DP, { sizeof (VENDOR_DEVICE_PATH), 0 } },
    { 0x9B2F41C6, 0x7A38, 0x4E15, { 0xB3, 0x62, 0x5D, 0x18, 0xE4, 0x77, 0x31, 0xA9 } }
  },
  { END_DEVICE_PATH_TYPE, END_ENTIRE_DEVICE_PATH_SUBTYPE, { sizeof (EFI_DEVICE_PATH_PROTOCOL), 0 } }
};

STATIC struct rk_arbiter  mArbiter;
STATIC struct rk_keys     mKeys;
STATIC struct rk_filter   mFilter;
STATIC EFI_INPUT_KEY      mQueue[QUEUE_SIZE];
STATIC UINTN              mHead, mCount;
STATIC UINTN              mFailures;
STATIC UINT8              mLastPressed;
STATIC EFI_EVENT          mTimer, mExit;
STATIC EFI_HANDLE         mHandle;

//
// Print the arbiter layout and its APID map. This is the measurement that tells
// us whether RMX3823's PMIC topolgy matches the Xiaomi port's assumption:
// PON must appear as a PPID with PID 0x13 (any SID), the volume-up GPIO as a
// PPID whose PID is 0x88 + N.
//
STATIC
VOID
DumpArbiter (
  VOID
  )
{
  UINT32  Version;
  UINT32  Features;
  UINT32  Apids;
  UINT32  I;

  Version  = mArbiter.core[0];
  Features = mArbiter.core[RK_FEATURES / 4];
  Apids    = Features & RK_FEATURES_APIDS_MASK;

  DEBUG ((DEBUG_ERROR, "RMXKEY arb ver=0x%08x feat=0x%08x apids=%u\n", Version, Features, Apids));

  if ((Version < RK_VERSION_V7_MIN) || (Version >= RK_VERSION_V8_MIN)) {
    DEBUG ((DEBUG_ERROR, "RMXKEY arb version out of the known range (0x70000000..0x7fffffff)\n"));
    return;
  }

  if ((Apids == 0) || (Apids > 1024)) {
    DEBUG ((DEBUG_ERROR, "RMXKEY arb APID count implausible, map not walked\n"));
    return;
  }

  for (I = 0; I < Apids && I < 48; I++) {
    UINT32  Map  = mArbiter.core[(RK_APID_MAP + 4 * I) / 4];
    UINT32  Ppid = (Map >> 8) & 0xfff;

    if (Map != 0) {
      DEBUG ((
        DEBUG_ERROR,
        "RMXKEY apid[%u] ppid=0x%03x sid=0x%x pid=0x%02x\n",
        I,
        Ppid,
        Ppid >> 8,
        Ppid & 0xff
        ));
    }
  }
}

STATIC
EFI_STATUS
EFIAPI
KeyReset (
  IN EFI_SIMPLE_TEXT_INPUT_PROTOCOL  *This,
  IN BOOLEAN                         ExtendedVerification
  )
{
  EFI_TPL  Tpl = gBS->RaiseTPL (TPL_NOTIFY);

  mHead  = 0;
  mCount = 0;
  gBS->RestoreTPL (Tpl);
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
KeyRead (
  IN  EFI_SIMPLE_TEXT_INPUT_PROTOCOL  *This,
  OUT EFI_INPUT_KEY                   *Key
  )
{
  EFI_TPL  Tpl = gBS->RaiseTPL (TPL_NOTIFY);

  if (mCount == 0) {
    gBS->RestoreTPL (Tpl);
    return EFI_NOT_READY;
  }

  *Key  = mQueue[mHead];
  mHead = (mHead + 1) % QUEUE_SIZE;
  mCount--;
  gBS->RestoreTPL (Tpl);
  return EFI_SUCCESS;
}

STATIC EFI_SIMPLE_TEXT_INPUT_PROTOCOL  mInput = { KeyReset, KeyRead, NULL };

STATIC
VOID
EFIAPI
KeyWait (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  if (mCount != 0) {
    gBS->SignalEvent (Event);
  }
}

STATIC
VOID
Push (
  IN UINT16  Scan,
  IN CHAR16  Unicode
  )
{
  if (mCount == QUEUE_SIZE) {
    return;
  }

  mQueue[(mHead + mCount) % QUEUE_SIZE].ScanCode    = Scan;
  mQueue[(mHead + mCount) % QUEUE_SIZE].UnicodeChar = Unicode;
  mCount++;
  gBS->SignalEvent (mInput.WaitForKey);
}

STATIC
VOID
EFIAPI
Poll (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  UINT8  Pressed;
  UINT8  Out;

  if (rk_sample (&mArbiter, &mKeys, &Pressed) != 0) {
    if (++mFailures >= MAX_FAILURES) {
      gBS->SetTimer (mTimer, TimerCancel, 0);
      DEBUG ((DEBUG_ERROR, "RMXKEY too many failed samples, polling stopped\n"));
    }

    return;
  }

  mFailures = 0;

  if (Pressed != mLastPressed) {
    DEBUG ((DEBUG_ERROR, "RMXKEY held=0x%02x (1=power 2=voldown 4=volup)\n", Pressed));
    mLastPressed = Pressed;
  }

  Out = rk_step (&mFilter, Pressed, REPEAT_DELAY, REPEAT_PERIOD);
  if (Out & RK_KEY_VOLUP) {
    Push (SCAN_UP, 0);
  }

  if (Out & RK_KEY_VOLDOWN) {
    Push (SCAN_DOWN, 0);
  }

  if (Out & RK_KEY_POWER) {
    Push (SCAN_NULL, CHAR_CARRIAGE_RETURN);
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
  gBS->SetTimer (mTimer, TimerCancel, 0);
}

//
// ConPlatformDxe only splits the input devices named in the ConIn variable, so
// this device has to be listed there or the boot manager never sees its keys.
//
STATIC
EFI_STATUS
AddToConIn (
  VOID
  )
{
  EFI_STATUS  Status;
  UINTN       OldSize, OurSize, NewSize;
  UINT32      Attributes;
  UINT8       *Old, *New;

  OurSize = GetDevicePathSize ((EFI_DEVICE_PATH_PROTOCOL *)&mPath);
  OldSize = 0;
  Status  = gRT->GetVariable (EFI_CON_IN_VARIABLE_NAME, &gEfiGlobalVariableGuid, &Attributes, &OldSize, NULL);
  if (Status == EFI_NOT_FOUND) {
    return gRT->SetVariable (EFI_CON_IN_VARIABLE_NAME, &gEfiGlobalVariableGuid, EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS, OurSize, &mPath);
  }

  if (Status != EFI_BUFFER_TOO_SMALL) {
    return Status;
  }

  Old = AllocatePool (OldSize + OurSize);
  if (Old == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Status = gRT->GetVariable (EFI_CON_IN_VARIABLE_NAME, &gEfiGlobalVariableGuid, &Attributes, &OldSize, Old);
  if (!EFI_ERROR (Status)) {
    EFI_DEVICE_PATH_PROTOCOL  *Instance;
    UINTN                     Offset;

    for (Offset = 0; Offset < OldSize;) {
      Instance = (EFI_DEVICE_PATH_PROTOCOL *)(Old + Offset);
      if (CompareMem (Instance, &mPath, sizeof (VENDOR_DEVICE_PATH)) == 0) {
        FreePool (Old);
        return EFI_SUCCESS;
      }

      Offset += GetDevicePathSize (Instance);
      if (Offset >= OldSize) {
        break;
      }
    }

    New     = Old;
    NewSize = OldSize - sizeof (EFI_DEVICE_PATH_PROTOCOL);
    CopyMem (New + NewSize, &mPath, OurSize);
    NewSize += OurSize;
    Status   = gRT->SetVariable (EFI_CON_IN_VARIABLE_NAME, &gEfiGlobalVariableGuid, Attributes, NewSize, New);
  }

  FreePool (Old);
  return Status;
}

EFI_STATUS
EFIAPI
RmxKeyInputDxeEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;
  int         Rc;

  mArbiter.core = (volatile UINT32 *)SPMI_CORE_BASE;
  mArbiter.obs  = (volatile UINT32 *)SPMI_OBSRVR_BASE;
  mArbiter.ee   = 0;

  DumpArbiter ();

  Rc = rk_open (&mArbiter, &mKeys);
  DEBUG ((DEBUG_ERROR, "RMXKEY open rc=%d pon=%d volup=%d\n", Rc, mKeys.pon, mKeys.gpio));

  if (Rc != 0) {
    DEBUG ((DEBUG_ERROR, "RMXKEY arbiter lookup failed, no keys\n"));
    return EFI_UNSUPPORTED;
  }

  Status = gBS->CreateEvent (EVT_NOTIFY_WAIT, TPL_NOTIFY, KeyWait, NULL, &mInput.WaitForKey);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "RMXKEY WaitForKey event %r\n", Status));
    return Status;
  }

  Status = gBS->CreateEvent (EVT_TIMER | EVT_NOTIFY_SIGNAL, TPL_CALLBACK, Poll, NULL, &mTimer);
  if (!EFI_ERROR (Status)) {
    Status = gBS->CreateEventEx (EVT_NOTIFY_SIGNAL, TPL_CALLBACK, OnExitBootServices, NULL, &gEfiEventExitBootServicesGuid, &mExit);
  }

  if (!EFI_ERROR (Status)) {
    Status = gBS->InstallMultipleProtocolInterfaces (
                    &mHandle,
                    &gEfiDevicePathProtocolGuid,
                    &mPath,
                    &gEfiSimpleTextInProtocolGuid,
                    &mInput,
                    NULL
                    );
  }

  if (!EFI_ERROR (Status)) {
    Status = gBS->SetTimer (mTimer, TimerPeriodic, EFI_TIMER_PERIOD_MILLISECONDS (POLL_MS));
  }

  DEBUG ((DEBUG_ERROR, "RMXKEY install %r\n", Status));

  if (!EFI_ERROR (Status)) {
    EFI_STATUS  Listed = AddToConIn ();

    DEBUG ((DEBUG_ERROR, "RMXKEY ConIn %r\n", Listed));
    gBS->ConnectController (mHandle, NULL, NULL, TRUE);
  }

  return Status;
}
