# Mocha 下游内核改动

基于 Debian Linux 6.12.111 的 Tegra124/Mocha ARMv7 适配。本目录保留版本来源、覆盖文件、构建工具和阶段验收；根目录为完整 Linux 源码。

2026-10-27 同步了此前尚未发布的 BCM 蓝牙 RTS、VDE 多平面/布局和原生日志叠加启动背景实现。启动动画在进入图形 VT、解绑 framebuffer 或超时后调用 detach，取消后续绘制、恢复原 fbops 并释放临时文本/覆盖缓冲；头像静止，仅局部加载圈定时更新。源码机制与本轮实机运行检查应区分，后者仍需设备连接。

- [蓝牙输入设备](BLUETOOTH-INPUT.md)
- [Tegra124 VDE](TEGRA124-VDE.md)
- [下游源码来源](source-provenance.json)
- [系统安装与镜像候选状态](https://github.com/Pisces-Moze/mocha-moze-debian)

VDE 目前支持受限的逐行 8-bit 4:2:0 constrained Baseline H.264，不代表所有网站、CABAC 或高规格 AVC 视频均可硬解。完整 Firefox 原生 Request 接入在桌面仓库继续构建，内核驱动通过不能代替浏览器验收。
