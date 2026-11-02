# services115：GPU 平台上下文和 rootful 写入

继承 services114 的 GPU parity ABI 2、Metal 前端、translator、Turnip 和原生分配器更新。源分支及公共组件精确修订见 `configs/pong-gpu-parity.toml`。

## 实际日志

services114 自动同步后回到 REC，取得本轮 console 和 pmsg。GPU 在 GPU native4K/SQE prepare 返回 I/O error，未发布加速器。APFS `apfs_mount_upgrade_checks:2191` 拒绝根卷可写升级。音频物理立体声端点存在，但 VirtualAudio 路由仍报未知属性；尚无声音输出证据。

## 修改

- 恢复 Pong V500 上下文明确的 polled faults、32-bit TTBR0 TBI 和 shared ASID 配置，保留真实寄存器回读和失败处理。
- 系统 BO 的缓存视图复用保留其真实 pmap 缓存属性；物理 MMIO 视图仍禁用缓存。
- 为 22H374 APFS 可写升级中的一个 `vfs_flags` 调用接入 `native_pong_apfs_upgrade_flags`。仅在 `pong_rootful=1`、uid 0、实际根挂载为 APFS、设备与启动时 UUID/UFS 窗口绑定相同、SystemOS 绑定完成且根复查成功时，为该策略调用返回不含 ROOTFS 的标志。真实挂载仍保留 ROOTFS；其他 APFS/VFS 授权、快照和写入条件不变。其他卷不适用。
- 适配器只改变生成候选中的一个 BL 调用，不改变 Apple 输入文件。原 prelink 保留字节声明限于普通链接步骤；额外修改单独记录在 `prelink.json` 的 `pong_rootful_policy` 中。

## 验证边界

内核已编译；设备结果将在自动抓取后补记。当前不宣称 GPU、rootful 或音频已经可用。现有 OS/Data/SystemOS 未重置，services114 用户态与 ABI 2 保持匹配。

## services115 设备结果

- 自动同步、自动回 REC、console/pmsg 本轮标识均已确认。
- GPU native4K/SQE、GMU/HFI、认证 nonsecure fence、IRQ332 和真实加速器发布均通过；Metal 用户态仍被先决条件拒绝，尚未确认实际渲染提交。
- 根卷写探针通过；真实 APT Packages、Release、Release.gpg 已出现在 Data，旧 rootless 标记已移走，Data seed 完成。未完成 firmware/configure 阶段。
- 音频设备和物理端口存在，路由仍返回未知属性。

services116 用户态继续修复：保留原生 setter 缓存；优先使用精确 22H374 路由函数的内部对象编号/响应处理；公开硬件参数检查失败时记录具体字段；计数器零不再被误判为失效，真实 CP fence/ABI/故障检查保留。Sileo 先执行包配置/注册，固件能力刷新有界且保留真实错误。rootful 已产生设备端 OS 写入，因此此次传输完整 OS，不能将主机增量叠加到不同的可写 APFS 状态上；Data/SystemOS 保留。

### 固件版本元数据

rootful seed 的 Sileo 处于 unpacked 状态并依赖 `firmware >= 11.0`，而 bootstrap 原始状态库没有该版本虚拟包。`rootful18.seed_firmware_version` 只从挂载 guest 的实际 SystemVersion.plist 登记 OS 版本，供 dpkg 的软件版本依赖使用；不会登记 GPU、音频、蜂窝等硬件能力。硬件能力仍由原 firmware 工具查询。此项将在 services116 首次启动前补入同一候选 OS。
