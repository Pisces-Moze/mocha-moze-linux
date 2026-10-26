# Tegra124 VDE 开发候选

此候选补齐当前上游 Tegra124 缺少的分块输出兼容路径。渐进式 CAVLC 解码及 I420 输出已在实机通过逐帧校验，MPV Request 硬解与 GPU 显示也已通过实机画面确认。完整 vde2 内核已通过一次 RAM 冷启动、正式壁纸插件与全屏资源释放测试；重复冷启动、持久安装与浏览器接入仍需分别验收。

上游 Linux 的 Tegra124 `decoded_fmts` 为空，不能仅启用 `CONFIG_VIDEO_TEGRA_VDE` 或设置 `hwdec=auto`。现有项目的默认 DT 也没有 VDE 节点。

## 实现范围

- 从维护者的实现移植 BSEV 独立时钟／复位管理，包含恢复失败时的释放路径。缺少 BSEV 描述的 Tegra124 在 probe 阶段报错，避免访问未启时钟的寄存器。
- Tegra124 使用 `16Bx2 / one GOB` 分块参考帧；帧表地址编码包含对应高位。硬件 DPB 保存分块内容，不把线性显示输出再次当作硬件参考帧。
- 初版通过显式 CPU 布局转换输出标准三平面 YUV420M / YVU420M，供 V4L2 stateless 用户态使用。此路径包含额外缓冲及一次布局转换，**不是零拷贝**。
- Capture 使用可映射的 MMAP 缓冲，可向下游导出 DMA-BUF。尚不支持导入任意 Capture DMA-BUF，避免把不可 CPU 映射的外部缓冲当作可写线性目标。
- 尺寸、平面大小、SPS 与 Capture 几何、DMA 方向和映射生命周期分别检查；失败清理包含私有分块缓冲。
- 私有 DPB 在有 IOMMU 时使用可缓存的普通页面，布局转换前后显式交接 DMA 所有权并同步 vmap 别名。无 IOMMU 时保留连续 DMA 分配。缓冲只在解码完成后由 CPU 读取，不把缓存映射当作硬件一致性内存。
- Tegra124 的同步 IRQ 在电源关闭时保持屏蔽；恢复时钟、清除旧完成状态后才启用，在复位和断电之前同步关闭。热加载先建立 m2m 队列再公布视频节点，避免 udev 过早打开。
- Request 控制提交、硬件启动与完成等待统一在同一个 worker 中执行，修复 ioctl 线程加锁后由 worker 解锁的跨任务 mutex 所有权错误。旧模块曾触发 DEBUG_LOCKS 警告；修复后的 vde2 完整内核在新冷启动中保持 DEBUG_MUTEXES 开启，完成解码与桌面测试后没有锁警告，两个阶段的记录分别保留。
- 暴露 FFmpeg Request 所需的 scaling-matrix 控制，仅接受系数全为 16 的平坦 4×4 矩阵；拒绝自定义矩阵以及使用 8×8 transform 的 PPS，不静默忽略不支持的缩放。
- 默认关闭，仅带有 `tegra_vde.tegra124_enable=1` 的候选启动允许 Tegra124 probe。稳定 DT 与默认引导不更换。

当前解码核心仍拒绝 CABAC 和交错视频；没有取消此检查或把不支持的码流伪装成已支持。第一轮测试使用独立生成的渐进式 CAVLC 样本，与软件解码逐平面对照。原始用户视频不提交到仓库。

## 设备树

`dts/native-vde-experimental.dts` 包含已验证的 `native-runtime.dts`，保留 DFLL 和 Wi-Fi keep-power 设置，新增实际 Tegra124 VDE 地址 `0x60030000`，不是 Tegra20 的 `0x6001a000`。描述 VDE/BSEV、MC reset、SMMU 与独立 SRAM 池。池位于 IRAM `+0x10000..+0x1ffff`，避开 CPU reset 首 1 KiB 和 `+4 KiB` 的 LPx resume 区。候选 VDE 时钟设为 204 MHz，不调整电压。

## 验证状态

布局转换主机测试覆盖 54 组 pitch / height、多个 GOB 行边界、输入输出不足和目标缓冲前后哨兵，配合 AddressSanitizer / UndefinedBehaviorSanitizer。

后续同内核关闭内置 T124 probe 后成功从 RAM 启动，通过独立开发模块隔离验证。`build-vde-dev-module.py` 使用匹配的内核输出目录，仅创建模块，不安装或加载；模块使用独立驱动和 trace 命名空间，避免与内置驱动冲突。当前默认引导继续保留。

显式 GStreamer `v4l2slh264dec` 管线（没有软件解码回退）完成四组共 96 帧 I420 比较，每帧 SHA256 均匹配软件参考，覆盖多参考帧 DPB 复用、GOB 边界以及 1536×1086 的裁剪输出。YVU420M 尚未完成内容校验：GStreamer 1.26.2 的 stateless 元件不接受 YV12 输出协商，不能把该失败解释为内核输出错误或视为已通过。

在 CPU 上限 612 MHz、schedutil、VDE 204 MHz、未运行桌面的条件下，1536×1086 / 30 fps、540 帧样本通过 `fakesink sync=false` 的耗时由 WC 缓冲的 74.93 秒降为缓存页面的 13.93 秒。三轮 `sync=true` 定时解码各约 18.5 秒，进程 CPU 约为单核的 30.4%～30.6%。这些是解码测试，不能作为桌面动画或最终壁纸播放帧率。

独立 FFmpeg 7.1.5 Request 回移植补齐三平面 Capture 排队、DRM 对象／平面描述与线性 YUV420 下载后，同样四组 96 帧全部匹配软件参考。MPV 0.40.0 显式使用 `v4l2request`，报告 DRM_PRIME / YUV420P 输出，通过三平面 DMA-BUF 交给 GPU 显示；用户确认画面、颜色与播放正常。20 秒采样未新增 decoder / VO 丢帧，播放器进程 CPU 约为单核的 26.45%。进程 CPU 不包含内核 worker 和桌面合成器，不能当作整机负载。

播放器与 FFmpeg 安装在独立候选目录，通过局部库路径选择，没有替换系统 libavcodec / libmpv。`hwdec=auto` 的安全白名单不会自动选择此 Request 后端，需要显式选择；不支持的码流不能被计作硬解成功。开发模块、逐帧结果与播放器状态见 [validation/vde-player-2026-10-17.json](validation/vde-player-2026-10-17.json)。

每轮结束能进入 runtime suspend，模块引用计数回到 0；连续三轮记录没有 IOMMU fault。CABAC 被拒绝后，下一段支持的码流能正常完成。这不等于硬件超时恢复或长期内存泄漏测试已通过。当前源码、模块及测试数据见 [validation/vde-hardware-2026-10-17.json](validation/vde-hardware-2026-10-17.json)。

首次完整 `6.12.111-moze.1-native-vde1` 临时启动未恢复管理连接，该轮失败及原始产物哈希保留于 [validation/vde-candidate-2026-10-17.json](validation/vde-candidate-2026-10-17.json)。后续关闭内置 probe 后从 RAM 启动同一内核并加载开发模块完成上述验证。包含最新缓存、worker 与控制修复的 `6.12.111-moze.1-native-vde2` 已通过 54 组 ASan / UBSan 布局测试及内置 VDE 开启的 RAM 冷启动；GStreamer 和 FFmpeg 两套四组样本各 96 帧再次匹配软件参考。正式壁纸插件以 Request 解码，三轮前台全屏切换均释放视频与 DMA-BUF 句柄、进入 runtime suspend，退出全屏后恢复硬解。壁纸显示阶段仍有 VO 丢帧与顶部弹窗卡顿，不能视为桌面性能通过。默认引导仍保留；重复冷启动、系统 suspend / resume、硬件超时恢复、长期资源释放和浏览器硬解尚未通过。完整内核记录见 [validation/vde-full-kernel-2026-10-17.json](validation/vde-full-kernel-2026-10-17.json)。

## 来源

- [Thierry Reding 的布局实现](https://github.com/thierryreding/linux/commit/122cc72828152f91962037a9bfbec852a5a7dbb0)
- [BSEV 时钟与复位](https://github.com/thierryreding/linux/commit/6ed81b6ad0023d304fc6c7f0c2fd013d578ffbc6)
- [Tegra124 VDE DT](https://github.com/thierryreding/linux/commit/4f0830f5a48dded48f2174a6a07d50fbf27d8d0d)
- [Tegra124 VDE SMMU](https://github.com/thierryreding/linux/commit/a5f4d188102fa5fa2b6d577836dbae11e2ea38a9)
- [Chromium minigbm 的 Tegra 布局](https://android.googlesource.com/platform/external/minigbm/+/6c504b854cc46897c5dc59649215570326529083/tegra.c)

移植文件保留原 GPL 版权头；上游仍处于 WIP 的 ABI／V4L2 改写没有整体套用。后续直接 DMA-BUF 分块导出／GPU 布局转换和更完整码流支持应在内容正确性通过后推进。
