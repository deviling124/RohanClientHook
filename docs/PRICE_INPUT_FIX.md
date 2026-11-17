# Kiosk single-digit price fix — 2026-11-15

Reported again: 1 displays as 14, 2 as 24, and 0 as 4, while 10 and larger
values appear correct. The earlier correction was delivered as a separate
patched executable; it was not included in the normal novaclient DLL build.

## Cause and integration

The reference ANSI edit-text getter at 004B5A30 calls the wide-text converter
at 004BED80. That converter passes an explicit source length to
WideCharToMultiByte and does not append a NUL. Numeric callers such as 00571290
parse an uninitialized stack buffer using _atoi64 without terminating it first.
A stale ASCII `4` in the next byte changes `1` into `14` and `0` into `04`.
A second digit overwrites that byte, explaining why the reported larger prices
can look correct. This is a buffer-content problem, not a price/tax calculation.

`src/PriceInput.cpp` now installs the correction in novaclient's existing
startup transaction. It reserves one byte of the getter's 1024-byte output,
calls the original converter, and terminates at the returned byte count.
Missing controls and failed conversions return empty text. No digit is stripped
from a valid value: deliberately entered 14 and 24 remain 14 and 24.

The getter is shared by other native edit controls. The hook retains its
thiscall/RET 4 ABI and the native wide-to-ANSI conversion, including its codepage.
The build verifies both code sites before installing. It also recognizes the
byte-exact earlier `.kpfix` executable and skips installing a second fix there.
Unrecognized getter/converter changes are rejected during startup validation.

## Verification and deployment

`tests/test-price-input.cmd` passes 139 native checks without launching the
game. Tests reproduce the stale-byte defect using Windows conversion calls,
invoke the actual replacement through the native calling convention, check all
single digits and representative multi-digit prices, UTF-8 byte counts,
capacity boundaries, missing controls, conversion failures, reference guards,
and compatibility with the prior executable patch.

Use the newly rebuilt `bin/Autoload/novaclient.dll` (identical to the Release
copy). Close the client completely, replace the deployed DLL, and reopen it.
The existing autoload executable works with this DLL; no server-side price
adjustment or injector is needed. Test a fresh kiosk listing after restarting.
The change does not rewrite prices already stored in existing listings.
Full in-game verification of the new DLL remains a live test.

Native analysis exports from the earlier investigation are retained under
`docs/evidence/price-input`. Their recorded original paths name the sibling
server workspace; the supplied client reference has the same SHA-256.
