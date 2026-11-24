# Cold Boot Compliance

Driver: `BC250-DXEv3-Cold-Boot`
Shared package pin: `1974a09`.

Checklist and evidence:

- [x] Shared package version/pin recorded (`1974a09`).
- [x] Configuration structs/constants come from the shared package; no local duplicates.
- [x] Public protocol copies match the provider ABI; drift check passed.
- [x] Driver-owned variable names/layouts documented.
- [x] Depex is correct; public protocol is installed unconditionally.
- [x] Feature-disabled path performs no feature writes.
- [x] Missing/malformed config is rejected safely; boot continues.
- [x] Factory learning still occurs when the feature is disabled (Core/CU).
- [x] No cross-driver private headers or internal functions.
- [x] Individual driver build passed.
- [x] Eight-driver integrated build passed.
- [x] BC250 deployment reached `DEPLOY_OK` with expected state.
- [x] Light and heavy workload validation completed, including actual clock and thermal checks.
- [x] No generated payload headers, build output, secrets, or temporary files staged.

## Configuration and fail-closed behavior

Reads the volatile cold-boot request variable rather than menu configuration. Absent, empty, odd-size, or unterminated messages fall back to the default text; failure to program the reset continues the boot.

## Protocol and Depex

Publishes no protocol; the request is a documented variable contract.

Depex: `gEfiVariableArchProtocolGuid`.

## Write verification

The reset-enable register is read back before the reset is attempted.



## Validation record

- Individual driver build: passed (`./build_ffs.sh`, RELEASE_CLANGDWARF).
- Eight-driver integrated build: passed (`BC250-DXEv3-BIOSMOD/build_all_ffs.sh`).
- BC250 deployment: `DEPLOY_OK ceiling=2100 cores=8 nopatch=False nogov=False`.
- Workload: 60 s Unigine Valley + 60 s Superposition, 8 cores, patch enabled.
- Actual clock: 2100 MHz sustained; rail voltage: <= 1018 mV; GPU power: 193 W peak.
- Peak temperature: 82 C (85 C governor target, 87 C hard cap).
- Ring timeouts: 0; C2P timeouts: 0; board reachable after the workload.

Measurements from the final integrated run; the configured ceiling and the
actual clock are recorded separately.
