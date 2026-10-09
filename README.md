# pnut-os

An operating system for small phones, built on Apache NuttX: the system's
services, the app runtime and the system UI, with apps as WebAssembly
packages. It is written from its design, in
[pnut-labs/design](https://github.com/pnut-labs/design): the RFCs there say
what the system does and why.

The work has just started. The tree builds NuttX's configurations for the
simulator, QEMU and the LilyGo T-Deck Max, with nothing of pnut-os in them
yet.

## Getting the tree

NuttX and nuttx-apps are submodules, pinned to the commits pnut-os is
tested with:

```sh
git clone --recursive https://github.com/pnut-labs/pnut-os.git
```

After a pull that moves them, run `git submodule update`. `make <target>`
warns when they differ from the commits pnut-os records.

## What you need

| For | Tools |
|---|---|
| every target | GNU make; kconfig-frontends; Python 3 with the kconfiglib module. On Debian or Ubuntu: `apt install kconfig-frontends python3-kconfiglib`. ESP-IDF's Python environment has a kconfiglib of its own that cannot read NuttX's Kconfig: when it comes first on the `PATH`, name another Python, as in `make tdeck-max PYTHON=/usr/bin/python3` |
| `sim` | gcc, genromfs, xxd, zlib's headers |
| `qemu`, `tdeck-max` | Espressif's toolchain `xtensa-esp-elf` 14.2 (GCC 15 does not build the ESP HAL); esptool; Espressif's QEMU, which has the `esp32s3` machine |

## Building

| Command | Does |
|---|---|
| `make <target>` | configures NuttX for the target, and builds it |
| `make <target> run` | builds it, then runs it: `sim` on the computer (`poweroff` leaves), `qemu` in QEMU (Ctrl-A X leaves) |
| `make <target> flash` | builds it, then writes it to the device |
| `make style` | checks pnut-os's C code with NuttX's `nxstyle` |
| `make clean` | removes the build |

| Target | Is |
|---|---|
| `sim` | NuttX's simulator, `sim:nsh` |
| `qemu` | the ESP32-S3 in Espressif's QEMU, `esp32s3-devkit:qemu_debug` |
| `tdeck-max` | the LilyGo T-Deck Max, `lilygo-tdeck-max:nsh`, flashed over USB (`ESPTOOL_PORT`, `/dev/ttyACM0` by default) |

The boards live in [pnut-labs/nuttx](https://github.com/pnut-labs/nuttx).

How a build goes:

1. `apps/external` is made a link to `src/`, so nuttx-apps builds pnut-os's
   code with its own.
2. NuttX is configured with the board's configuration that
   `configs/<target>/target.mk` names.
3. `configs/<target>/fragment.config`, pnut-os's options, is merged over it
   with NuttX's `tools/merge_config.py`. Every option in it must reach the
   final configuration: one that is misspelt, or whose dependencies are not
   met, stops the build.
4. NuttX builds the image.

One target is configured at a time; building another configures afresh.

## Layout

| Path | What |
|---|---|
| `nuttx/`, `apps/` | submodules: [pnut-labs/nuttx](https://github.com/pnut-labs/nuttx), [pnut-labs/nuttx-apps](https://github.com/pnut-labs/nuttx-apps) |
| `configs/<target>/` | a target: `target.mk` (the board's configuration, how to run or flash it) and `fragment.config` (pnut-os's options) |
| `src/` | what NuttX builds into the firmware; `apps/external` links here |
| `tools/` | the build's own tools: `configure-target.sh` configures NuttX for a target |

As the code arrives:

| Path | What |
|---|---|
| `src/lib/` | the service library, `libpnut` |
| `src/system/<program>/` | the system's programs and their services |
| `src/apps/<id>/` | built-in apps |
| `proto/` | the interfaces, topics and schemas, as `.proto` files |
| `sdk/` | the SDK for app developers |
| `tests/` | unit and system tests |
| `docs/` | reference documentation |

## Contributing

- **C11 in NuttX's style,** checked by `make style`; every file carries an
  SPDX license line.
- **Commit messages name the part they touch,** as NuttX's do:
  `system/settings: ...`.
- **Changes go through pull requests** to `master`, squashed into one
  commit; the checks must pass.

## License

[Apache 2.0](LICENSE), like NuttX. The design documents are CC BY 4.0, in
their own repository.
