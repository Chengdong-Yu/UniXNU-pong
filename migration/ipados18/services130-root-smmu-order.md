# services130：根存储 SMMU 接管顺序与音频失败恢复

## services129 日志

REC 读取的 console/pmsg 共 8990 字节，停止在 UFS 的 SID e0、firmware bank 2 → target bank 74 配置阶段，以及 Adreno startup requested。没有 panic、APFS 挂载、launchd、APM、TFA 或 PRM 时钟结果，不能将本轮卡死归因于音频 PRM 拒绝或确认音频成功。

## 启动与 USB

代码审计发现真实竞争：UFS 原有 hostBringUp 没有取得共享 Apps-SMMU lease；可选 USB 在平台 start 中创建 worker，services129 解决低地址分配失败后可与根存储同时选择空闲 bank/SMR，再分别写入。选择器中的“空闲”检查无法保护两个并发选择者。这个缺口与新 USB DMA 路径及日志停点相符，但保留日志并不足以证明具体最后一条 MMIO 指令。

- UFS 在 bank/SMR 规划、读取状态和配置写入期间取得同一共享 lease，所有返回路径释放。队列命令、等待 LUN 和 ready 通知都在锁外。
- UFS 对可选 power aggregate 的锁函数使用 weak imports，保留不包含该 aggregate 的存储单独构建方式；本配置仍链接真实共享锁。
- USB worker 先在自身线程有界等待真实 pong_ufs_ready；没有根存储就不做可选控制器/SID0 接管。它不持锁等待存储、不阻塞平台 start，也不禁用已实现的 USB 传输。
- USB、音频保留不重叠的 UEFI DMA 分区；SMMU bank 仍由实际占用状态动态规划，不能把所有功能都硬编码到 bank74。

## 音频

保留 services128 的单一 PCM 所有者、真实 committed samples 和 DSP WRITE_DONE 路径，以及 services129 的 tertiary MI2S 0x104 / 3.072MHz PRM 申请/释放。

新增 last-start-failed 状态位（共享布局、版本和大小不变），原生 start 等待能识别失败。清理前保存首个错误；清理 ACK 不覆盖它。仅在功放和图均已证明停止后，保留真实设备并允许下一次显式 CoreAudio start 重试；取消当前请求，避免自动重试风暴。若不能确认 graph/DMA 退休，仍撤销 readiness 并保留缓冲，绝不伪造可用或复用未知在途 DMA。

## 构建与部署

主机构建遇到开发盘空间不足，两个早期版本的独立临时传输 chunk 移至本机 Pong-old-host-temp 保留；没有删除当前 OS、Data、固件或源码。用户维护的构建脚本不修改。最终启动镜像输出到本机 Downloads，现有 services128 OS 保持；MYMC3LL/A、固定展示序列号、Wi-Fi、亮度、按键、GPU 组合和 rootful 环境保留。自动重启关闭，不运行镜像哈希、读回比较或性能基准。编译及临时启动结果在完成后补记，实际音频/USB/桌面效果分别记录。

Darwin24 最终内核编译成功，匹配 prelink/启动镜像已构建至 /Users/desktop/Downloads/Pong-build-services130/pong-xnu-initial.img。临时启动结果：

```text
Sending 'boot.img' (100112 KB)                     OKAY [  2.321s]
Booting                                            OKAY [ 10.168s]
Finished. Total time: 12.591s
```

没有写入 OS/Data/SystemOS 或 boot 分区；设备保持运行。是否恢复桌面和实际音频/USB 效果仍需本轮设备结果，不能用 Fastboot OKAY 代替。

启动后约54秒主机观察：没有发现 Pong CDC/USB modem，也没有 Qualcomm 900e/9008 设备；没有取得实时 USB console。USB 枚举仍未确认成功。设备保持运行，不自动返回 REC。
