# Mocha BCM4354 上电与 HID 输入

`drivers/bluetooth/hci_bcm.c` 对 `nvidia,mocha` 启用上游既有的 `drive_rts_on_open` 顺序：芯片上电期间先释放主机 RTS，退出 shutdown 后恢复流控。修复前仅 HCI Reset 成功；修复后版本／特征读取、固件 RAM 下载、扫描及实际 S102 音频均通过。完整源码与 `source-overlay/` 同步，版权和 GPL 许可保留。

当前 VDE2 已安装显式 serdev 开发模块，无需替换默认内核。工具只构建，不安装或绑定：

```sh
python3 moze/tools/build-bcm-rts-dev-module.py \
  --source /path/to/linux --output /path/to/vde2-output \
  --moze /path/to/moze --destination /path/to/new-bcm-build
python3 moze/tools/build-bluetooth-input-modules.py \
  --source /path/to/linux --output /path/to/vde2-output \
  --destination /path/to/new-input-build
```

两个工具限定 `6.12.111-moze.1-native-vde2` 的已完成构建输出。BCM 工具派生独立 serdev 驱动并保留原串口驱动，避免重复注册全局 HCI UART 协议；公开工具重新编译得到的派生源码 SHA256 与实机已加载候选一致。安装与失败回退在 drivers 仓库的蓝牙加载器中实现。

UHID 和 uinput 从同版本原始源码构建，分别提供 BLE／用户态 HID 与 AVRCP 输入接口。当前配置和 native runtime fragment 已加入 `CONFIG_UHID=m`、`CONFIG_INPUT_UINPUT=m`，后续完整内核构建应携带这些标准模块。ThinkBook BLE 鼠标已建立 UHID 输入节点；K480 的配对码输入、保存绑定、连接及内核键盘输入节点也已通过，常规打字尚未单独确认。不能由模块编译或设备发现直接断言全部键鼠可用。
