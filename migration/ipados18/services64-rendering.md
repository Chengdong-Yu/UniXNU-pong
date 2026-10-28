# services64：桌面冻结与壁纸渲染

## 本轮依据

从 REC 读取 services63 的 pstore，并按目录提取 Data 中的两份崩溃报告。只读取日志和必要的 APFS 元数据，没有对镜像做哈希或回读校验。

- `backboardd-2026-10-27-035418.ips`：线程 17 在 Turnip `r2d_dst_buffer` 写到命令缓冲末尾以外，调用链为 `tu_CmdCopyBuffer2 → rows_copy → copy_from_texture_to_buffer → Presenter::present`。这是编码阶段的用户态 SIGSEGV。
- 该进程死亡时，scanout sequence 12 尚未确认退休。内核保留它的页面并阻止后续复制，重启的 backboardd 因而没有新的显示输出。保留真正未知 DMA 的隔离规则。
- `SBRendererService-2026-10-27-035508.ips`：CoreImage 的 `CIRenderDestination` 调用 `NBTexture isDrawable`，缺失方法导致 NSInvalidArgumentException / SIGABRT。
- 本轮内存日志为 4 个受管理银行、9728 MiB（内核预留前），8 个 CPU 已启动；不是一份新的内核 panic。

## 修改

1. GPU 缓存维护、WC 页面属性设置和重申，统一用 `pmap_valid_page` 检查真实的受管理页面。原来的 `gPhysBase/gPhysSize` 只覆盖主银行，扩展银行里的 BO 会被拒绝或漏维护；逐页检查也避免把物理空洞当作 RAM。
2. Turnip 的 buffer-copy 在编码一个完整的 R2D 步骤前预留命令空间，并把真实分配失败传给 Vulkan command buffer。Release 构建原先只在断言中处理预留失败，后续包会继续写入旧空间。本修改保留分配错误，不伪造 GPU 完成。
3. 普通纹理、IOSurface 导入纹理及其 view 的 `isDrawable` 返回 NO。它们不是 CAMetalDrawable 的呈现纹理。
4. 构建脚本纳入 Mesa 修补，使用 22H374 的 Metal ABI、iOS 18 部署目标和当前内核的 NativeAdreno ABI 重编译。

## Siri 与 Apple Intelligence

原始与已安装的 22H374 系统均有 Siri.app、AssistantSettings、assistantd、siriinferenced、modelmanagerd、generativeexperiencesd 及对应启动定义。按组件目录核对 118021 个文件的存在性，缺失为 0；没有比较文件内容或计算哈希。结果保存在本机 `audit-services63/assistant-inventory.json`。日志确认 assistantd 和 IntelligencePlatformComputeService 启动过；服务存在不代表在线请求、语音输入或生成式推理已可用。

日志中缺少 PairedSyncServices 的 Siri plist，但原始 iPad7,11 恢复镜像也没有它，不能补造一个同步服务。MediaConversionService 的两个 XPCService=false 也来自原版，不改成伪造服务来消除告警。

这份 A10/arm64 恢复环境并非 Apple Intelligence 的受支持平台。[Apple 对 iPadOS 18 的原始说明](https://www.apple.com/newsroom/2024/10/the-new-ipad-mini-is-available-today/)列出 A17 Pro 或 M1 及更新的 iPad。Pong 尚无兼容 Apple 模型的执行后端/ANE 驱动和已配置的模型资源，网络和音频链路也未完成。保留原版模型服务及其能力检查，不用机型标识或强制成功应答宣称已接通 Apple Intelligence。

## 构建与使用边界

保留 8 GiB UFS Data、多核、屏幕日志和现有显示参数。只更新 OS 中的 Metal 运行库及临时启动镜像；不刷写 boot 分区。未运行测试或基准，实际桌面与壁纸结果需本轮设备启动后确认。
