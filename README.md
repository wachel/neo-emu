# KOF98 Native

用 C++ 从零实现的 KOF'98（Neo Geo MVS）原生模拟器：M68000 + Z80 + YM2610 全软件模拟。

- **可玩版**：Win32 窗口 / waveOut 音频，单 exe、无外部 DLL 依赖（Windows）
- **RL 训练接口**：纯 C 动态库（Windows / Linux / macOS，含 Apple Silicon），Python 直接调用

## 快速开始

### 1. 准备 ROM（必需）

需要 MAME 格式的两个 ROM 集（请自行合法获取，本项目不含 ROM）：

```
roms/kof98.zip
roms/neogeo.zip
```

启动时自动解压、按 Neo Geo 内存布局拼接（含 KOF98 的 68k 程序 ROM 解密），无需任何预处理。

### 2. 一键构建

**Windows**（需要 [zig 0.14.0](https://ziglang.org/download/)，解压到 `tools\` 或加入 PATH）：

```powershell
powershell -ExecutionPolicy Bypass -File build.ps1
```

产物：`build\kof98native.exe`（可玩版）+ `build\lib\kof98.dll`（RL 接口）

**Linux / macOS**（用系统自带编译器即可，无需 zig）：

```sh
sh build.sh
```

产物：`build/lib/libkof98.so`（Linux）或 `libkof98.dylib`（macOS）

> 老的 x86-64 机器：`build.ps1` 默认 `-mcpu=x86_64_v3`（AVX2，2013 年后的 CPU）。
> 遇到 `0xc000001d` 非法指令崩溃时，设 `$env:KOF98_CPU="x86_64_v2"` 再编。
> 分发给别人的机器情况不明时同理——**不要用本机默认 CPU 特性编译**。

## 可玩版（带界面）

```
build\kof98native.exe      # 把 roms\ 放在 exe 旁边，双击即玩
```

默认键位：

| 功能 | P1 | P2 |
|---|---|---|
| 方向 | 方向键 | I / K / J / L |
| A / B / C / D | Z / X / C / V | 小键盘 1 / 2 / 3 / 4 |
| 开始 | 1 | 2 |
| 选择 | 3 | 4 |
| 投币 | 5 | 6 |

`F2` 测试菜单，`ESC` 退出。`bram.bin` 为电池记忆，自动读写。

> 界面版目前只有 Windows。Linux/macOS 上可以用下方的 Python 查看器（pygame）代替。

## RL 训练接口（跨平台）

动态库导出纯 C 接口（`src/kof98_api.h`）：步进、输入、读内存观测、save-state、帧缓冲。
Python 封装在 `tools/py/kof98env.py`：

```python
from kof98env import Kof98, A, RIGHT

env = Kof98("roms")              # 默认最快配置（无视频/轻音频/音乐引擎关闭）
env.set_input(coin=1); env.run(10)
env.set_input(start=1); env.run(600)
FIGHT_START = env.save()         # 215KB 快照，episode 重置 ~5µs

env.load(FIGHT_START)            # 每局开始
env.set_input(p1=A | RIGHT)
env.run(1)
x = env.wram16(0x1234)           # 读 WRAM 观测（地址按 KOF98 内存表）
```

性能（Ryzen 9 9950X）：无视频 ~6000 fps；开视频渲染 ~150 fps。
多实例：每进程一个 `Kof98`（或 `Kof98VecEnv` 状态交换单进程多 env），多核用 `multiprocessing`。

### 查看训练效果（带界面跑 agent）

```sh
pip install pygame numpy
python tools/py/play_agent.py              # 键盘试玩，有声音（方向键+ZXCV，1 开始，5 投币）
python tools/py/play_agent.py --fast       # 不限速
python tools/py/play_agent.py --mute       # 无声（更快）
```

把你的策略填进 `play_agent.py` 的 `agent_input()`（每帧返回按键位），即可观看 agent 实战。
快照回放：`--snap 快照文件`。

### 测试

```sh
python tools/py/test_api.py     # 接口 + save/load 确定性验证
```

## headless 调试（无窗口）

```
set KOF98_HEADLESS=帧数 && kof98native.exe     # 跑指定帧数后退出并打印状态/帧率
```

速度优化开关（RL 默认全开，玩的时候不要开）：`KOF98_SKIP_VIDEO` / `KOF98_LITE_AUDIO` / `KOF98_NO_ZINT`。

## 源码结构

```
src/
  main.cpp          Win32 平台层：窗口、输入、音频输出（可玩版）
  kof98_api.cpp/h   纯 C RL 接口（跨平台，无平台依赖）
  emu.cpp           机器状态、内存映射 I/O、帧调度、save-state
  romload.cpp       直接读取 MAME zip（自包含 inflate）+ ROM 拼接 + kof98 解密
  cpu_interp*.cpp   68k 解释器（含 vblank 自旋快进）
  video.cpp         LSPC 视频：精灵 + fix 层渲染
  z80.cpp           Z80 音频 CPU（含空闲循环跳过/整调用 park）
  ym2610.cpp        YM2610 封装（支持轻量音频模式）
  ymfm/             YMFM 音频核心（BSD 许可）
tools/py/           Python 工具：kof98env.py（RL 封装）、play_agent.py（查看器）、test_api.py
build.ps1           Windows 一键构建（exe + dll）
build.sh            Linux/macOS 一键构建（RL 动态库）
```
