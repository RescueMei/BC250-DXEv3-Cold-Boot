# BC250-DXEv3-Cold-Boot

Standalone late-loading DXE driver for the BC-250 that automates a cold boot when requested by another driver, and displays a custom text message supplied by the requesting driver.

Example BIOS using it: https://github.com/RescueMei/BC250-DXEv3-BIOSMOD

NOTE: Auto Cold Boot is only functional on BC250s that are constantly powered or are wired for native ATX control, the BC250 needs standby power to be awoken! BC250s that do not meet this requirement are automatically powered off, but require a manual restart.

## What changed from DXEv2

DXEv2 triggered a cold boot through a volatile variable marker and displayed a hardcoded message. DXEv3 keeps the exact same variable-based trigger mechanism and runtime behavior, but the request variable's **payload now carries the message text** that other drivers want displayed during the cold-boot countdown. Why a new project? I changed the vendor GUID for everything in v3 to unify them, and I didn't want to break v2.

## Behavior

The driver waits for `ReadyToBoot` before starting to ensure it can write to the display. Once started it checks the volatile UEFI variable `MeiMeiDXEv3RequestColdBoot` using the shared MeiMeiDXEv3 vendor GUID. If the marker is not present, it does nothing and normal boot continues.

When the marker is present, the driver:

1. Consumes the variable (one-shot — it is deleted so a later boot does not repeat the shutdown).
2. Decodes the payload as a NUL-terminated `CHAR16` message. A valid message is displayed in the recovery box; an empty, malformed, or oversized payload selects the default message.
3. Displays a ten-second countdown.
4. Programs an RTC wake alarm three seconds in the future.
5. Enables the ACPI PM1 RTC wake event.
6. Enters ACPI S5 through the BC-250 PM1 control block.

The scheduled wake powers the platform back on, providing a cold boot rather than a firmware reset.

If RTC setup fails, it reports the error and continues boot rather than shutting down without a verified wake path. If no console is available, the shutdown is skipped and normal boot continues.

## Integration with Other Drivers

Other drivers request a cold boot by creating the volatile variable below. The payload is an optional NUL-terminated `CHAR16` message displayed in the recovery countdown box. An empty or malformed payload selects the driver's default message.

- **Name:** `MeiMeiDXEv3RequestColdBoot`
- **Vendor GUID:** `49CC168D-E8B0-4613-A807-16969986726F`
- **Attributes:** `EFI_VARIABLE_BOOTSERVICE_ACCESS`
- **Payload:** NUL-terminated `CHAR16` string, up to 160 characters (plus terminator)

The vendor GUID and variable name are defined for consumers in `BC250DXEv3ColdBootPkg/Include/BC250ColdBoot.h` as `BC250_COLD_BOOT_VENDOR_GUID` and `BC250_COLD_BOOT_VARIABLE_NAME`.

### Example (custom message)

```c
#include <BC250ColdBoot.h>

EFI_GUID gBc250ColdBootVendorGuid = BC250_COLD_BOOT_VENDOR_GUID;
CHAR16   msg[] = L"Applying platform changes; reboot required.";

gRT->SetVariable(BC250_COLD_BOOT_VARIABLE_NAME, &gBc250ColdBootVendorGuid,
                 EFI_VARIABLE_BOOTSERVICE_ACCESS,
                 (UINTN)((StrLen(msg) + 1) * sizeof(CHAR16)), msg);
```

### Example (default message)

To request a cold boot with the driver's default message, write an empty
`CHAR16` string as the payload:

```c
CHAR16 empty[] = L"";

gRT->SetVariable(BC250_COLD_BOOT_VARIABLE_NAME, &gBc250ColdBootVendorGuid,
                 EFI_VARIABLE_BOOTSERVICE_ACCESS,
                 (UINTN)sizeof(empty), empty);
```

Note: do not use `DataSize = 0` with `SetVariable`, as that **deletes** the
variable instead of creating the request.

## Variable lifetime and one-shot semantics

The marker is deliberately volatile and only exists for the current boot session. It is cleared by reset or power loss. The Cold Boot driver consumes (deletes) it before the shutdown sequence, making the request one-shot.

## Message format

- The payload must be an even number of bytes (`CHAR16` units).
- The string must contain a NUL terminator within the first 160 `CHAR16` units.
- Messages longer than 160 characters, unterminated strings, odd-sized payloads, or empty strings are rejected and the default recovery text is displayed.
- The displayed message is centered and clipped to the recovery box interior (maximum 78 columns).

## Build

Container build (Podman is the only container engine I tested):

```bash
./build_ffs.sh
```

Host build with an existing EDK II workspace and built BaseTools:

```bash
NO_CONTAINER=1 EDK2_DIR=/path/to/edk2 ./build_ffs.sh
```

Artifacts are written to `Build/Output/`:

- `MeiMeiDXEv3_ColdBoot.efi`
- `MeiMeiDXEv3_ColdBoot.ffs`

The `.ffs` can be inserted into an AMI DXE firmware volume with UEFITool.

## Dispatch note

This DXE module must be the last to execute, or it will miss the request to reboot from a driver that executes afterwards. The module depends only on `gEfiVariableArchProtocolGuid`, so it dispatches early; the signal is serviced at `ReadyToBoot`.