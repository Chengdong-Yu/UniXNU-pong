# services148：修复 UniXNU 菜单停留

## 根因

services147 完整打包并获得 Fastboot Sending/Booting OKAY，但没有枚举 XNU USB，也没有新的 XNU console。REC 保存的是 Linux 日志，不应当作 XNU 启动结果。

对照实际 DarwinBoot24 `BootArgs.h` 和 `DarwinBoot.c`：`CommandLine` 数组为 1024 字节，字符串至多 1023 字节，超出即在内核加载/交接前返回 EFI_BAD_BUFFER_SIZE。services147 的最终参数为 1025 字节；services144 为 1000 字节。追加 Preferences、tonelibraryd 日志过滤项使服务修复镜像在 loader 阶段被拒绝，与音频或显示运行时代码是否成功无关。

## 修复

缩短可选的 `applog_name` 过滤列表，保留全部功能参数、8 核、128MiB GPU 池、USB、128GB Data、铃声和唤醒修补。保持 boot_args ABI 和 loader 长度检查，不截断功能参数。新增独立命令行长度 helper 和本机 AGENTS 约束，供打包/启动入口按源配置计算长度；用户修改的现有构建脚本未改动。

本轮仅重新打包启动镜像，继续使用 services147 已安装的配套 OS，不写系统或 Data。实际参数长度、临时启动与枚举结果记录在 PongDev services148 日志。
