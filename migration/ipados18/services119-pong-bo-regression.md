# services119：对照正常启动轮次修复 GPU 回归

## services118 的实际结果

用户确认未进入桌面并报告亮度异常。读取本轮 REC console/pmsg：A730 初始化 fence 通过，第一笔用户提交仍出现 BR/BV opcode error `0x2001`。services118 的空闲等待和映射属性修改没有消除此故障，不将其记为启动恢复。

前一轮新增的 IB 快照打印全零。进一步代码审计发现，无内核映射 BO 的 `peekWords` 依赖 `native_adreno_dram_read32`，Pong 并未安装该可选回调；调用返回失败后快照仍被打印。因此不能以全零推断真实命令内容。services119 记录读取状态，失败时不输出无效字。

## 与可启动路径的对比及修补

GPU parity 合并前，Pong 普通 BO 使用带内核映射的 wired 散页，显式 `makeWriteCombined`，提交前逐页检查并恢复实际 pmap 缓存属性。合并后的 task-less pageable system BO 不执行这一步，尚无 Pong/Darwin24 正常渲染证据。

恢复实际工作的分配/映射模型：普通 BO 使用 wired 散页，GPU IOVA 仍连续；只有调用方明确要求时才分配物理连续页。去掉该路径上额外预分配后又丢弃的 pageable BO。动态对象表、ABI2、128 IB、Metal/translator/shader 更新、进程 VM、真实 fence/PTE 撤销和 services118 的切表同步保留。

## 亮度对比

services112/113 与 services118 均成功发布 NT37705 背光，实际初始读数 `2048 / 4095`。services113 另有 `main queue wake completed backlight=1 lockscreen=1`；services118 没有到达该步骤。不能仅凭硬件初始化通过宣称滑块可用，也不能在图形服务阻塞时断言 DCS 传输已损坏。

对照保留的 services112 与已安装 services116 运行时，两个 CoreBrightness 适配函数的入口、逻辑、匹配数据一致；静态指令差异仅为同一字符串的布局重定位，目标字符串一致。本轮不替换这些已工作的函数，不关闭滑块或背光服务，先恢复正常渲染/主队列启动。

## 构建与边界

Darwin24 内核已构建成功，日志 `out/ipados18/kernel-services119.log`。本轮不修改用户维护的脚本、不重置 Data、不重写 OS/SystemOS、不恢复自动重启。临时启动结果在完成后另记；UI、亮度和 GPU 的实机结果仍需运行证据。

services119 Fastboot 临时启动 Sending 100112 KB / 2.372 秒、Booting / 10.127 秒均为 OKAY，记录 `out/ipados18/boot-services119/fastboot.log`。设备保留运行状态，不主动返回 REC。尚无进入桌面或亮度滑块恢复的确认。
