# pnut-os

An operating system for small phones, built on Apache NuttX: the system's
services, the app runtime and the system UI, with apps as WebAssembly
packages. It is written from its design, in
[pnut-labs/design](https://github.com/pnut-labs/design): the RFCs there say
what the system does and why.

The work has just started. The tree builds NuttX's configurations for the
simulator, QEMU and the LilyGo T-Deck Max, and the first pieces of
pnut-os: the service library, `libpnut`, and the system program with its
first services, Board's description of the device, Service states,
Settings and Time, tested on the computer
and in the simulator. On the T-Deck Max, the system program also keeps
the log's warnings and errors, and crash reports, in `/var/lib/log`, and
powers the device off when the battery runs low.

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
| every target | GNU make; kconfig-frontends; Python 3.11 or later (for its TOML reader) with the kconfiglib module. On Debian or Ubuntu: `apt install kconfig-frontends python3-kconfiglib`. ESP-IDF's Python environment has a kconfiglib of its own that cannot read NuttX's Kconfig: when it comes first on the `PATH`, name another Python, as in `make tdeck-max PYTHON=/usr/bin/python3`. genromfs and xxd, for the image's `/etc`. protoc and Python's protobuf module, for the interfaces' code (`apt install protobuf-compiler python3-protobuf`); curl, which fetches nanopb, checked against its pinned checksum (nuttx-apps fetches libsodium, and checks it the same way) |
| `sim` | gcc, zlib's headers |
| `qemu`, `tdeck-max` | Espressif's toolchain `xtensa-esp-elf` 14.2 (GCC 15 does not build the ESP HAL); esptool; Espressif's QEMU, which has the `esp32s3` machine |
| `tdeck-max` | the network while it builds: NuttX downloads the IMU's firmware image, Bosch's `BHI260AP.fw` (BSD-3-Clause), at a pinned commit and checks its SHA-256. It is kept in the NuttX tree until `make clean` or a build for another target removes it. MCUboot (Apache-2.0): CMake (and Ninja, if there) to build it, from MCUboot's and Espressif's sources, which NuttX clones at pinned commits and keeps in its tree until `make clean`; and MCUboot's `imgtool` (`pip install imgtool`), which makes the firmware's image for it. nuttx-apps fetches MCUboot's library, at a newer commit of its own, for the system program's confirm |
| `make test` | cmocka and libsodium (`apt install libcmocka-dev libsodium-dev`), or `CMOCKA_CFLAGS` and `CMOCKA_LIBS` naming cmocka |

## Building

| Command | Does |
|---|---|
| `make <target>` | configures NuttX for the target, and builds it |
| `make <target> run` | builds it, then runs it: `sim` on the computer (`poweroff` leaves), `qemu` in QEMU (Ctrl-A X leaves) |
| `make <target> flash` | builds it, then writes it to the device |
| `make test` | runs the unit tests, on the computer |
| `make gen` | generates the interfaces' code from their `.proto` files into `build/gen` (every build does it when they change) |
| `make <target> test` | builds it, then runs the tests inside it: `sim` runs libpnut's unit tests in the simulator |
| `make style` | checks pnut-os's C code with NuttX's `nxstyle` |
| `make clean` | removes the build |

| Target | Is |
|---|---|
| `sim` | NuttX's simulator, `sim:nsh` |
| `qemu` | the ESP32-S3 in Espressif's QEMU, `esp32s3-devkit:qemu_debug` |
| `tdeck-max` | the LilyGo T-Deck Max, `lilygo-tdeck-max:full`, flashed over USB (`ESPTOOL_PORT`, `/dev/ttyACM0` by default) |

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
4. pnut-os's `/etc` is staged in `build/etc`: `etc/`; the target's
   description, `configs/<target>/board.toml`, checked against
   `pnut.BoardDescription` and compiled into `pnut/board.pb` (RFC 0024);
   and its settings' defaults, `configs/<target>/defaults.toml`, checked
   against the settings' schemas and compiled into `pnut/defaults.pb`
   (RFC 0025); any owner may be given a module's log level there,
   `log.level`.
5. NuttX builds the image. The board's `/etc` gets `build/etc` laid over
   it (`ETC_OVERLAY`, which the Makefile hands every make of NuttX's):
   NxInit, NuttX's first task, reads `etc/init.d/pnut.rc`, which runs the
   board's own `init.rc`, then starts pnut-os's programs (RFC 0006). Built
   by running NuttX's make directly, the image lacks pnut.rc, and NxInit,
   which is configured to read it, starts nothing.

6. A target's `AFTER_BUILD`, if its `target.mk` gives one: the T-Deck
   Max's builds MCUboot (`make -C nuttx bootloader`).

One target is configured at a time; building another configures afresh.

### The T-Deck Max's flash

MCUboot starts the firmware from the first of two slots (RFC 0013).
`make tdeck-max flash` writes MCUboot, and the firmware into the first
slot, confirmed: the image is padded to the slot's 4 MB, which esptool
compresses. The rest of the flash is not written, so the system
partition keeps its files.

| From | Size | What |
|---|---|---|
| `0x0` | 128 KB | MCUboot |
| `0x20000` | 4 MB | the first slot: the firmware that runs |
| `0x420000` | 4 MB | the second slot: an update, or the previous firmware |
| `0x820000` | 256 KB | free: MCUboot swaps the slots by moving their sectors, with no scratch area |
| `0x860000` | 64 KB | the device's data (RFC 0010), not used yet |
| `0x870000` | 2 MB | the system partition, littlefs, at `/var/lib` |
| `0xa70000` | 5.6 MB | free: for the package store (RFC 0009) |

A firmware in the second slot that is marked for a trial is started at
the next start: MCUboot swaps the slots, so the previous firmware is
kept in the second. The system program confirms the new one once every
program in `etc/pnut/programs` that NxInit has has stayed ready for 10 s
(`PNUT_STATES_READY_HOLD`); if the device starts again before that,
MCUboot swaps them back. Until Packages
installs updates, a trial is made by hand, from a build's `nuttx/nuttx.hex`:

```sh
imgtool sign --pad --align 4 -v 0 -s auto -H 32 --pad-header \
  -S 0x400000 -M 1024 nuttx.hex trial.bin
esptool -p /dev/ttyACM0 write-flash 0x420000 trial.bin
```

as `tools/esp32s3/Config.mk` signs, but without `--confirm`; `--pad`
puts the mark for a trial at the end of the slot.

## adb over Wi-Fi

On the T-Deck Max, adbd listens on port 5555 from the start, reached
once Wi-Fi is joined (RFC 0012). A computer gets in only once its key is
let in, on the device:

1. On the computer: `adb connect <device>:5555`. It shows the device
   "unauthorized"; the device keeps the computer's key waiting, and logs
   its fingerprint.
2. On the device's shell: `adbkeys` lists the keys waiting, each with its
   fingerprint. Compare it with the computer's, which `md5sum` prints
   without the colons:
   `cut -d' ' -f1 ~/.android/adbkey.pub | base64 -d | md5sum`.
3. `adbkeys allow <n>` lets that one in. On the computer:
   `adb disconnect <device>:5555`, then `adb connect <device>:5555`.

`adbkeys forget` lets none in any more. Until the system UI asks on the
screen, this is the only way in.

adbd's port is reached from Wi-Fi only: Network (RFC 0015) has NuttX's
packet filter drop connections to it from any other link, mobile data's
included. For now adbd starts at boot; RFC 0012's Developer option, off
after each restart, and the screen's prompt come with the system UI.

## Layout

| Path | What |
|---|---|
| `nuttx/`, `apps/` | submodules: [pnut-labs/nuttx](https://github.com/pnut-labs/nuttx), [pnut-labs/nuttx-apps](https://github.com/pnut-labs/nuttx-apps) |
| `configs/<target>/` | a target: `target.mk` (the board's configuration, how to run or flash it), `fragment.config` (pnut-os's options), `board.toml` (the device's description, RFC 0024), and `defaults.toml` (its settings' defaults, if it changes any) |
| `etc/` | laid over the board's `/etc` in the image (RFC 0010): `init.d/pnut.rc`, NxInit's configuration (RFC 0006); `pnut/programs`, which services each program runs (RFC 0007) |
| `src/` | what NuttX builds into the firmware; `apps/external` links here |
| `src/lib/` | the service library, `libpnut` (RFC 0023): the event loop, timers, workers, modules, fixed pools, logging; services and clients on local sockets; topics, on uORB; telling NxInit a program is ready; typed settings, kept in a struct and up to date, a program's over one connection (RFC 0025); each module's log level, a setting `log.level` of the owner named as it (RFC 0026); time zones, every zone of the tz database by name with its current rule (RFC 0028); the interfaces' generated code |
| `src/system/` | the system program, `pnut_system` (RFC 0007), with Board (RFC 0024), so far the device's description, read from `/etc` and served whole, every unit unprovisioned, its parts and their control lines (a modem's supply and power key), and airplane mode, a setting of Board's, announced on the `airplane_mode` topic (RFC 0030); Service states (RFC 0006: the services' states, which task is which program, and a command typed in a developer's shell told as the shell's), Settings (RFC 0025), which checks who calls, and lets a command typed in the developer's shell do what the system UI may (RFC 0012), and keeps secrets sealed (XChaCha20-Poly1305, libsodium) under a key of its own in `secrets.key` beside the settings' files, read back by the program that runs their owner alone (RFC 0031); Time (RFC 0028): the clock from the sources' readings or the owner's time, the zone from its settings or the network's offset, the last good time kept across a power loss, and the sleep clock's rate, so far the device's from Board's description, announced on the `sleep_rate` topic; and Power, so far its power-off at a low battery, at the voltage Board's description gives, after the other programs have stopped (each finishing in order: the modem powered down), read through the battery's gauge and charger the description names, and handing the kernel the sleep clock's rate (`BOARDIOC_SLEEP_RATE`) |
| `src/commands/` | the shell's commands (RFC 0012): `settings`, which lists, reads, changes and resets settings (`settings list <owner>`, `get <owner> <key>`, `set <owner> <key> <value>`, `reset <owner> [<key>]`); `telephony`, the modem's and the network's state, the SIM's PIN (`telephony pin <PIN>`, `puk <PUK> <PIN>`) and an AT command passed through (`telephony at <command> [<ms>]`); `messages`, the messages store's conversations, a conversation's messages, a message sent, read or deleted (`messages show <conversation>`, `send <transport> <peer> <text>`, `read`, `delete`); `logs`, the log kept in files across restarts, by level and service (`logs [<level>] [<service>]`), and the crash reports (`logs crashes`, `logs crash <name>`); `wifi`, Wi-Fi's state, the networks in reach, a network joined with its passphrase (a known one with its own), left, the known networks and one forgotten (`wifi scan`, `join <name> [<passphrase>]`, `leave`, `known`, `forget <name> [open]`); `adbkeys`, the computers adb lets in, each with its key's fingerprint, and those waiting (`adbkeys allow <n>`, `clear`, `forget`) |
| `src/network/` | the network program, `pnut_network`, with Network (RFC 0015), so far connectivity: the link in use, Wi-Fi when joined with an address and a router, else mobile data, said on the `network` topic (and whether it costs money); NuttX's default route through it, and its name server for name lookups; and the local ports (adb's) kept to the local links (Wi-Fi), by NuttX's packet filter. And Wi-Fi (RFC 0031): the radio up and down with the `wifi.enabled` setting, a scan, a join and an address by DHCP, the link watched, its state on the `wifi` topic (with DHCP's router and name servers); the known networks, up to 16, last used first, kept in Wi-Fi's settings with their passphrases as secrets, and joined on their own while Wi-Fi is on (the one joined last if in reach, else the strongest; one whose joins fail tried less and less often, up to every 8 minutes; one the owner's join failed not until the owner joins it again); the scan, a join, a leave, the known networks and forgetting one through `pnut.Wifi`; the system UI and the developer's shell may call it |
| `src/telephony/` | the telephony program, `pnut_telephony` (RFC 0030), step 1: the modem, its UART and lines from Board's description; powered up, and down in order (`AT+CPOF`, then the rail once it stops answering), also as the program stops; its UART multiplexed with 27.010 from the start, by a multiplexer of its own on the loop; its settings set again at each start; its state, the SIM, registration, operator and signal on the `telephony` topic and from `pnut.Telephony`; the network's time on `network_time`; the SIM's PIN taken from the system UI and the developer's shell; airplane mode as Board announces it; restarts of the modem's own followed, three an hour at most; step 4, mobile data: with the `data` setting on (off by default) and the modem registered, channel 2 dials the default bearer (`ATD*99***1#`, the access point set first if the `apn` setting names one) while the AT channel is held, then carries PPP to NuttX's pppd on a thread of its own, through a raw pseudo-terminal; the link gets a host-only netmask, and Network routes traffic through it when it is the link in use; a failure hangs up and tries again after 30 s, doubling to 10 minutes; its state on the topic (off, connecting, on, failed), with the link's address and name servers for Network |
| `src/data/` | the data program, `pnut_data` (RFC 0007): Messages (RFC 0029), the device's store of text messages, in files appended to; and Storage's first part (RFC 0011): the card in each slot Board's description gives, mounted at `/media/sd<n>` as the program starts, FAT only and never formatted, announced on the `storage` topic; Storage writes nothing on a card |
| `proto/` | the interfaces, their topics and their settings (RFC 0023, RFC 0025): one `.proto` file each, their numbers in `proto/interfaces.md`; pnut-os's own in `proto/pnut/`: its options, and `pnut.Error`, an error answer's detail |
| `src/tests/` | the unit tests, built as programs for the simulator |
| `tests/unit/` | the unit tests, with cmocka |
| `tests/proto/` | interfaces for the tests |
| `tools/` | the build's own tools: `configure-target.sh` configures NuttX for a target; `sim-test.py` runs programs in the simulator; `protoc-gen-pnut`, protoc's plugin for the interfaces' clients, servers, topics, settings and references, run with nanopb's by `generate.sh`; `stage-etc.sh` stages `/etc`, with `board-description`, which compiles a target's `board.toml`, and `settings-defaults`, which compiles its `defaults.toml`; `gen-zones` writes the time zones' table from the tz database, kept in the source |

As the code arrives:

| Path | What |
|---|---|
| `src/<program>/` | the system's other programs and their services |
| `src/apps/<id>/` | built-in apps |
| `proto/` | the interfaces, topics and schemas, as `.proto` files |
| `sdk/` | the SDK for app developers |
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
