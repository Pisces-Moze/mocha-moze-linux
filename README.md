# mocha moze linux 6.12.111-moze.1

> 带 Mocha 板级适配的 Debian Linux 6.12.111 完整源码仓库，为小米平板 1（A0101，NVIDIA Tegra124）提供可复现的内核构建。

[![License: GPL-2.0-only](https://img.shields.io/badge/License-GPL--2.0--only-blue.svg)](LICENSE)

## 简介

这是**完整的内核源码仓库**，不是只放补丁的仓库，也不是 kernel.org 已接受的上游支持；仓库里没有伪造的上游 Git 历史。2026-10-07 的快照把三件事分开提交：导入 Debian 6.12.111 源码基线、加入 Mocha 适配与 stable/native 两套配置、发布实现说明与可复现的构建工具。

树内只放需要跟内核一起编译的板级改动。背光、音频、触控和 GPU/CUDA 诊断在 [mocha-moze-drivers](https://github.com/Pisces-Moze/mocha-moze-drivers)，那边刻意不重复维护树内 USB、PMIC、DSI 驱动。安装、回退和设备状态由 [mocha-moze-debian](https://github.com/Pisces-Moze/mocha-moze-debian) 统一负责，U-Boot 在 [mocha-moze-boot](https://github.com/Pisces-Moze/mocha-moze-boot)，桌面与充电界面在 [mocha-moze-desktop](https://github.com/Pisces-Moze/mocha-moze-desktop)。

## 上游基线与命名

| 项 | 值 |
|---|---|
| 基线 | Debian `linux-source-6.12`，Makefile 版本 6.12.111 |
| 源包 SHA256 | `dcb52c568b7f906ec831feeadabb1d60435040bf0f1f3976be371c9a5c12582a` |
| 人类名称 | `mocha moze linux 6.12.111-moze.1`（Makefile 的 `NAME` 为 `mocha moze linux`） |
| 稳定内核 release | `6.12.111-moze.1` |
| 原生实验 release | `6.12.111-moze.1-native` |
| 改动路径记账 | `moze/source-provenance.json`，36 条（14 条设备树，13 条在 `drivers/`） |

三份提交：

```
Import Debian Linux 6.12.111 source baseline
Add Mocha board adaptation, stable and native profiles, and Moze branding
Publish Mocha source snapshot, implementation notes and reproducible integration tools
```

`moze/source-provenance.json` 同时记下基线 SHA256、品牌名、快照日期 2026-10-07，以及 `private_identity_removed: true`：公开设备树里已经去掉本机身份、旧 bootargs 与 initrd 地址，所以它的 SHA256 与历史上实测的私有 DT 不同，不能沿用旧哈希。

## 目录结构

```
moze/configs/stable-desktop.config        # 稳定桌面：CONFIG_LOCALVERSION="-moze.1"
moze/configs/native-experimental.config   # 原生双 DSI 实验：CONFIG_LOCALVERSION="-moze.1-native"
moze/configs/smp-base.config              # 早期诊断配置，build.sh 不接受
moze/dts/stable-desktop.dts               # 最终背光/触控/四核设备树脱敏快照
moze/dts/native-experimental.dts          # 原生链路顺序实验
moze/dts/native-link-order.patch          # 链路起点顺序的差异记录
moze/dts/native-lp-clock.patch            # 两路 DSI 的 LP 命令时钟改为 12 MHz
moze/source-overlay/                      # 与上游同形状的覆盖树
moze/tools/build.sh                       # O= 独立输出构建
moze/source-provenance.json               # 基线 SHA256 与改动路径清单
```

Linux 原生的 `arch/`、`drivers/`、`include/`、`kernel/` 等目录保持完整。`arch/arm/boot/dts/nvidia/tegra124-mocha*.dtsi` 是早期按模块拆分拼接的板级源码，仍然保留在树内；**首次安装用的是 `moze/dts/` 下的最终快照**，不能假定早期拼接 DTS 与最终默认 DT 等价。

`moze/source-overlay/` 镜像了改动路径的形状：`Documentation/devicetree/`、`arch/arm/{boot,kernel,mach-tegra,mm}/`、`drivers/{bluetooth,clk,gpu,power,regulator,tty,usb}/`、`init/main.c`。

## 两套配置与设备树

| | stable-desktop | native-experimental |
|---|---|---|
| `CONFIG_LOCALVERSION` | `-moze.1` | `-moze.1-native` |
| `CONFIG_LOCALVERSION_AUTO` | 关闭 | 关闭 |
| 显示路径 | 把已初始化的屏幕交给 simpledrm | Tegra DRM / 双 DSI |
| 用途 | 默认桌面，先选这条 | 仅供原生显示开发 |

两份 config 的其余关键项一致：`CONFIG_ARM_LPAE` 关闭（32 位非 LPAE）、`CONFIG_SMP=y`、`CONFIG_HIGHMEM=y`、`CONFIG_DRM_TEGRA=y`、`CONFIG_DRM_NOUVEAU=m`。配置里记录的编译器是 `arm-linux-gnueabihf-gcc (Debian 14.2.0-19) 14.2.0`（`CONFIG_GCC_VERSION=140200`）；native 配置开了 Rust 支持（`CONFIG_RUSTC_VERSION=109500`），stable 是 `CONFIG_RUSTC_VERSION=0`。

`moze/configs/smp-base.config` 不属于这两套：它的 `CONFIG_LOCALVERSION` 是 `-mocha-experimental-fbdiag` 且开着 `LOCALVERSION_AUTO`，`build.sh` 只认 `stable` 和 `native`，要用这份配置得自己指定。

两份设备树的完整逐行比对结果：stable 3330 行，native 3341 行，**差异只有 23 行**。差异集中在五处：

- native 给 `dsi@54300000` 与 `dsi@54400000` 各加了三行 `assigned-clock-rates = <0xb71b00>`（12 MHz）、`assigned-clock-parents`、`assigned-clocks`，这就是 `native-lp-clock.patch` 记下的改动；LP 命令阶段的频率从 68 MHz 换成已验证 U-Boot 命令阶段的 12 MHz 之后，屏幕才出现 GPU 色块。
- `nvidia,ganged-mode` 与 `link2` 的 phandle 不同（stable `0x0b`/`0x0c`，native `0x36`/`0x37`），对应 543 与 544 谁是主链路的顺序差异，来源是 `native-link-order.patch`。
- `nvidia,mipi-calibrate` 从 stable 的 `<0x0a 0x180>` 改成 native 的 `<0x0a 0x0c>`。
- 三处 `status` 在两边取值相反。
- native 侧多出 `reset-names = "serial"` 与 `serial2 = "/serial@70006200"` 别名。

两份 DTS 的公共头部一致：`compatible = "nvidia,mocha", "nvidia,tegra124"`、`model = "NVIDIA Tegra124 MOCHA"`、`nvidia,boardids = "1780:1100:3:A:7", "1845:1000:0:A:7"`，并且带上 `nvidia,mocha-secure-reset-vector` 属性，它是四核复位改动的开关。

## 板级改动清单

改动按区域分成六组，完整逐条清单以 `moze/source-provenance.json` 的 `changed_paths` 为准。

| 区域 | 文件 | 做了什么 |
|---|---|---|
| 内核核心与早期诊断 | `arch/arm/kernel/{head.S,head-common.S,setup.c,time.c}`、`arch/arm/mm/mmu.c`、`init/main.c`、`drivers/tty/serial/earlycon.c`、`arch/arm/mach-tegra/io.c` | 一套早期 framebuffer 诊断：把 bootloader 已经点亮的 framebuffer 用固定设备别名 `0xfda00000`（物理 `0xf1700000`，长 `0x600000`）保留到 `paging_init()` 之后，`mocha_fb_checkpoint()` 在 `of_clk_init` 等阶段把进度打到屏幕上。源码注释里逐处标了 RAM-only，不是产品功能 |
| 四核与安全复位 | `arch/arm/mach-tegra/{reset.c,io.c}` | `reset.c` 通过板级显式 DT 属性调用原厂 TLK SMC `0x82000001` 设置安全 reset vector，SMP 操作继续用 Tegra PMC/flow-controller；四核已实测 |
| 供电与充电 | `drivers/regulator/palmas-regulator.c`、`drivers/power/supply/bq24190_charger.c`、`Documentation/devicetree/bindings/power/supply/bq24190.yaml` | Palmas 保持原厂供电的 software 模式，避免切外部控制后屏幕熄灭；bq24190 增加 `ce-gpios` 充电使能控制，绑定文档同步补上这个属性 |
| USB | `drivers/usb/chipidea/{ci_hdrc_tegra.c,otg.c,udc.c}`、`drivers/usb/phy/phy-tegra-usb.c` | Mocha 的 device VBUS 走 Palmas 比较器，改动把它的真实状态同步进 Tegra UTMI 的 session selector，而不是强行标记成已连接；OTG 与 UDC 分支用 `of_machine_is_compatible("nvidia,mocha")` 限定范围。PHY 侧是显式选入的 RAM 测试，只设 session detector、不控制 VBUS 输出，该模式下没有拔出检测 |
| 显示 | `drivers/gpu/drm/tegra/dsi.c`、`drivers/gpu/drm/panel/{Kconfig,Makefile,panel-sharp-lq079l1sx01.c}` | 双链路、LP 命令时钟、ganged 布局、短 DCS 返回长度处理；面板驱动进树 |
| 其他 | `drivers/clk/tegra/clk-tegra124.c`、`drivers/bluetooth/btbcm.c` | 时钟与蓝牙的板级调整 |

设备树这一组是 14 个文件：主 `tegra124-mocha.dts` 加上 touchscreens、bluetooth、fixed、pmic、sdhci、i2c、power、identity、display、mipi、pinmux、usb、keys 十三个 `.dtsi`。

## 构建

```sh
# 稳定桌面内核（默认路径）
bash moze/tools/build.sh stable ../artifacts/kernel
# 原生显示实验内核，只有原生显示开发者需要
bash moze/tools/build.sh native ../artifacts/native
```

第一个参数选 profile，第二个是输出目录（默认 `../artifacts/kernel`）；`JOBS` 控制并行度，默认 4。脚本按顺序做这几件事：

1. 按 profile 选配置：`stable` 用 `moze/configs/stable-desktop.config`，`native` 用 `moze/configs/native-experimental.config`，其它取值直接退出。
2. 把配置复制成 `$out/.config`，跑 `make O=$out ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- olddefconfig`。
3. 编译 `Image` 与 `modules`。
4. `modules_install` 到 `$out/modules`，带 `INSTALL_MOD_STRIP=1` 剥掉调试符号。
5. 用 `dtc` 把 `moze/dts/<name>.dts` 编成 `$out/mocha.dtb`。
6. 用 `mkimage -A arm -O linux -T kernel -C none -a 0x80008000 -e 0x80008000 -n 'mocha moze linux 6.12.111-moze.1'` 把 `arch/arm/boot/Image` 打成 `$out/uImage`。
7. 复制一份 `Image`，对 `Image`、`uImage`、`mocha.dtb`、`.config` 生成 `SHA256SUMS`，最后打印 `kernelrelease`。

脚本开头有一行 `export LOCALVERSION=`，这是刻意的：构建时显式置空，避免继承 shell 里残留的值；release 后缀由配置里的 `CONFIG_LOCALVERSION` 决定。两份配置都关掉了 `LOCALVERSION_AUTO`，否则这份没有上游 tag 的源码快照会自动加上 `+`。

前置条件：交叉工具链 `arm-linux-gnueabihf-`（配置快照来自 gcc 14.2.0）、`dtc`、`mkimage`（`u-boot-tools`），以及 `make`、`bc`、`bison`、`flex`、`libssl-dev`、`libelf-dev`。构建用 `O=` 独立输出，不污染源码树，可以同时留几份不同 profile 的输出。

输出是 `uImage`、`Image`、`mocha.dtb`、`modules/`、`.config` 和 `SHA256SUMS`；`kernelrelease` 由 make 打印，构建时要保存日志并与模块目录核对。总入口 [INSTALL.md](https://github.com/Pisces-Moze/mocha-moze-debian/blob/main/docs/INSTALL.md) 里的构建顺序是 kernel → 外置驱动 → U-Boot/容器 → rootfs/桌面 → RAM 验证 → APP → LNX；内核产物复制进 rootfs 时改名为 `/boot/uImage-desktop` 与 `/boot/mocha-desktop.dtb`。

外置模块（`mocha-moze-drivers` 的背光与音频）必须用同一份 `O=` 目录的头文件和 `Module.symvers` 编译，装进同一个 release 的模块目录。跨 release 放进去的 `.ko` 不会生效。

## 验证范围与当前状态

2026-10-08 的发布只验证到源码级别，没有重新构建并刷机：

| 检查 | 结果 |
|---|---|
| 两份设备树 | stable-desktop 与 native-experimental 都能被 `dtc` 编成 DTB，警告保留 |
| stable 配置 | `olddefconfig` 通过，`kernelrelease=6.12.111-moze.1` |
| 源码树 | 86,744 个 Git 跟踪文件，上游基线与 Mocha 改动、规范化构建提交相互分离 |
| 改名后的内核 | 没有重新走完整流程刷机验收 |

2026-10-09 继续诊断时，原型机 `/proc/config.gz` 确认 `CONFIG_BINFMT_ELF=y`、`CONFIG_COREDUMP=y`，但 `CONFIG_ELF_CORE` 关闭。这解释了历史 Niri 崩溃没有生成 ELF core 文件。现已把 stable 与 native 两套公开配置中的 `CONFIG_ELF_CORE` 改为 `y`，构建脚本在 `olddefconfig` 后检查上述三个选项，防止再次构建出无法保存用户程序 core 的内核。`smp-base.config` 保持历史对照。两份新配置已在 Debian 13／GCC 14.2.0 构建主机上通过 `olddefconfig`，三个选项均保留为 `y`；截至 2026-10-10，stable/native 的 Image/uImage、模块与 DTB 完整构建均通过，两个 vmlinux 都包含 `elf_core_dump`；实际 release 分别为 `6.12.111-moze.1` 与 `6.12.111-moze.1-native`。后续 RAM 启动与实机 core 捕获结果见下方 2026-10-10 记录，不能沿用上表的旧配置验证结果。捕获 core 时指定数据分区或 `/tmp` 的私有目录并控制大小；不要让 core 占满 APP，也不要把 core 提交到仓库。

实机状态按总入口的状态表汇总，与内核和 DTS 有关的部分是这样：四核 CPU 0–3 上线、逐核负载与冷启动通过，用的是原厂 TLK SMC，CPU DVFS 尚未启用；默认显示冷启动、横屏、触控、亮度通过，simpledrm 输出仍有同步与 CPU 拷贝开销；原生 Tegra 双 DSI 下 GPU 线性 DMA-BUF 色块实机可见、约 29.8 FPS，旧系统 Mesa 下 Niri 曾 SIGSEGV；候选 Mesa 配合原始内核＋手动校正的 RAM Niri 画面已确认正常；Nouveau NVEA / GK20A 硬件着色器通过，固件需要自行提取，DVFS 与热管理未完成；Wi-Fi BCM4354 可用；音频未完成；摄像头、OTG、休眠未适配。

2026-10-10 后续 RAM 实测：上述 stable/native 两套新内核均成功启动，USB SSH 可用、CPU 0–3 在线、三个 core 选项均为 y。受控 SIGSEGV 在 RAM `/tmp` 各生成 327680 字节 ELF32/ET_CORE/EM_ARM 文件，大小、头部与 SHA256 已保存；捕获时 eMMC 只读且未挂载。stable Linux 日志画面由用户确认正常；native 显示的单独诊断见总入口的 [RAM 记录](https://github.com/Pisces-Moze/mocha-moze-debian/blob/codex/mocha-diagnostics-2026-10-09/docs/DIAGNOSTICS-2026-10-10.md)。core 捕获已通过，完整安装和原生桌面验收仍未完成。

后续同内核 RAM 对照恢复 native5 的 DSI-B 主机／面板控制归属后，用户确认色块可见但左右交换，控制台从中间开始；仅把 DSI-A/B 横向起点改为 0/768 后，用户确认位置正常。因此 native DTS 保留 DSI-B 主机和 12 MHz LP 时钟，新增本地绑定 `nvidia,ganged-mode-swap-links`，尝试让驱动在每次 modeset 时交换扫描半屏，避免以交换控制主从来修正画面。属性缺省时保留原有左右分段；stable DTS 不变。两版候选的内核、模块与 DTB 完整构建均通过，但自动应用的 RAM 实测均失败：视频 enable 前校正黑屏；保留原始分段启动视频、enable 后等待 40 ms 再校正也黑屏。两次起点读数均正确，不能据此标记修复通过。同一个新内核使用此前可显示的无校正 native5 DTB 也黑屏，原始内核＋相同 DTB 则再次显示成功。原始组合手动校正后，GPU 色块与候选 Mesa 下的 Niri＋终端已由用户确认正常，仍不能称自动方案通过。原始驱动重建基线与此前成功内核的机器指令完全相同，仅 34 字节构建元数据不同；重建基线首次黑屏，DRM 关闭再开启面板后恢复，手动校正的 GPU/Niri 画面正常。当前候选在 Mocha DSI runtime resume 中，时钟就绪后主动 assert reset，再沿用原有等待与 deassert，尝试清除首次初始化继承状态；native 完整目标与匹配背光模块构建通过，下载哈希、CRC 与源码一致性检查通过；RAM 实机验收待完成。DPMS 同时改变多项状态，尚不能证明复位为唯一根因。

编译通过不等于实机通过，DTB 编出来也不等于外设跑起来。内核侧的结论以实机屏幕、`uname`、四核上线和 journal 为准。

## 未实现与计划

| 事项 | 现状 | 出处 |
|---|---|---|
| CPU DVFS、热管理、suspend | 四核上线不代表这些完成；CPU/GPU 调频与超频保持未启用 | `arch/arm/mach-tegra/reset.c` 的改动范围、总入口 `docs/STATUS.md` |
| 原生显示路径默认化 | 候选 Mesa 的独立 RAM Niri 画面通过；新内核自动分段黑屏，完整桌面未验收，尚未替换默认路径 | `moze/configs/native-experimental.config`、总入口 `docs/ISSUES.md` |
| 左右链路的最终画面 | 原始内核手动设置 A=0/B=768 后画面正常；新镜像自动方案仍黑屏，寄存器读数不能代替实测 | 总入口 `docs/ISSUES.md` |
| Tegra124 硬件编解码 | VDE 的非标准 tile 布局没有完整格式，只有软件解码可用 | 总入口 `docs/ISSUES.md` |
| 音频 | RT5671 在 `0x1c` 返回 NACK，ALSA 无卡；树内的 RT5670 codec 只作接口参考 | `drivers/` 树内 codec 与本仓库配置、总入口 `docs/ISSUES.md` |
| 蓝牙、摄像头、OTG、休眠 | 现代内核的 UART、固件、GPIO 与配对待完成；控制器、传感器、VBUS 与恢复链路待适配 | 总入口 `docs/STATUS.md` |
| 改动推向上游 | 这些改动是为这一台设备写的，没有提交到 kernel.org，也没有逐项拆成可 review 的补丁序列 | 本仓库的三条提交 |

## 常见问题

**native 的模块能装进 stable 吗？** 不能。两份配置的 `CONFIG_LOCALVERSION` 不同（`-moze.1` 与 `-moze.1-native`），release 名不同，vermagic 对不上，`insmod` 会被拒绝。同一个 profile 内部也不要把不同构建的 `.ko` 混着用。

**`smp-base.config` 为什么用不上？** `build.sh` 只接受 `stable` 与 `native`。这份配置的 `CONFIG_LOCALVERSION` 是 `-mocha-experimental-fbdiag`，而且开着 `LOCALVERSION_AUTO`，是早期诊断留下的，用它要自己指定配置与后缀。

**树里那套 `tegra124-mocha*.dtsi` 和 `moze/dts/` 是什么关系？** 前者是早期按模块拼接的板级源码，后者是最终实测配置的脱敏快照。首次安装用 `moze/dts/stable-desktop.dts`，不要假定早期拼接的结果与最终默认 DT 等价。

**在 Windows 上怎么用这个仓库？** 完整源码树在 Windows 上无法整体检出：`drivers/gpu/drm/nouveau/nvkm/subdev/i2c/aux.c`、`include/soc/arc/aux.h` 这类名字撞上 Windows 保留设备名，`git checkout` 会报 `invalid path` 并且工作区留空。Windows 上的做法是 sparse checkout，只展开 `moze/` 与根目录文件，避免大小写碰撞；完整构建在 Linux 上做。

**生成的 `uname -r` 是什么？** `6.12.111-moze.1`，原生实验内核对 `6.12.111-moze.1-native`。人类可读名称是 `mocha moze linux 6.12.111-moze.1`，空格只出现在名称里。

**驱动实验在哪？** 背光、音频、触控、诊断和 CUDA 都在 [mocha-moze-drivers](https://github.com/Pisces-Moze/mocha-moze-drivers)，本仓库刻意不重复维护树内 USB、PMIC、DSI 驱动。

## 许可证与来源

本仓库新增的内核与驱动适配采用 GPL-2.0-only，条款见 [LICENSE](LICENSE)；Debian 基线保留 `COPYING`、`LICENSES` 与各自的版权头。参考的厂商 GPL 源码是 MiCode 的 `mocha-kk-oss`（`79b4898e25fe3b506ff902e182b47068598c838a`），移植过来的文件保留原有版权与 GPL 声明。

| 项目 | 基线 / 出处 | 使用方式 |
|---|---|---|
| Linux | Debian `linux-source-6.12`，Makefile 6.12.111 | 完整源码快照，Mocha 修改单独提交 |
| 厂商源码 | [Xiaomi_Kernel_OpenSource](https://github.com/MiCode/Xiaomi_Kernel_OpenSource)，`mocha-kk-oss` | 复位/SMP、面板、PMIC、USB、音频与触控的移植依据 |
| 社区 Mocha | [Insei/linux](https://github.com/Insei/linux)，`1e3857d7a1ea87cd2cc15eca2d36f57cb591c4bf` | 早期板级与面板参考，后续逐项对照官方源码与实机 |

本仓库不授予 NVIDIA CUDA、MIUI 固件、Wi-Fi/蓝牙固件或 TFA DSP 参数的再分发权，需要从合法持有的设备或官方包中自行提取。同样的边界另见 [LICENSE-NOTES.md](LICENSE-NOTES.md)，完整的源码来源见总入口的 `SOURCES.md`。

## 相关仓库

同一个工程的另外四个仓库：[mocha-moze-debian](https://github.com/Pisces-Moze/mocha-moze-debian)（总入口、rootfs、RAM 安装器与安装文档）、[mocha-moze-boot](https://github.com/Pisces-Moze/mocha-moze-boot)（U-Boot 2026.07 覆盖与 Android 容器）、[mocha-moze-drivers](https://github.com/Pisces-Moze/mocha-moze-drivers)（树外驱动适配与诊断）、[mocha-moze-desktop](https://github.com/Pisces-Moze/mocha-moze-desktop)（Niri/Noctalia 桌面与竖屏充电界面）。
