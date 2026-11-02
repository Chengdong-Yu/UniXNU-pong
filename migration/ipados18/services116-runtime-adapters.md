# services116：22H374 运行时适配

本轮复用 services115 已确认初始化成功的 GPU 内核与 rootful OS-only APFS 适配，安装匹配的用户态后临时启动。自动回收保持 240 秒、目标 REC、原 pstore 路径，无永久 boot 分区写入。

## Metal

真实 CP 初始化 fence 与 ABI/芯片/故障状态仍是必要条件。去掉将计数器零值直接当作硬件失效的判定：计数器读成功与完成的 CP fence 分别提供接口/启动依据，零值本身不能证明故障。其他前提拒绝时记录字段以确定实际原因，不掩盖故障。

## 音频

保留已解析的原生 setter，避免后续调用覆盖回通用 HAL setter。使用精确 22H374 的 `vaemSetRouteConfigurationDictionaryOnVAD`，由 MediaExperience 自己使用已初始化的内部插件身份并处理响应；物理端口仍按实际 UID 选择。仅在 guest build/函数 ABI 匹配时接入，拒绝和音量能力不足仍返回失败，不伪造可用音频。

## rootful

- 根据已确认的 Data 种子完成状态继续配置，不清空 Data。
- 系统版本的 dpkg 虚拟包由实际 guest `SystemVersion.plist` 生成，仅记录 OS 18.7.10 / 22H374，不添加硬件能力。
- 包配置与注册不被全量硬件能力查询先行阻塞。原 firmware 工具仍运行，有界执行并保留错误；失败下轮重试。
- 配置完成标记属于 OS，而非跨 OS 重建保留的 Data。
- 根卷已可写，不能对发生 guest 写入后的 APFS 直接叠加旧主机增量。本轮完整写入 OS；首次启动前的补充元数据采用同一未启动基线的小增量。Data/SystemOS 保留。

## 设备结果

以 `logs-services116-auto/capture.json` 和对应 console/pmsg 为依据，回收后补记。主机编译、文件写入及 Fastboot 成功不等于实际渲染、播放或安装成功。

## services116 设备证据与后续修补

自动回 REC、console/pmsg 本轮标识、同步重启提交均确认。内核 GPU 完成真实初始化，但用户态得到“成功且全零”的查询。随后定位到合并时 runCore 普通分支遗漏真正的 `core_->dispatch`，状态未初始化且输出未填写。services117 恢复真实分发，并把 GPU C++ 的未初始化变量/缺失返回编译诊断设为错误，避免此类错误继续生成镜像。

Sileo 已在设备上完成 `dpkg --configure org.coolstar.sileo`，架构为 iphoneos-arm，移动用户的原生 LS proxy 有效且指向 /Applications/Sileo.app。APT 使用真实已播种索引；网络刷新目前报 apt.procurs.us DNS 解析失败，没有伪造成功。原 firmware 工具仍在全量能力查询中阻塞，限时终止后不会阻塞 Sileo；实际包安装操作尚无证据。

音频 rtcf 的属性尺寸32字节及可写性都被真实 HAL 确认。调用内部路由函数后错误变为 -12986，但音量能力仍0；不能宣称 PCM/声音已接通。

同一审计还补齐了采用 Pong 外部上下文后的三个观察入口：observeContext/readTtbr0 使用实际 SMMU，probeBuffer 使用实际 scratch。原生自建 bring-up 所有者路径不变。
