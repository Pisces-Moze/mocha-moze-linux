# 协作方式

通用规则（证据要求、断言口径、不要提交的东西、许可与来源）见总入口的 [CONTRIBUTING.md](https://github.com/Pisces-Moze/mocha-moze-debian/blob/main/CONTRIBUTING.md)。这里只写本仓库特有的验收口径。

## 本仓库的验收口径

- 说清改的是哪套 profile：`stable`（`moze/configs/stable-desktop.config`）还是 `native`（`moze/configs/native-experimental.config`），或者两套都改。两份配置的 `CONFIG_LOCALVERSION` 不同（`-moze.1` 与 `-moze.1-native`），产物不能交叉使用。
- 改设备树要写明改的是 `moze/dts/stable-desktop.dts` 还是 `moze/dts/native-experimental.dts`，并说明是否重新做过实机冷启动。只改 `arch/arm/boot/dts/nvidia/tegra124-mocha*.dtsi` 那些早期拼接源码，不等于改了安装时用的默认 DT。
- 新增或删除改动路径时，同步 `moze/source-provenance.json` 的 `changed_paths` 与 `moze/source-overlay/`，让记账和实际改动一致。
- 上游基线与 Mocha 改动分开提交，不要把格式化、重排或大面积改名混进板级改动，那样既没法 review 也没法回退。
- 附 `bash moze/tools/build.sh stable ../artifacts/kernel` 的输出结尾（`kernelrelease`），并说明是否重新刷机验收过。
- 改了配置要说明是 `olddefconfig` 重新生成的还是手工编辑的；手工编辑写清改了哪几项、为什么。
- Windows 上按 README 的 sparse checkout 做法操作，注意不要把大小写碰撞的路径带进提交。

## 提交前自查

- 没有把构建产物（`uImage`、`Image`、`mocha.dtb`、`modules/`、`SHA256SUMS`）带进提交。
- `moze/source-provenance.json` 的 36 条 `changed_paths` 与实际改动仍然一致。
- 外置模块（背光、音频）仍然能在这次构建的 `O=` 目录上编译；跨 release 的 `.ko` 不写进文档当作可用。
