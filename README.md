# KOF98 Native

用 C++ 从零实现的 KOF'98（Neo Geo MVS）原生模拟器：M68000 + Z80 + YM2610 全软件模拟，Win32 窗口 / waveOut 音频，单 exe、无外部 DLL 依赖。

## 编译

### 1. 准备编译器

下载 [zig 0.14.0 (windows-x86_64)](https://ziglang.org/download/) 并解压即可。zig 内置 clang，可直接编译 C++，无需安装 MSVC。

### 2. 编译

在源码根目录执行（PowerShell，把 zig 路径换成你的实际路径）：

```powershell
$z   = "tools\zig-windows-x86_64-0.14.0\zig.exe"
$cpu = "-mcpu=x86_64_v3"   # 重要，见下方说明

# 编译（12 个源文件）
$srcs = "emu","video","cpu_interp","cpu_interp2","main","translated_stub","z80","ym2610","romload"
foreach ($s in $srcs) { & $z c++ -O2 -std=c++17 $cpu -c -o "build\$s.o" "src\$s.cpp" }
foreach ($s in "ymfm_adpcm","ymfm_opn","ymfm_ssg") { & $z c++ -O2 -std=c++17 $cpu -c -o "build\$s.o" "src\ymfm\$s.cpp" }

# 链接
& $z c++ -O2 $cpu -o build\kof98native.exe build\emu.o build\video.o build\cpu_interp.o build\cpu_interp2.o build\main.o build\translated_stub.o build\z80.o build\ym2610.o build\romload.o build\ymfm_adpcm.o build\ymfm_opn.o build\ymfm_ssg.o -lwinmm -lgdi32 -luser32
```

### 关于 `-mcpu`（分发必读）

zig **默认按编译机器的 CPU 特性生成代码**。在支持 AVX-512 的新机器（如 Zen 4/5）上编出的 exe，拷到不支持的老机器上会立即闪退（崩溃码 `0xc000001d` 非法指令）。要分发给别人，必须显式指定基线：

| 选项 | 要求 | 适用 |
|---|---|---|
| `-mcpu=x86_64_v3` | AVX2 | 2013 年后的 CPU（推荐） |
| `-mcpu=x86_64_v2` | SSE4.2 | 2008 年后，更保险 |
| `-mcpu=x86_64` | 无 | 任意 x86-64 |

## ROM 准备

需要 MAME 格式的两个 ROM 集（请自行合法获取，本项目不含 ROM）：

```
roms\kof98.zip
roms\neogeo.zip
```

把 `roms\` 目录放在 exe 旁边即可——启动时程序自动解压、按 Neo Geo 内存布局拼接（含 KOF98 的 68k 程序 ROM 解密），无需任何预处理。

也兼容旧的 `rom\*.bin` 布局（找不到 `roms\*.zip` 时自动回退），bin 可用 `tools\py\prepare_roms.py` 从 zip 生成。

## 运行

```
kof98native.exe
```

首次运行会在 exe 旁生成 `kof98.ini`，可自定义键位。默认键位：

| 功能 | P1 | P2 |
|---|---|---|
| 方向 | 方向键 | I / K / J / L |
| A / B / C / D | Z / X / C / V | 小键盘 1 / 2 / 3 / 4 |
| 开始 | 1 | 2 |
| 选择 | 3 | 4 |
| 投币 | 5 | 6 |

其他：`F2` 测试菜单，`ESC` 退出。`bram.bin` 为电池记忆（存档），自动读写。

调试：设置环境变量 `KOF98_HEADLESS=帧数` 可无窗口运行指定帧数后退出，并打印 CPU 状态，用于回归验证。

## 打包分发

只需三样（约 41 MB）：

```
kof98native.exe
roms\kof98.zip
roms\neogeo.zip
```

## 源码结构

```
src\
  main.cpp          Win32 平台层：窗口、输入、音频输出、帧同步
  emu.cpp           机器状态、内存映射 I/O、ROM 加载、帧调度
  romload.cpp       直接读取 MAME zip（自包含 inflate）+ ROM 拼接 + kof98 解密
  cpu_interp*.cpp   68k 解释器
  translated_stub.cpp
  video.cpp         LSPC 视频：精灵 + fix 层渲染
  z80.cpp           Z80 音频 CPU
  ym2610.cpp        YM2610 封装
  ymfm\             YMFM 音频核心（BSD 许可）
tools\py\           离线工具（prepare_roms.py 等，非必需）
```
