# BLE 墨水屏图片推送（BLE Central）

> nRF52840 + Zephyr (NCS) 实现的 **BLE 中央设备（Central）**，通过 Nordic UART Service (NUS) 把 **三色墨水屏（128×296，黑 / 白 / 红）** 的图片数据无线推送到屏端，并触发刷屏。
>
> 固件侧对图片点阵做 **QuickLZ 压缩 + CRC16 校验**，按自定义应用层协议分包，利用协商后的大 MTU（247）单次写完大帧。

---

## 目录

- [1. 项目简介与功能概述](#1-项目简介与功能概述)
- [2. 工作原理](#2-工作原理)
- [3. 环境依赖与安装](#3-环境依赖与安装)
- [4. 目录结构](#4-目录结构)
- [5. 构建与烧录](#5-构建与烧录)
- [6. 图片生成流程](#6-图片生成流程)
- [7. 通信协议说明](#7-通信协议说明)
- [8. 基本使用示例](#8-基本使用示例)
- [9. 贡献指南](#9-贡献指南)
- [10. 许可证](#10-许可证)

---

## 1. 项目简介与功能概述

`ble-eink-price-tag-pusher` 是一个运行在 **nRF52840** 开发板上的嵌入式固件，配合 PC 端图片预处理工具，实现把一张普通图片转换、压缩后，通过 **BLE（低功耗蓝牙）** 推送到 **三色墨水屏** 并显示出来。

核心能力：

- **BLE Central + NUS**：作为中央设备扫描、连接屏端，基于 Nordic UART Service 收发数据。
- **大 MTU 传输**：协商 ATT MTU = 247，使单个 242 字节数据帧可一次写完（NCS 2.5.0 下通过 `ACL_RX/TX_SIZE` + `L2CAP_TX_MTU` 修复协商死锁）。
- **三色墨水屏（黑 / 白 / 红）**：图片分黑层与红层分别取模、压缩、发送。
- **QuickLZ 压缩**：PC 端把点阵压缩（约 3.8×），固件端按 **块边界切包** 解压，避免切断压缩块头。
- **CRC16-ARC 校验**：每帧带 CRC16（poly=0xA001, init=0xAAAA, refin=true, 大端），保证传输可靠。
- **NUS 流粘包重组**：对通知通道下发的字节流按"长度字段"切帧，处理分包 / 粘包。

> 本项目为个人嵌入式学习项目，目标是练习「BLE Central + 自定义应用层协议 + 压缩与校验链路」的工程化实现，并把每一层都做到可调试、可验证。

---

## 2. 工作原理

```
┌──────────┐   BLE / NUS    ┌──────────────┐
│ nRF52840  │  ───────────► │ 三色墨水屏     │
│ Central   │   图片点阵压缩 │ （128×296）    │
│ (本固件)   │   分包 + CRC   │               │
└──────────┘                └──────────────┘
      ▲
      │ 图片来源
┌─────┴──────┐
│ PC 工具链   │  image_to_lcd.js → QuickLZ 压缩 → compressed_image.h
│ (Node/CLI) │
└────────────┘
```

固件上电后进入 **扫描 → 连接 → MTU 协商 → 发现 NUS → 订阅通知 → 会话开始 → 文件信息 → 数据分包 → 会话结束** 的有限状态机。整个流程由屏端主动请求、本端应答并推送数据。

---

## 3. 环境依赖与安装

### 3.1 固件（设备端）

| 依赖 | 版本 / 说明 |
|---|---|
| **nRF Connect SDK (NCS)** | v2.5.0（基于 Zephyr RTOS） |
| **工具链** | NCS 自带 arm-none-eabi GCC |
| **调试器** | J-Link（nRF52840 DK 板载或外置） |
| **硬件板** | `nrf52840dk_nrf52840`（标准 nRF52840 DK） |
| **Python** | 3.x（用于部分 PC 端脚本，可选） |

NCS 安装与 `west` 环境搭建请参考 Nordic 官方文档，并确认 `ZEPHYR_BASE` 已设置。

### 3.2 PC 端图片工具

| 依赖 | 说明 |
|---|---|
| **Node.js** | 运行 `tools/image_to_lcd/image_to_lcd.js` |
| **jimp** | 图片解码 / 缩放依赖，已在 `tools/image_to_lcd/package.json` 中声明 |

安装图片工具依赖：

```bash
cd tools/image_to_lcd
npm install        # 安装 jimp
```

---

## 4. 目录结构

```
ble-eink-price-tag-pusher/
├── CMakeLists.txt            # Zephyr 工程定义（target_sources: src/main.c）
├── prj.conf                  # 内核 / BT 配置（Central、NUS、大 MTU 等）
├── monitor.ps1               # Windows 串口监听脚本（捕获固件日志，COM6/115200）
├── src/
│   └── main.c                # 固件主逻辑（BLE 状态机、应用层协议、QuickLZ 解压）
│                             # 点阵数据头（image_data.h、compressed_*.h）由第 6 节
│                             # 工具链本地生成，属构建中间产物，已在 .gitignore 中排除
├── tools/
│   ├── image_to_lcd/         # 图片 → LCD 点阵工具（Node/JS）
│   │   ├── image_to_lcd.js   #   主程序（取模、转置、极性对齐）
│   │   └── package.json
│   ├── qlz_tool/             # QuickLZ 压缩 / 反转工具（PC 端生成 compressed_*.h）
│   │   ├── *.exe             #   压缩 / 反转可执行文件
│   │   └── qlz_rev_tool.c    #   反转层工具源码
│   ├── decompress_render.js  # 解压还原预览
│   ├── verify_layers.js      # 图层校验
│   └── *.py                  # Python 辅助脚本（压缩解压校验等）
└── build/                    # 构建产物（不入库，见 .gitignore）
```

---

## 5. 构建与烧录

在**应用根目录**（即包含 `CMakeLists.txt` 的目录）执行：

```bash
# 1) 构建（板型 nrf52840dk_nrf52840）
west build -b nrf52840dk_nrf52840 -d build

# 2) 烧录到 nRF52840 DK（需 J-Link）
west flash
```

> `CMakeLists.txt` 未硬编码 `BOARD`，构建时通过 `-b <board>` 指定。若使用非 DK 板载目标，请将 `nrf52840dk_nrf52840` 替换为实际板型，并确认 `prj.conf` 中的 BLE / 缓冲配置与之匹配。

烧录后固件自动开始扫描；用串口（默认 115200）即可观察日志（见 `monitor.ps1`）。

---

## 6. 图片生成流程

把一张图片变成固件可推送的数据，分两步：

### 6.1 取模（图片 → 点阵 `.h`）

```bash
cd tools/image_to_lcd
node image_to_lcd.js <你的图片.png> \
    --mode=black \          # 黑层（默认）；红层用 --mode=red
    --lcd-w=128 --lcd-h=296 \
    --content-w=296 --content-h=128 \   # 内容按横屏处理，输出竖屏点阵
    --threshold=60 \        # 黑阈值（RGB 三通道均小于它判黑）
    --out=../../src/image_data.h \
    --preview=preview.png    # 可选：生成点阵还原图用于核对
```

要点（来自工具自身约定）：

- 屏物理方向 **128×296（竖屏）**；图片先缩到 296×128 再**转置**。
- 极性：固件为「**0 = 有墨**」，与常见「黑 = 1」相反，工具默认 `--polarity=0` 已对齐。
- 红层用 `--mode=red --red-threshold=120`（R 高且 G / B 低判红）。

### 6.2 压缩（点阵 → `compressed_image.h`）

用 `tools/qlz_tool/` 下的 QuickLZ 压缩工具，把 `image_data.h` 中的点阵数据压缩，生成 `src/compressed_image.h`（固件直接 `#include` 并在运行时解压）。

```bash
# 具体参数以该工具自身帮助为准（见 tools/qlz_tool/ 下可执行文件）
tools/qlz_tool/qlz_tool_v2.exe <输入点阵> <输出 compressed_image.h>
```

> 压缩产物 `compressed_*.h` 属构建中间产物，已加入 `.gitignore`，不随仓库发布；本地生成后即可编译。

---

## 7. 通信协议说明

### 7.1 NUS UUID

- **写通道（Central → 屏端）**：`49535343-8841-43f4-a8d4-ecbe34729bb3`
- **通知通道（屏端 → Central）**：`49535343-1e4d-4bd9-ba61-23c647249616`

> 以上两个 UUID 来自 **Nordic 官方 UART Service (NUS) 定义**，属公开标准。

### 7.2 应用层帧格式

```
[ 长度 2B 大端 ][ 包序 1B ][ 命令 1B ][ 数据 NB ][ CRC16 2B 大端 ]
```

- **CRC16-ARC**：poly=0xA001，init=0xAAAA，refin=true，校验值**高字节在前（大端）**。
- 大 MTU（247）下，数据帧 payload 可达 **242 字节**，单包一次写完。
- 固定帧头设计（长度 + 包序 + 命令）使接收端可以仅凭长度字段完成粘包切分，不依赖命令字。

### 7.3 应用层协议设计

本端与屏端之间的应用层协议围绕「一次完整图片传输」的生命周期设计，命令分为**下行（Central → 屏端）**与**上行（屏端 → Central）**两组：

**下行命令（4 类）**

| 类别 | 作用 |
|---|---|
| **会话开始** | 唤醒确认、屏幕动作参数（如文件更新 / 刷屏通知） |
| **文件信息** | 描述本次传输：文件大小、文件类型、分包数量、自动刷新与周期刷新参数、屏幕显示模式 |
| **数据分片** | 携带包位置与图片点阵数据；**按 QuickLZ 块边界切包**，避免压缩块被截断 |
| **会话结束** | 传输完成确认，携带结束码 |

**上行命令（4 类）**

由屏端主动发起、驱动整个流程推进，Central 端逐一应答：

```
状态上报 → 请求文件信息 → 请求指定数据包 → 传输完成通知
```

**流水线（状态机）**

```
扫描 → 连接 → MTU 协商(247) → 发现 NUS → 订阅通知
     → 唤醒 → 会话参数 → 文件信息 → 数据分片 × N → 传输完成 → 结束确认
```

- 屏端主动推进；本端（Central）应答并推送数据。
- 大帧异步写期间，用**发送保护标志**避免其他会话命令的回复抢占通道，导致大帧被截断。

---

## 8. 基本使用示例

**1. 准备图片**（见第 6 节），确保 `src/compressed_image.h` 已生成。

**2. 构建并烧录**（见第 5 节）。

**3. 监听日志**（Windows PowerShell）：

```powershell
# monitor.ps1 会定时尝试打开 COM6/115200 并落盘日志
.\monitor.ps1
```

或直接用串口工具（115200 8N1）观察类似输出：

```
=== BLE E-ink Central (QuickLZ compressed bitmap) ===
Connected! Exchanging MTU...
MTU exchange OK, ATT MTU = 247
Session opened. Waiting for device status...
Device reports status. Sending session parameters...
Device asks file info. Replying file info...
Device asks data packet. Sending data...
Transfer complete! FileID=...
```

**4. 给屏端上电 / 复位**，它会广播并被本端连接，完成图片推送与刷屏。

---

## 9. 贡献指南

欢迎 Issue / Pull Request。本地开发约定：

- **分支策略**：从 `master` 切功能分支，PR 合并前请确保 `west build` 通过。
- **代码风格**：固件遵循 Zephyr 内核风格；脚本用 2 空格缩进。
- **提交信息**：用中文或英文简洁描述「做了什么 / 为什么」。
- **不要提交**：
  - 构建产物与依赖目录：`build/`、`node_modules/`、各类 `*.bak*` 备份；
  - 大体积二进制与本地日志（`log_*.txt`、`serial_capture*.txt` 已在 `.gitignore` 中排除）；
  - 点阵数据头等构建中间产物（`src/image_data*.h`、`src/compressed_*.h`），由第 6 节工具链在本地生成。

---

## 10. 许可证

本项目采用 **MIT License**，详见 [LICENSE](LICENSE)。

---

## 附：技术要点小结

| 环节 | 做法 |
|---|---|
| 连接建立 | BLE Central 扫描 → 连接 → MTU 协商 → NUS 服务发现 → CCCD 订阅 |
| 大帧传输 | `ACL_RX/TX_SIZE = 255` + `L2CAP_TX_MTU = 247`，单帧 payload 242 B |
| 数据压缩 | PC 端 QuickLZ 压缩（约 3.8×），固件端按块边界切包解压 |
| 完整性 | 每帧 CRC16-ARC（大端存储）；NUS 字节流按长度字段切帧防粘包 |
| 流程控制 | 屏端主动请求驱动的有限状态机；大帧写期间加发送保护标志 |
| 图片处理 | 双图层（黑 / 红）分别取模、压缩、传输；极性对齐「0 = 有墨」 |
