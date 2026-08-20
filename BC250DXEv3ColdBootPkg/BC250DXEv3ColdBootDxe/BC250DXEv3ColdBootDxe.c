/**
  BC-250 DXEv3 cold-boot recovery driver.

  This driver consumes the MeiMeiDXEv3RequestColdBoot volatile variable at
  ReadyToBoot, schedules an RTC wake event a few seconds in the future, and
  then enters ACPI S5 through the BC-250's PM1 registers.  The scheduled wake
  causes the platform to power on again, providing a cold boot rather than a
  firmware reset.  If the platform cannot expose console output or configure
  the RTC, the driver deliberately leaves the machine running instead of
  risking an unverified shutdown.

  Unlike DXEv2, the request variable's payload is an optional NUL-terminated
  CHAR16 message supplied by the requesting driver.  When present it is
  displayed in the recovery countdown box; an empty or malformed payload
  selects the default message.

  The PM1 register addresses and sleep-type value below are specific to the
  BC-250 P3.00 firmware.  They must be revalidated against the target ACPI
  tables before using this module on another firmware revision or platform.
**/

#include <Uefi.h>

#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>

#include <BC250ColdBoot.h>

#define RTC_WAKE_DELAY_SECONDS      3U
#define RECOVERY_COUNTDOWN_SECONDS  10U
#define RECOVERY_BOX_MAX_WIDTH      78U
#define RECOVERY_BOX_HEIGHT         13U

//
// Request message limits.  The payload is a NUL-terminated CHAR16 string;
// messages longer than REQUEST_MESSAGE_MAX_CHARS or without a terminator
// within the cap are rejected and the default recovery text is used instead.
//
#define REQUEST_MESSAGE_MAX_CHARS   160U
#define REQUEST_MESSAGE_CAPACITY    (REQUEST_MESSAGE_MAX_CHARS + 1U)

//
// BC-250 P3.00 ACPI FADT values.  PM1a_EVT_BLK is 0x400 and the event
// registers are two bytes each: status at 0x400 and enable at 0x402.
// RTC_STS and RTC_EN are bit 10 in their respective registers.
//
#define BC250_PM1A_EVT_BLK          0x400U
#define BC250_PM1_STATUS_OFFSET     BC250_PM1A_EVT_BLK
#define BC250_PM1_ENABLE_OFFSET     (BC250_PM1A_EVT_BLK + sizeof (UINT16))
#define BC250_PM1A_CNT_BLK          0x404U
#define BC250_PM1_RTC_BIT           BIT10
#define BC250_PM1_SLP_TYP_MASK      (BIT10 | BIT11 | BIT12)
#define BC250_PM1_SLP_EN            BIT13
#define BC250_PM1_S5_VALUE          (5U << 10)

// The variable is intentionally kept in this module so all Runtime Services
// accesses use the same GUID object and the request namespace is easy to find.
STATIC EFI_GUID  gBc250ColdBootVendorGuid = BC250_COLD_BOOT_VENDOR_GUID;

// Default recovery line shown when no validated custom message was supplied.
STATIC CONST CHAR16  mDefaultRecoveryMessage[] =
  L"Your BC250 needs to reboot to apply some changes.";

/**
  Extract the requesting driver's display message from the variable payload.

  The payload must be an even number of bytes and contain a NUL terminator
  within the first REQUEST_MESSAGE_MAX_CHARS CHAR16 units.  Valid messages are
  copied, including the terminator, into the caller's bounded buffer.  Empty
  or malformed payloads return FALSE so the caller falls back to the default
  recovery text.

  @param[in]  RequestSize  Byte size of the variable payload.
  @param[in]  Payload      The variable payload read from Runtime Services.
  @param[out] Message      Receives at most REQUEST_MESSAGE_CAPACITY CHAR16.

  @retval TRUE   A validated message was copied into Message.
  @retval FALSE  Payload is empty, malformed, or exceeds the message cap.
**/
STATIC
BOOLEAN
GetRequestMessage (
  IN  UINTN   RequestSize,
  IN  VOID    *Payload,
  OUT CHAR16  *Message
  )
{
  UINTN  UnitCount;
  UINTN  Index;

  if (Payload == NULL || RequestSize == 0 ||
      (RequestSize % sizeof (CHAR16)) != 0U) {
    return FALSE;
  }

  UnitCount = RequestSize / sizeof (CHAR16);

  // Locate the NUL terminator within the cap.  A missing terminator or a
  // string longer than the cap is treated as malformed.
  for (Index = 0U; Index < UnitCount && Index < REQUEST_MESSAGE_CAPACITY;
       ++Index) {
    if (((CONST CHAR16 *)Payload)[Index] == L'\0') {
      break;
    }
  }

  if (Index == 0U || Index == UnitCount || Index >= REQUEST_MESSAGE_CAPACITY) {
    return FALSE;
  }

  CopyMem (Message, Payload, (Index + 1U) * sizeof (CHAR16));
  return TRUE;
}

/**
  Return whether a Gregorian calendar year has a February 29th.

  @param[in] Year  Four-digit calendar year from EFI_TIME.

  @retval TRUE   Year is a leap year.
  @retval FALSE  Year is not a leap year.
**/
STATIC
BOOLEAN
IsLeapYear (
  IN UINT16  Year
  )
{
  return (BOOLEAN)(((Year % 4U) == 0U && (Year % 100U) != 0U) || ((Year % 400U) == 0U));
}

/**
  Return the number of days in a calendar month.

  The caller supplies a validated month in the range 1..12.  This helper is
  used only by AddSeconds, where EFI_TIME already represents a valid date.

  @param[in] Year   Calendar year, used to identify leap years.
  @param[in] Month  Calendar month, numbered from 1 (January) to 12.

  @return Number of days in Month for Year.
**/
STATIC
UINT8
DaysInMonth (
  IN UINT16  Year,
  IN UINT8   Month
  )
{
  STATIC CONST UINT8 Days[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
  return (Month == 2U && IsLeapYear (Year)) ? 29U : Days[Month - 1U];
}

/**
  Add a non-negative number of seconds to an EFI_TIME value.

  EFI_TIME stores each field separately, so carries are performed from seconds
  through minutes and hours before the remaining day count is applied.  The
  final loop handles month lengths and year rollover, including leap years.
  Time zones are not interpreted here; the caller normalizes the resulting
  EFI_TIME fields before passing it to Runtime Services.

  @param[in,out] Time     Time value to update.
  @param[in]     Seconds  Number of seconds to add.
**/
STATIC
VOID
AddSeconds (
  IN OUT EFI_TIME  *Time,
  IN     UINT32    Seconds
  )
{
  Time->Second += (UINT8)(Seconds % 60U);
  Seconds = Seconds / 60U + (Time->Second / 60U);
  Time->Second %= 60U;
  Time->Minute += (UINT8)(Seconds % 60U);
  Seconds = Seconds / 60U + (Time->Minute / 60U);
  Time->Minute %= 60U;
  Time->Hour += (UINT8)(Seconds % 24U);
  Seconds = Seconds / 24U + (Time->Hour / 24U);
  Time->Hour %= 24U;
  while (Seconds-- != 0U) {
    if (++Time->Day > DaysInMonth (Time->Year, Time->Month)) {
      Time->Day = 1U;
      if (++Time->Month > 12U) {
        Time->Month = 1U;
        ++Time->Year;
      }
    }
  }
}

/**
  Compare the calendar fields used by the RTC wakeup interface.

  EFI_TIME also contains nanosecond, time-zone, and daylight fields.  Those
  fields are intentionally excluded because firmware may normalize them while
  preserving the requested wake instant.

  @param[in] Left   First wake time.
  @param[in] Right  Second wake time.

  @retval TRUE   Compared fields are identical.
  @retval FALSE  At least one compared field differs.
**/
STATIC
BOOLEAN
SameWakeTime (
  IN CONST EFI_TIME  *Left,
  IN CONST EFI_TIME  *Right
  )
{
  return (BOOLEAN)(Left->Year   == Right->Year   &&
                   Left->Month  == Right->Month  &&
                   Left->Day    == Right->Day    &&
                   Left->Hour   == Right->Hour   &&
                   Left->Minute == Right->Minute &&
                   Left->Second == Right->Second);
}

/**
  Clear a stale RTC PM1 event and enable RTC wake generation.

  PM1 status uses write-one-to-clear semantics.  Only the RTC bit is written,
  so unrelated platform wake sources are not acknowledged.  The enable
  register is read back because some firmware or chipset configurations can
  reject writes to the PM1 block.

  @retval EFI_SUCCESS       RTC wake generation is enabled.
  @retval EFI_DEVICE_ERROR  The RTC enable bit could not be observed set.
**/
STATIC
EFI_STATUS
EnableAcpiRtcWake (
  VOID
  )
{
  UINT16  Status;
  UINT16  Enable;

  // PM1 status bits are write-one-to-clear.  Clear only a stale RTC event;
  // do not disturb unrelated wake/status sources.
  Status = IoRead16 (BC250_PM1_STATUS_OFFSET);
  IoWrite16 (BC250_PM1_STATUS_OFFSET, BC250_PM1_RTC_BIT);

  Enable = IoRead16 (BC250_PM1_ENABLE_OFFSET);
  IoWrite16 (BC250_PM1_ENABLE_OFFSET, (UINT16)(Enable | BC250_PM1_RTC_BIT));
  Enable = IoRead16 (BC250_PM1_ENABLE_OFFSET);

  DEBUG ((DEBUG_INFO,
    "BC250DXEv3ColdBootDxe: PM1 RTC wake status-before=0x%04x enable=0x%04x enable-after=0x%04x\n",
    Status, (UINT16)(Enable & (UINT16)~BC250_PM1_RTC_BIT), Enable));

  if ((Enable & BC250_PM1_RTC_BIT) == 0U) {
    DEBUG ((DEBUG_ERROR,
      "BC250DXEv3ColdBootDxe: PM1 RTC_EN could not be enabled\n"));
    return EFI_DEVICE_ERROR;
  }

  return EFI_SUCCESS;
}

/**
  Enter the BC-250 ACPI S5 soft-off state.

  The platform's EfiResetShutdown implementation does not reliably reach the
  desired ACPI path, so this routine writes PM1a_CNT directly.  Interrupts are
  disabled and RTC_EN is reasserted immediately before the SLP_EN write; this
  prevents any intervening firmware activity from clearing the wake source.
  Successful execution should power the machine off, therefore CpuDeadLoop is
  used as a defensive fallback if the hardware unexpectedly returns.
**/
STATIC
VOID
EnterBc250S5 (
  VOID
  )
{
  UINT16  Control;
  UINT16  SleepControl;

  //
  // EfiResetShutdown on the AMI P3.00 firmware does not reliably use the
  // ACPI S5 path.  The BC-250 FADT exposes PM1a_CNT_BLK at 0x404 and its
  // _S5 package contains SLP_TYP=5.  Reassert RTC_EN immediately before the
  // final write because no firmware code runs between this write and S5.
  //
  Control = IoRead16 (BC250_PM1A_CNT_BLK);
  SleepControl = (UINT16)(Control & (UINT16)~(BC250_PM1_SLP_TYP_MASK | BC250_PM1_SLP_EN));
  SleepControl = (UINT16)(SleepControl | BC250_PM1_S5_VALUE | BC250_PM1_SLP_EN);

  DEBUG ((DEBUG_INFO,
    "BC250DXEv3ColdBootDxe: entering S5 PM1_CNT before=0x%04x write=0x%04x\n",
    Control, SleepControl));

  DisableInterrupts ();
  IoWrite16 (BC250_PM1_ENABLE_OFFSET,
             (UINT16)(IoRead16 (BC250_PM1_ENABLE_OFFSET) | BC250_PM1_RTC_BIT));
  IoWrite16 (BC250_PM1A_CNT_BLK, SleepControl);

  // The platform should power off.  Do not return into DXE if it does not.
  CpuDeadLoop ();
}

/**
  Program and verify the Runtime Services wakeup time and chipset RTC source.

  The requested time is based on the current firmware time, not on a host
  clock.  Readback verification covers both the UEFI wakeup state and the
  calendar fields before the driver enables the corresponding ACPI PM1 bit.
  Any failure clears a partially programmed wakeup request so the machine is
  not shut down without a confirmed way to wake it.

  @param[out] WakeTime  Receives the verified wake time.

  @retval EFI_SUCCESS  Wakeup was programmed, verified, and enabled.
  @return              Failure status from Runtime Services or ACPI setup.
**/
STATIC
EFI_STATUS
ArmRtcWake (
  OUT EFI_TIME  *WakeTime
  )
{
  EFI_STATUS  Status;
  EFI_TIME    Now;
  EFI_TIME    Readback;
  BOOLEAN     Enabled;
  BOOLEAN     Pending;

  Status = gRT->GetTime (&Now, NULL);
  DEBUG ((DEBUG_INFO, "BC250DXEv3ColdBootDxe: GetTime=%r now=%04u-%02u-%02u %02u:%02u:%02u\n",
    Status, Now.Year, Now.Month, Now.Day, Now.Hour, Now.Minute, Now.Second));
  if (EFI_ERROR (Status)) {
    return Status;
  }

  AddSeconds (&Now, RTC_WAKE_DELAY_SECONDS);
  CopyMem (WakeTime, &Now, sizeof (*WakeTime));
  WakeTime->Nanosecond = 0;
  WakeTime->TimeZone = EFI_UNSPECIFIED_TIMEZONE;
  WakeTime->Daylight = 0;

  Status = gRT->SetWakeupTime (TRUE, WakeTime);
  DEBUG ((DEBUG_INFO, "BC250DXEv3ColdBootDxe: SetWakeupTime=%r requested=%04u-%02u-%02u %02u:%02u:%02u\n",
    Status, WakeTime->Year, WakeTime->Month, WakeTime->Day,
    WakeTime->Hour, WakeTime->Minute, WakeTime->Second));
  if (EFI_ERROR (Status)) {
    return Status;
  }

  ZeroMem (&Readback, sizeof (Readback));
  Status = gRT->GetWakeupTime (&Enabled, &Pending, &Readback);
  DEBUG ((DEBUG_INFO, "BC250DXEv3ColdBootDxe: GetWakeupTime=%r enabled=%u pending=%u readback=%04u-%02u-%02u %02u:%02u:%02u\n",
    Status, Enabled, Pending, Readback.Year, Readback.Month, Readback.Day,
    Readback.Hour, Readback.Minute, Readback.Second));
  if (EFI_ERROR (Status) || !Enabled || !SameWakeTime (&Readback, WakeTime)) {
    if (!EFI_ERROR (Status)) {
      Status = EFI_DEVICE_ERROR;
    }
    gRT->SetWakeupTime (FALSE, NULL);
    return Status;
  }

  Status = EnableAcpiRtcWake ();
  if (EFI_ERROR (Status)) {
    gRT->SetWakeupTime (FALSE, NULL);
    return Status;
  }

  return EFI_SUCCESS;
}

/**
  Draw the horizontal border of the recovery message box.

  @param[in] Column  Left-most console column.
  @param[in] Row     Console row for the border.
  @param[in] Width   Total border width, including both corner characters.
**/
STATIC
VOID
DrawRecoveryBorder (
  IN UINTN  Column,
  IN UINTN  Row,
  IN UINTN  Width
  )
{
  CHAR16  Border[RECOVERY_BOX_MAX_WIDTH + 1U];
  UINTN   Index;

  Border[0] = L'+';
  for (Index = 1U; Index < Width - 1U; ++Index) {
    Border[Index] = L'-';
  }
  Border[Width - 1U] = L'+';
  Border[Width] = L'\0';
  gST->ConOut->SetCursorPosition (gST->ConOut, Column, Row);
  Print (L"%s", Border);
}

/**
  Draw one centered, padded line inside the recovery message box.

  Text longer than the box interior is clipped to keep the local buffer and
  the console row bounded by RECOVERY_BOX_MAX_WIDTH.

  @param[in] Column  Left-most console column.
  @param[in] Row     Console row for this line.
  @param[in] Width   Total line width, including both side borders.
  @param[in] Text    NUL-terminated text to center in the line.
**/
STATIC
VOID
DrawRecoveryLine (
  IN UINTN         Column,
  IN UINTN         Row,
  IN UINTN         Width,
  IN CONST CHAR16  *Text
  )
{
  CHAR16  Line[RECOVERY_BOX_MAX_WIDTH + 1U];
  UINTN   Interior;
  UINTN   TextLength;
  UINTN   LeftPadding;
  UINTN   Index;

  Interior = Width - 2U;
  TextLength = StrLen (Text);
  if (TextLength > Interior) {
    TextLength = Interior;
  }
  LeftPadding = (Interior - TextLength) / 2U;

  Line[0] = L'|';
  for (Index = 1U; Index <= Interior; ++Index) {
    Line[Index] = L' ';
  }
  CopyMem (&Line[1U + LeftPadding], Text, TextLength * sizeof (CHAR16));
  Line[Width - 1U] = L'|';
  Line[Width] = L'\0';
  gST->ConOut->SetCursorPosition (gST->ConOut, Column, Row);
  Print (L"%s", Line);
}

/**
  Calculate the available console layout and render the recovery box.

  The box is centered and bounded to work on both ordinary text consoles and
  wide firmware consoles.  The countdown line's coordinates are returned to
  the caller so it can be redrawn without rebuilding the entire screen.

  @param[out] CountdownColumn  Column containing the countdown line.
  @param[out] CountdownRow     Row containing the countdown line.
  @param[out] BoxWidth         Width to pass to subsequent line redraws.
  @param[in]  Message          Requesting driver's message, or the default
                               text when no valid request message was given.

  @retval TRUE   Console mode was usable and the box was drawn.
  @retval FALSE  Console services were unavailable or the mode was too small.
**/
STATIC
BOOLEAN
DrawRecoveryBox (
  OUT UINTN         *CountdownColumn,
  OUT UINTN         *CountdownRow,
  OUT UINTN         *BoxWidth,
  IN  CONST CHAR16  *Message
  )
{
  EFI_STATUS  Status;
  UINTN       Columns;
  UINTN       Rows;
  UINTN       Width;
  UINTN       Column;
  UINTN       TopRow;

  if (gST->ConOut == NULL || gST->ConOut->Mode == NULL) {
    return FALSE;
  }

  Status = gST->ConOut->QueryMode (gST->ConOut, gST->ConOut->Mode->Mode, &Columns, &Rows);
  if (EFI_ERROR (Status) || Columns < 60U || Rows < RECOVERY_BOX_HEIGHT) {
    return FALSE;
  }

  Width = (Columns - 2U < RECOVERY_BOX_MAX_WIDTH) ? Columns - 2U : RECOVERY_BOX_MAX_WIDTH;
  Column = (Columns - Width) / 2U;
  TopRow = (Rows - RECOVERY_BOX_HEIGHT) / 2U;

  gST->ConOut->ClearScreen (gST->ConOut);
  DrawRecoveryBorder (Column, TopRow, Width);
  DrawRecoveryLine (Column, TopRow + 1U, Width, L"");
  DrawRecoveryLine (Column, TopRow + 2U, Width, L"MeiMeiDXE BC250 Cold-Boot");
  DrawRecoveryLine (Column, TopRow + 3U, Width, L"");
  DrawRecoveryLine (Column, TopRow + 4U, Width, Message);
  DrawRecoveryLine (Column, TopRow + 5U, Width, L"");
  DrawRecoveryLine (Column, TopRow + 6U, Width, L"We will attempt to restart your BC250 automatically.");
  DrawRecoveryLine (Column, TopRow + 7U, Width, L"If an automatic restart is not possible, please restart your BC250");
  DrawRecoveryLine (Column, TopRow + 8U, Width, L"after it powers down.");
  DrawRecoveryLine (Column, TopRow + 9U, Width, L"");

  *CountdownColumn = Column;
  *CountdownRow = TopRow + 10U;
  *BoxWidth = Width;
  DrawRecoveryLine (Column, *CountdownRow, Width, L"Shutting down in 10 seconds...");
  DrawRecoveryLine (Column, TopRow + 11U, Width, L"");
  DrawRecoveryBorder (Column, TopRow + 12U, Width);
  return TRUE;
}

/**
  Display the user-facing recovery countdown.

  A graphical-looking box is preferred when the current text mode is large
  enough.  The plain-text fallback keeps the recovery path usable on minimal
  consoles and includes the request message when one was supplied.  The delay
  is intentionally completed before RTC programming so the user has time to
  see why the machine is shutting down.

  @param[in] Message  Requesting driver's message, or the default text.
**/
STATIC
VOID
ShowRecoveryCountdown (
  IN CONST CHAR16  *Message
  )
{
  UINTN   Remaining;
  UINTN   CountdownColumn;
  UINTN   CountdownRow;
  UINTN   BoxWidth;
  CHAR16  Countdown[RECOVERY_BOX_MAX_WIDTH + 1U];

  BoxWidth = 0U;
  if (!DrawRecoveryBox (&CountdownColumn, &CountdownRow, &BoxWidth, Message)) {
    Print (L"\n\n  BC250 cold-boot recovery requested.\n");
    Print (L"  %s\n", Message);
    Print (L"  RTC wake will be scheduled before shutdown.\n\n");
  }
  for (Remaining = RECOVERY_COUNTDOWN_SECONDS; Remaining != 0U; --Remaining) {
    if (BoxWidth != 0U) {
      UnicodeSPrint (Countdown, sizeof (Countdown), L"Shutting down in %u seconds...", Remaining);
      DrawRecoveryLine (CountdownColumn, CountdownRow, BoxWidth, Countdown);
    } else {
      Print (L"  Shutting down in %u seconds...\n", Remaining);
    }
    gBS->Stall (1000000U);
  }
}

/**
  Handle the ReadyToBoot notification for a requested cold boot.

  The event is closed first because this is a one-shot request.  The marker is
  then read, consumed, and decoded to produce the display message.  Missing or
  malformed requests are ignored; console absence and RTC setup failure are
  also non-fatal and return control to the normal boot flow.  After the
  countdown, the successful path remains silent while it prepares S5; only an
  RTC setup error is printed to the console.

  @param[in] Event    ReadyToBoot event being serviced.
  @param[in] Context  Unused event context.
**/
STATIC
VOID
EFIAPI
ColdBootReadyToBoot (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  EFI_STATUS  Status;
  UINTN       RequestSize;
  VOID        *Payload;
  CHAR16      Message[REQUEST_MESSAGE_CAPACITY];
  BOOLEAN     HasMessage;
  EFI_TIME    WakeTime;

  gBS->CloseEvent (Event);

  // Phase 1: does a request exist?  GetVariable reports the payload size by
  // returning EFI_BUFFER_TOO_SMALL when the output buffer is omitted.
  RequestSize = 0U;
  Status = gRT->GetVariable (BC250_COLD_BOOT_VARIABLE_NAME,
                             &gBc250ColdBootVendorGuid, NULL,
                             &RequestSize, NULL);
  if (Status != EFI_BUFFER_TOO_SMALL) {
    return;
  }

  // Phase 2: read the payload before consuming the marker.
  Payload = NULL;
  if (RequestSize != 0U) {
    Payload = AllocatePool (RequestSize);
    if (Payload != NULL) {
      Status = gRT->GetVariable (BC250_COLD_BOOT_VARIABLE_NAME,
                                 &gBc250ColdBootVendorGuid, NULL,
                                 &RequestSize, Payload);
      if (EFI_ERROR (Status)) {
        FreePool (Payload);
        Payload = NULL;
      }
    }
  }

  // Phase 3: consume the request so a later boot does not repeat shutdown.
  Status = gRT->SetVariable (BC250_COLD_BOOT_VARIABLE_NAME,
                             &gBc250ColdBootVendorGuid, 0, 0, NULL);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "BC250DXEv3ColdBootDxe: failed to consume request: %r\n", Status));
  }

  HasMessage = GetRequestMessage (RequestSize, Payload, Message);
  if (Payload != NULL) {
    FreePool (Payload);
  }
  if (!HasMessage) {
    CopyMem (Message, mDefaultRecoveryMessage, sizeof (mDefaultRecoveryMessage));
  }

  if (gST->ConOut == NULL) {
    DEBUG ((DEBUG_WARN, "BC250DXEv3ColdBootDxe: no ConOut at ReadyToBoot; continuing boot\n"));
    return;
  }

  ShowRecoveryCountdown (Message);
  Status = ArmRtcWake (&WakeTime);
  if (EFI_ERROR (Status)) {
    Print (L"\n  RTC wake setup failed (%r); continuing boot without shutdown.\n", Status);
    gBS->Stall (3000000U);
    return;
  }

  gBS->Stall (1000000U);
  EnterBc250S5 ();
}

/**
  Register the one-shot callback that services the cold-boot request.

  The callback is attached to the standard ReadyToBoot event group so all
  boot-time services needed by the recovery flow are available while DXE is
  still active.

  @param[in] ImageHandle  Driver image handle.
  @param[in] SystemTable  UEFI system table supplied by the dispatcher.

  @retval EFI_SUCCESS  Event registration completed.
  @return              Status returned by CreateEventEx.
**/
EFI_STATUS
EFIAPI
BC250DXEv3ColdBootEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_EVENT  Event;

  return gBS->CreateEventEx (EVT_NOTIFY_SIGNAL, TPL_CALLBACK,
                             ColdBootReadyToBoot, NULL,
                             &gEfiEventReadyToBootGuid, &Event);
}