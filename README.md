# PowerMac Emulator

PowerMac Emulator 是一个面向 Power Macintosh / PowerPC Mac 的实验性模拟器前端，
提供 Qt6 图形界面、虚拟机管理器和 Windows x64 发布版。

> 本模拟器基于 [DingusPPC](https://github.com/dingusdev/dingusppc) 的 PowerPC
> Macintosh 模拟引擎二次重建；本项目与 DingusPPC 官方项目无隶属关系。

本仓库不包含任何 Apple ROM、Mac OS 安装介质、磁盘镜像或游戏/软件镜像。运行模拟器
需要用户自行提供合法取得的 ROM 与系统安装介质。

## 当前可以实现的功能

- **平台**：Windows x64（Release 由 GitHub Actions 构建）
- **机型**：Power Macintosh 8500 / 9500 / 8600 / 9600，Beige G3 桌面版/塔式版
- **CPU**：PowerPC 604 / 604e / 750（G3）
- **内存**：可配置内存大小，支持动态 VHD 稀疏磁盘
- **显示**：
  - TNT 机型板载 Control 视频
  - ATI Rage GT / Rage Pro 视频
  - Qt 原生窗口渲染，支持画面缩放、全屏和分辨率切换
  - ATI Rage 硬件光标（G3）
- **存储**：
  - TNT 内置/外置 SCSI（Curio、MESH）
  - G3 IDE（ATA/ATAPI）
  - 动态 VHD 硬盘镜像
  - CD-ROM 镜像：ISO、BIN/CUE、CCD/IMG、MDS/MDF
  - 第二 CD-ROM 光驱
- **声音**：
  - AWACS / Screamer 系统声音
  - CD-DA 音乐 CD 播放（独立 cubeb 音频流）
  - Mac OS 8/9 提示音与系统声音
- **输入**：
  - 键盘、鼠标
  - 自动/手动鼠标捕获
  - Caps Lock 锁存和状态指示
- **系统**：
  - Mac OS 8.x
  - Mac OS 9.x
  - Rhapsody DR2（G3/IDE 路径，视配置而定）
- **界面**：
  - Qt 菜单栏、工具栏、状态栏
  - 虚拟机管理器：新建/编辑/启动 VM
  - 运行时切换 CD、暂停/恢复、重启、退出
  - 可选的 ImGui 备用前端

## 当前不能做到 / 已知问题

- **Mac OS X Server 1.0 / Rhapsody DR1 还不能完整启动**
  - Server 1.0 需要额外的 BootX 磁盘和 Open Firmware
    `boot-device` / `boot-file` 启动变量；当前前端只支持一个 IDE 硬盘。
  - Rhapsody DR1 在 PM8500/9500 上黑屏，PM9600 上会卡在 SCSI 初始化；
    DR1 安装光盘也不支持 G3。
  - 尚未实现完整的 Open Firmware 启动脚本/`nvramrc` 执行。
- **G3 MESH/SCSI 不可用**
  - 会出现 `ScsiMesh` 重复设备或 DBDMA `LOAD_QUAD` 错误。
  - G3 请使用 IDE/ATAPI 路径。
- **网络尚未实现**
  - MACE 控制器有基础代码，但尚未接入 Slirp 或其他用户态网络后端。
- **音频输入/麦克风未实现**
- **串口、ADB 设备热插拔、部分 Open Firmware 功能仍不完整**
- **Release 目前只提供 Windows x64**
  - Linux/macOS 可以尝试源码构建，但 CI 未提供对应发布包。
- **不提供 ROM 或系统镜像**
  - 仓库中的 `roms/`、`vms/`、`test-isos/` 均被忽略，不会上传。

## 构建（Windows x64）

需要：

- Visual Studio 2022/2026，含“使用 C++ 的桌面开发”
- CMake 3.16+
- Qt 6（Widgets）
- 仓库自带的 SDL2（`thirdparty/SDL2`）和 cubeb（`thirdparty/cubeb`）

```bat
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 ^
      -DCMAKE_PREFIX_PATH=I:/Qt/6.12.0/msvc2022_64
cmake --build build --config Release --parallel
```

生成文件：

```text
build\bin\PowerMacEmulator.exe
build\bin\PowerMacVMManager.exe
build\bin\SDL2.dll
build\bin\Qt6*.dll
build\bin\platforms\...
```

如果未找到 Qt6，CMake 会回退到 SDL/ImGui 前端；但正式 Release 使用 Qt6。

## 运行

1. 启动 `PowerMacVMManager.exe`。
2. 选择或新建 VM 目录，设置机型、CPU、内存、ROM、硬盘和安装光盘。
3. 选择 VM 并点击 **Start**。
4. 也可以直接启动 `PowerMacEmulator.exe --run --config <powermac.ini> -b <ROM>`。

ROM、系统盘和 CD 镜像需要用户自行准备，不能随仓库分发。

## GitHub Actions Release

工作流：`.github/workflows/release.yml`

- 推送到 `main` / 创建 PR：构建 Windows x64 并上传构建产物。
- 推送 `v*` 标签：构建并创建 GitHub Release，附带
  `PowerMacEmulator-Windows-x64.zip`。
- 也可以手动触发 `workflow_dispatch`（只上传构建产物，不创建 Release）。

发布包包含模拟器、虚拟机管理器、Qt 运行库、SDL2 和许可证/说明文件。

## 许可证

本项目基于 DingusPPC 引擎二次开发，整体按 **GNU General Public License v3.0**
发布。原始 DingusPPC 代码及其作者版权声明保留在源文件中；完整许可证见
[`LICENSE`](LICENSE)。
