# mocha moze linux 6.12.111-moze.1

Mocha/Tegra124 ARMv7适配内核，基于Debian Linux6.12.111完整源码。
这是**带板级修改的完整Linux源码仓库**，不是仅包含补丁、不是已被kernel.org接受的上游主线支持，也没有伪造上游完整Git历史。
第一条提交导入Debian源码快照；第二条提交加入Mocha适配、品牌、配置及设备树。

## 结构

Linux原生arch/drivers/include/kernel/等目录保持完整。项目材料位于：

```
moze/configs/stable-desktop.config      # simpledrm默认桌面
moze/configs/native-experimental.config # Tegra原生双DSI实验
moze/dts/stable-desktop.dts             # 最终背光/触控/四核设备树脱敏快照
moze/dts/native-experimental.dts        # native6链路顺序实验
moze/source-provenance.json            # 基线SHA256与修改列表
moze/tools/build.sh                    # O=独立输出构建
```

`arch/arm/boot/dts/nvidia/tegra124-mocha*.dtsi`保留早期板级模块化源码；首次安装使用moze/dts下最终快照，不能假定早期拼接DTS等同最终默认DT。

```sh
bash moze/tools/build.sh stable ../artifacts/kernel
# 原生显示开发者才使用：
bash moze/tools/build.sh native ../artifacts/native
```

生成的uname release=`6.12.111-moze.1`；显示名称为`mocha moze linux 6.12.111-moze.1`。
当前改名后的构建尚未完成全流程实机复测，不能把旧版本实测状态直接等同本次新产物。

## 实现

reset.c通过板级显式DT属性调用TLK SMC0x82000001设置安全reset vector，继续使用TegraPMC/flow SMP，四核实测。
palmas-regulator.c/DT保持原厂供电的software模式，避免切外控造成屏幕熄灭；bq24190新增板级充电使能处理。
ChipIdea/Tegra USB会话检测结合真实PMIC输入，取代早期假定VBUS的实验；断插SSH已实测。
earlycon/head/init诊断用于在时钟/PMIC初始化前保留可读屏幕日志，诊断暂停只能显式启用，默认关闭。
panel-sharp和tegra/dsi实现双链路、LP命令时钟、ganged布局和短DCS返回长度处理；native色块通过，但Niri fence仍崩溃。

外置背光/音频实验放[drivers](https://github.com/Pisces-Moze/mocha-moze-drivers)。安装/问题/状态统一见[总入口](https://github.com/Pisces-Moze/mocha-moze-debian)。

原生实验使用独立release `6.12.111-moze.1-native`，不能加载稳定profile的模块。构建显式设置LOCALVERSION为空，避免无上游tag的源码快照自动加“+”。安装模块时剥离debug信息控制小APP分区占用；构建目录保留原始调试文件。

此次发布验证：稳定/实验DTS编译通过，stable olddefconfig输出6.12.111-moze.1；新品牌版本尚未完成完整内核重编译及实机重新刷机。完整源码在Git中保留，Windows本地使用sparse checkout，仅展开moze与根目录文件，避免大小写碰撞；Linux开发者普通clone会获得完整树。
