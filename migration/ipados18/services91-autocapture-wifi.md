# Pong：定时返回 REC 抓取日志与 Wi-Fi 菜单冻结

依据 [UniXNU 日志抓取文档](https://unixnu.com/porting/log-capture) 的持久化读取器、返回路径、
单轮运行记录与有界超时流程。继续使用本机已经多次完成的 REC/ADB + pstore 读取方式。

## 上一轮实际日志

services90 的 REC console 已保存到本机 `out/ipados18/logs-services90/pstore`：

- 六项物理 WLAN 电源检查通过，PCIe0/SMMU/MSI、MHI/QMI 均完成；加载本机活动 B 槽
  QCA6490 固件和实际 BDF，station 已扫描到 13 个热点。
- 原生 Wi-Fi 用户客户端完成版本、能力、角色、父接口和电源请求。日志未出现菜单操作
  同步等待固件的证据；显示冻结后电源 GET 仍在返回。
- Preferences 出现后，CP BV 再次发生 opcode fault（cause=0x2000，packet=0x70970001），
  GPU copy sequence98 未完成，之后 Metal DeviceLost 和显示服务退出。
  因此不能把菜单冻结仅归因为无线固件失败。
- 上一版充电订阅已得到确认，日志真实显示 external 0→1、VBUS 约5V、online 0→1，
  随实际充电状态变化更新 IOPMPowerSource。上一版相关修改保留。

## 自动抓取

`drivers/PongBootCapture/PongBootCapture.h` 通过 Pong ArmCpu 的启动点接入：

- `pong_capture_seconds=0` 或省略时完全不启用。有效诊断时间为30–3600秒。
- `pong_capture_target=1` 返回 REC；2 返回 bootloader。`pong_capture_id` 标识本轮。
- Pong REC live DT 确认 PMK8350 SID0 SDAM 0x7100 的 offset0x48、bits<1 7>，
  SPMI v7 APPS EE0；IMEM 对应0x146aa65c。编码来自 NothingOSS qcom-reboot-reason.c
  和 msm-poweroff.c：recovery=1 / IMEM0x77665502，bootloader=2 / IMEM0x77665500。
  写入前查找实际APID并检查owner，保留SDAM bit0。没有借用未核对的Sheng参数。
- 已存在返回线程后设置目标；原生 `reboot_kernel(RB_QUICK)` 在定时点执行文件系统sync，
  不使用RB_NOSYNC。Pong平台回调只在此诊断模式接管restart，优先PSCI RESET2 warm reset。
- 正常shutdown若被阻塞，独立线程在30秒后尝试物理返回并写明原因。
  这不是独立硬件watchdog；所有CPU停止调度或完整内核死锁时仍可能无法返回。没有照搬
  尚未在Pong核对调用约定的Sheng/Gunyah看门狗，也未绕过真实panic记录。
- 阶段与boot ID直接写入原有console/pmsg，不经过可能在shutdown持锁的IOLog。
  console为循环区，pmsg保留首次错误；不改变原有reserved RAM布局和REC文件名。

`scripts/capture-pong-boot.py` 是新增脚本，不修改用户维护的既有构建脚本：

```sh
python3 scripts/capture-pong-boot.py \
  --boot-image out/ipados18/boot-services91/pong-xnu-initial.img \
  --out out/ipados18/logs-services91 --boot-id services91 \
  --serial dd72e429 --adb /path/to/adb --fastboot /path/to/fastboot --port 5038
```

单次临时启动→等待定时返回→确认REC→拉取 `/sys/fs/pstore` 至原有 `logs-*/pstore/`。
`capture.json` 保存镜像路径、设备、当前阶段、REC boot ID、重启提交标记和错误。
文本boot ID用于区别新旧日志，不进行镜像哈希或镜像回读。超时保存部分证据，不循环重启。
默认最后留在REC；`--return-to bootloader` 可在读完日志后返回FB；
目标为FB时可用 `--reader-image` 临时启动已经准备好的读取器。
后续普通镜像省略capture参数即可保持运行，不会按时间自行重启。

## 菜单冻结修补

1. 保留上一版IB列表标记，在用户IB到内核fence的边界同步BR/BV，IFPC结束后按照Nothing
   gen7 ring返回BR空闲状态。继续GPU渲染，未隐藏设备丢失、未提前回收未完成BO。
2. 原生Apple80211电源/断开命令原本在gLock下直接发送网络事件，可能与回调接口查询产生
   锁重入。现在只在锁内记录可合并状态，由预先分配的thread_call在释放锁后发送。
   扫描、关联、RX与原有完成事件路径保留；不移除Wi-Fi开关、不以假成功代替真实错误。
3. 八核/X2排序、SPMI供电重试、电池订阅、原生音频/亮度/HID入口均保留。本版内核更新，
   不重置Data或修改已安装OS、Metal前端。

## 构建及设备结果

首次链接因稀疏开发盘的宿主磁盘不足，strip输出失败。只清理可由保留的最终镜像重新生成的
旧payload FAT、ramdisk和stage中间文件，源码、内核、最终镜像及日志保留。
Darwin24驱动及内核随后编译/链接完成；未运行镜像哈希、镜像回读或性能基准。
services91打包为8核、180秒REC诊断镜像。Sending 2.359s、Booting 10.170s 均 OKAY；计时后完成文件系统同步、记录同一 boot ID 和重启提交标记，成功返回 REC 并拉取 console/pmsg，capture.json success=true。RESET2 返回不支持后，标准 PSCI SYSTEM_RESET 成功。该轮未见旧 BV opcode 故障，但出现纹理 OutOfMemory(Device)。QCA6490 完成 10 个热点扫描，未见原生界面扫描/关联；音频发现完成仍无 StartIO；亮度无 SET；音量报告设备未建立。软件定时器不能覆盖调度器完全停死的情况。
