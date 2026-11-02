# services87：修复 services86 显示回退

## 日志与根因

REC console 为本机 `out/ipados18/logs-services86/pstore`，另从 Data 读取了对应 IPS 和现有运行日志（81 次有界 64KiB 诊断读取，没有镜像校验/回读比较）。摘要在本机 `crash-summary.json`。

- backboardd 连续十次 SIGSEGV，崩溃线程为主线程，位置为 Turnip `tu_init_hw<A7XX> +180`，空地址写入。调用链为 BeginCommandBuffer → bridge EncoderCore::open → blitCommandEncoder → QuartzCore warmup_shaders → 显示服务器建立。
- services86 新建第二个 Nmb/Vulkan device。当前实际 Turnip 后端 `tu_knl_darwin.cc` 使用进程全局 `dw`，枚举 `tu_knl_darwin_load` 时会打开、随后关闭这个共享连接。第二 instance 的枚举破坏第一 device 的 BO/connection，随后命令初始化写入失效资源。这是本项目上一轮呈现优化引入的回退，不是内核 panic。
- replicatord 的 IPS 是 QuartzCore `Unable to query displays from server` 后 abort；SpringBoard exit255 是同时期后续反应，不应单独禁用这些桌面组件。

## 实际修补

删除独立 Nmb presenter 路径，scanout 恢复复用已创建的真实 MTLDevice，并建立自己的 command queue。保留已有三个源 buffer/linear texture 的有界缓存、真实 GPU 完成通知、源帧引用和失败映射保留。未把 SIGSEGV 捕获后继续运行，也未移除 fence 或改成虚假完成。

后续如要独立 Vulkan 上下文，须先把 Turnip 的连接、BO、队列与 fence 生命周期移到 per-device 所有权，保证枚举不关闭其他 live device 的连接；仅更换 cache directory 不具备这种隔离。

## 同轮其他问题

- softwareupdated 本轮连续三次 SIGSEGV，IPS 位于 objc_release 读取地址0x20；同目录中更早的 IPS 也存在。Pong 未实现 Apple OTA 更新，板级 job policy 停用该可选后台守护任务并保留文件/定义，避免反复崩溃。没有禁用 replicatord、backboardd 或 SpringBoard。
- Pong QCA6490 完成实际首次扫描，15 个 BSS；不再将本轮问题归为 PCIe training 或固件加载失败。关联和 DHCP 尚无成功证据。
- 音频实体服务、host clock 和设备发现通知均成功，尚未见 StartIO；桌面恢复前不能认定播放已恢复。
- Backlight 本轮只完成初始 DBV32 回读，backboardd 在渲染 warmup 阶段退出，尚无本轮滑块写入证据。
- Sileo 日志包含累计旧引导输出，不能将无时间戳的旧 uicache 错误当成本轮桌面失败原因。

## 构建和安装

仅重建匹配 22H374 的 Metal 插件并应用板级 job policy；复用 services86 已编译内核和其他外设运行时。用户维护的构建脚本未修改，Data/SystemOS 保留。没有运行测试、基准、镜像哈希、镜像审计或设备回读比较。

120MiB OS 差量已收到设备写入完成回执；Data/SystemOS 保留。启动镜像为本机 `out/ipados18/boot-services87/pong-xnu-initial.img`，显示插件为 plugin-services87，保留 runtime-services86。Fastboot Sending 和 Booting 均为 OKAY（输出位于本机 `out/ipados18/fastboot-services87.log`）；未刷永久 boot 分区，设备留在运行状态。桌面恢复仍需设备反馈。
