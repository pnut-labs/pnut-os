<!-- SPDX-License-Identifier: Apache-2.0 -->

# Interfaces

Every interface has a number, given in its `.proto` file with
`option (pnut.iface)`, and never reused (RFC 0023). This is the list.

| Number | Interface | File |
|---|---|---|
| 1 | `pnut.Settings`, every setting (RFC 0025) | `proto/pnut/settings.proto` |
| 2 | `pnut.Services`, the services' states and which task is which (RFC 0006) | `proto/pnut/services.proto` |
| 65000 | `pnut.test.Echo`, libpnut's tests | `tests/proto/pnut/test/echo.proto` |

Numbers from 65000 are for tests.
