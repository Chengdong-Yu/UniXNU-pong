# services69：Pong WLAN 电源确认与 PIPE 时钟交接

services68 的 REC console 保存在本地 `audit-services68/pstore/console-ramoops-0`。Wi-Fi 两次训练都停在 DetectQuiet，ELBI=1，PHY=0x86，DLL 未激活。HAL 日志已出现 AudioServerPlugIn 工厂、Initialize 和物理立体声服务连接；未据此宣称有实际声音输出。

对照 UniXNU 中文 network 文档、Sheng WifiProbe 与 Pong REC 的 live FDT，82 项 PHY 表、PCIe0 资源、GPIO80/94/95/96/204、GCC 时钟、复位和 collapse vote 均使用 Pong 的 SM8475 值。此次修改：

- PIPE 在 TCXO 父时钟下启用并完成主机/PHY 复位及表项写入；到 Pong 表中 PCS SW_RESET=0 前切换到 PHY 输出，然后写 START=3。此前 PIPE 在外部父时钟下停到表项加载结束，与 Sheng 的完整 QMP 交接流程不同。
- PARF PHY_CTRL 仅清 PHY_TEST_PWR_DOWN bit0。PCS 的 SW_PWRDN/REFCLK_DRV_DSBL 属于 PHY+0x240，不能套用到 PARF。
- 沿用 Pong 的 SPMI v7 只读 observer，按本机 PMIC 的实际 PPID/type/subtype 检查已确认的 WLAN 电源。RPMh 请求完成后，等待 enable、in-regulation 及电压下限。AON smpe2 为 SID4/page9e/type1c/subtype0e；其余 buck 类型为03/0a。共享 analogue LDO 与天线 LDO 的未知 PPID不作猜测。
- 电源未就绪即停止后续 PCIe MMIO/训练，保留普通系统启动；两次链路尝试仍有明确超时。

本轮构建只更新内核和启动镜像。沿用 services68 已安装的 OS/HAL，保留 8 GiB UFS Data。使用 format2 构建和 Pong 硬件 DT；按用户要求跳过镜像哈希、镜像审核、设备镜像回读及基准。临时启动结果和物理电源/PCIe结果需在本轮设备运行后记录，未把编译完成等同于 Wi-Fi 可用。

构建与交付结果：Darwin24 内核及预链接镜像构建完成，临时镜像100112 KiB；Fastboot Sending 2.335s / Booting 10.127s 均 OKAY。未写 OS、Data 或 boot 分区。等待本轮 REC 电源及链路日志确认硬件结果。
