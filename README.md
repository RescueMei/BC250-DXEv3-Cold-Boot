# BC250-DXEv3-Cold-Boot

Late-loading DXE driver for the BC-250 that performs a scheduled cold boot when
another driver asks for one, and shows a caller-supplied message during the
countdown. It exists because the core and CU learning paths need a clean
power-cycle state that only a real power cycle provides.

> **Power requirement:** the scheduled wake needs standby power. On a BC-250
> without native ATX control the platform powers off and must be started
> manually.

## At a glance

| Item | Value |
| --- | --- |
| Package / module | `BC250DXEv3ColdBootPkg` -> `MeiMeiDXEv3_ColdBoot.efi` |
| Depex | `gEfiVariableArchProtocolGuid` |
| Produces | no protocol (variable contract) |
| Consumes | `MeiMeiDXEv3RequestColdBoot` volatile variable |
| Configuration | none (one-shot request variable) |
| Compliance | [COMPLIANCE.md](COMPLIANCE.md) |

## Dependencies

Only variable services are required for dispatch. The driver registers a
`ReadyToBoot` callback, so it sees requests written by any earlier DXE driver.
It does not need to be last in dispatch order.

## How it works

At `ReadyToBoot` the driver checks the volatile variable
`MeiMeiDXEv3RequestColdBoot`. If it is absent, nothing happens. If present:

1. The variable is consumed (deleted), so a later boot cannot repeat it.
2. The payload is decoded as a NUL-terminated `CHAR16` message. Empty, odd-size,
   unterminated, or over-long payloads select the default text.
3. A ten-second countdown is displayed in the recovery box.
4. An RTC wake alarm is programmed three seconds ahead.
5. The ACPI PM1 RTC wake event is enabled.
6. The platform enters ACPI S5 through the BC-250 PM1 block.

The scheduled wake powers the platform back on, which reloads platform firmware and
gives a genuine cold boot rather than a warm reset.

Fail-closed behavior: if the RTC or wake path cannot be programmed, or no
console is available, the driver reports the problem and continues the normal
boot instead of shutting down without a verified wake path.

## Integration with other drivers

Create the variable below. The payload is an optional NUL-terminated `CHAR16`
message; an empty or malformed payload selects the default message.

| Item | Value |
| --- | --- |
| Name | `MeiMeiDXEv3RequestColdBoot` |
| Vendor GUID | `BC250_COLD_BOOT_VENDOR_GUID` (shared config GUID) |
| Attributes | `EFI_VARIABLE_BOOTSERVICE_ACCESS` |
| Payload | NUL-terminated `CHAR16`, up to 160 characters plus terminator |

The name and GUID are defined in `<BC250ColdBoot.h>`. That header is
provider-owned; consumer copies are checked against it at build time.

```c
#include <BC250ColdBoot.h>

EFI_GUID gBc250ColdBootVendorGuid = BC250_COLD_BOOT_VENDOR_GUID;
CHAR16   Msg[] = L"Applying platform changes; reboot required.";

gRT->SetVariable (BC250_COLD_BOOT_VARIABLE_NAME, &gBc250ColdBootVendorGuid,
                  EFI_VARIABLE_BOOTSERVICE_ACCESS,
                  (UINTN)((StrLen (Msg) + 1) * sizeof (CHAR16)), Msg);
```

For the default message, write an empty string. Do not pass `DataSize = 0`:
that deletes the variable instead of creating a request.

The marker is volatile and one-shot. It is consumed before the shutdown
sequence, so it never survives the power cycle.

Message rules: even byte count, NUL within 160 `CHAR16` units, displayed text
centered and clipped to the 78-column recovery box.

## Configuration

None. The only input is the volatile request variable described above; there is
no persistent configuration record.

## Protocols

Also publishes `gMeiMeiDXEv3DriverInfoProtocolGuid` with its name, version, module GUID, config schema, and payload version for the `BC250-DXEv3-Driver-Info` telemetry driver.

None produced. The request is a documented variable contract; there is no
protocol lookup.

## Build

```bash
./build_ffs.sh                                  # container build
NO_CONTAINER=1 EDK2_DIR=/path/to/edk2 ./build_ffs.sh   # host build
```

Output:

- `Build/Output/MeiMeiDXEv3_ColdBoot.efi`
- `Build/Output/MeiMeiDXEv3_ColdBoot.ffs`

## Validation

2026-11-24, eight-driver stack: the driver was present in the validated build
and did not interfere with the boot or reboot paths. See
[COMPLIANCE.md](COMPLIANCE.md).
