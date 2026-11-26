English | [简体中文](README.md)

# PowerMac Emulator

PowerMac Emulator is an experimental frontend for Power Macintosh / PowerPC Mac
emulation. It provides a Qt6 interface, a virtual machine manager, a dedicated
Rhapsody launcher, and Windows x64 release builds.

> This emulator is rebuilt on top of the PowerPC Macintosh emulation engine from
> [DingusPPC](https://github.com/dingusdev/dingusppc). This project is not
> affiliated with the official DingusPPC project.

This repository does not include any Apple ROMs, Mac OS installation media, disk
images, or software/game images. Running the emulator requires ROMs and system
installation media that you have obtained legally.

## Executables

- `PowerMacEmulator.exe`: general-purpose emulator for Mac OS 8 / 9 and normal
  configurations.
- `RhapsodyEmulator.exe`: dedicated Rhapsody launcher using a G3 + IDE profile.
- `PowerMacVMManager.exe`: virtual machine manager; VM settings can choose
  whether to launch `RhapsodyEmulator.exe`.

## Current features

- **Platform**: Windows x64 (Release builds are produced by GitHub Actions)
- **Machines**: Power Macintosh 8500 / 9500 / 8600 / 9600, Beige G3 desktop/tower
- **CPU**: PowerPC 604 / 604e / 750 (G3)
- **Memory**: configurable RAM, dynamic sparse VHD disks
- **Video**:
  - TNT onboard Control video
  - ATI Rage GT / Rage Pro video
  - Native Qt window rendering with scaling, fullscreen, and resolution changes
  - ATI Rage hardware cursor (G3)
- **Storage**:
  - TNT internal/external SCSI (Curio, MESH)
  - G3 IDE (ATA/ATAPI)
  - Dynamic VHD hard disk images
  - CD-ROM images: ISO, BIN/CUE, CCD/IMG, MDS/MDF
  - Second CD-ROM drive
  - Second G3 IDE hard disk (usable for a BootX disk)
  - Floppy images with runtime hot-swap
- **Networking**:
  - MACE Ethernet controller
  - Built-in Rust userspace Slirp (10.0.2.0/24, DHCP, DNS, TCP/UDP NAT)
  - Statically linked; no external libslirp/glib DLLs
- **Audio**:
  - AWACS / Screamer system sound
  - CD-DA music CD playback using a separate cubeb stream
  - Mac OS 8/9 alert and system sounds
- **Input**:
  - Keyboard and mouse
  - Automatic/manual mouse capture
  - Caps Lock latching and status indication
- **Systems**:
  - Mac OS 8.x
  - Mac OS 9.x
  - Rhapsody DR2 (through `RhapsodyEmulator.exe` with a G3 + IDE profile)
- **Interface**:
  - Qt menu bar, toolbar, and status bar
  - Virtual machine manager: create/edit/start VMs
  - **Rhapsody mode** toggle in VM settings
  - Runtime CD swap, pause/resume, restart, and quit
  - Optional ImGui fallback frontend

## Rhapsody mode

Edit a VM in the virtual machine manager and enable
**Use RhapsodyEmulator (G3 / IDE profile)**. When that VM starts, the manager
launches the `RhapsodyEmulator.exe` in the same directory.

`RhapsodyEmulator.exe` automatically applies the following configuration:

- Machine: Beige G3 (`pmg3dt`)
- CPU: PowerPC 750 (G3)
- Hard disk: the VM's selected disk image attached to `Ide0:0`
- CD-ROM: the VM's selected CD image attached to `Ide1:0`
- Log file: `rhapsody.log`

Therefore, the existing "SCSI hard disk/CD" fields in VM settings are
reinterpreted as IDE devices in this mode. Rhapsody mode requires a G3 ROM.

## Known issues / limitations

- **Mac OS X Server 1.0 / Rhapsody DR1 still do not boot completely**
  - Server 1.0 needs an additional BootX disk and the Open Firmware
    `boot-device` / `boot-file` variables; the frontend currently supports only
    one IDE hard disk.
  - Rhapsody DR1 shows a black screen on PM8500/9500 and stops during SCSI
    initialization on PM9600; the DR1 installation CD does not support G3.
  - Full Open Firmware boot-script / `nvramrc` execution is not implemented.
- **G3 MESH/SCSI is not usable yet**
  - `ScsiMesh` duplicate-device and DBDMA `LOAD_QUAD` errors can occur.
  - Use the IDE/ATAPI path on G3; `RhapsodyEmulator.exe` already maps devices to
    IDE.
- **Networking is still being debugged**
  - MACE and the Rust Slirp backend are integrated, but DHCP, browsing, and
    Internet access under Mac OS 9 are not fully verified yet.
- **Audio input / microphone is not implemented**
- **Serial ports, ADB device hot-plug, and some Open Firmware features are
  incomplete**
- **Releases currently target Windows x64 only**
  - Source builds on Linux/macOS may work, but CI does not provide packages for
    those platforms.
- **No ROMs or system images are provided**
  - `roms/`, `vms/`, and `test-isos/` are ignored by Git and are not uploaded.

## Build (Windows x64)

Requirements:

- Visual Studio 2022/2026 with the "Desktop development with C++" workload
- CMake 3.16+
- Rust stable (`cargo` available on `PATH`, used to build `rust/slirp`)
- Qt 6 (Widgets)
- Vendored SDL2 (`thirdparty/SDL2`) and cubeb (`thirdparty/cubeb`)

```bat
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 ^
      -DCMAKE_PREFIX_PATH=I:/Qt/6.12.0/msvc2022_64
cmake --build build --config Release --parallel
```

Outputs:

```text
build\bin\PowerMacEmulator.exe
build\bin\RhapsodyEmulator.exe
build\bin\PowerMacVMManager.exe
build\bin\SDL2.dll
build\bin\Qt6*.dll
build\bin\platforms\...
```

If Qt6 is not found, CMake falls back to the SDL/ImGui frontend; official
Release builds use Qt6.

## Running

1. Start `PowerMacVMManager.exe`.
2. Select or create a VM directory and configure the machine, CPU, RAM, ROM,
   hard disk, and installation CD.
3. For normal Mac OS 8/9, leave **Rhapsody mode** disabled.
4. For Rhapsody DR2, enable **Rhapsody mode** and use a G3 ROM.
5. Select the VM and click **Start**.

You can also launch directly:

```bat
PowerMacEmulator.exe --run --config <powermac.ini> -b <ROM>
RhapsodyEmulator.exe --run --config <powermac.ini> -b <G3-ROM>
```

ROMs, system disks, and CD images must be supplied by the user and cannot be
distributed with this repository.

## GitHub Actions Release

Workflow: `.github/workflows/release.yml`

- Push to `main` / open a PR: build Windows x64 and upload build artifacts.
- Push a `v*` tag: build and create a GitHub Release with
  `PowerMacEmulator-Windows-x64.zip`.
- Manual `workflow_dispatch` is also supported (uploads artifacts only, without
  creating a Release).

The release package contains the general emulator, Rhapsody-specific emulator,
virtual machine manager, Qt runtime, SDL2, and license/readme files.

## License

This project is derived from the DingusPPC engine and is distributed under the
**GNU General Public License v3.0**. The original DingusPPC code and its
contributors' copyright notices remain in the source files. See
[`LICENSE`](LICENSE) for the complete license text.
