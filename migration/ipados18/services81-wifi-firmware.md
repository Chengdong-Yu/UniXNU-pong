# Pong Wi-Fi 固件与注册顺序修复

## 设备日志

services80 的 REC console 中，PCIe0、SID1c01 native4K DMA、MHI/QMI 已完成。
QMI 返回 chip `0x12`、board `0xff`、SoC `0x400c1211`，加载 `amss20.bin`、
`bdwlang.elf`、`m3.bin` 和 `regdb.bin`。驱动登记了 102 个通道，紧接着进入
`MHI_CB_EE_RDDM`，重复通知持续到 90 秒初始化截止。日志没有证明扫描或联网成功。

## 修改

- 提取脚本读取 Pong 的实际槽位；本轮为 `_b`，只读提取 `modem_b/image/qca6490`。
  固件与来源记录保存于本机忽略目录，不进入 Git。此前脚本固定提取 `modem_a`。
- 构建器只接收具有 Pong 当前槽位提取记录的固件目录；打包明确列出的无线二进制及
  `bdwlan*` 板级数据，排除来源 JSON 和调试数据库。QMI chip/board 与 TCSR major
  仍决定具体文件，不借用 Sheng 的 WCN7850 固件。
- mac80211 的 registered 回调只保存通知。MAC start/add_interface 移到 QMI bring-up
  工作返回、`ATH11K_FLAG_REGISTERED` 已置位之后，避免在尚未完成注册的驱动里重入。
- 仅在实际 station 建立后发布网络接口；分配了 vif 但 add_interface 失败不再视为可用。
- MHI SYS_ERROR/RDDM/FATAL_ERROR 首次出现即标记失败，阻止继续发送 WMI。板级停止
  dispatcher、屏蔽 MSI、拉低 PERST 并保留 DMA 映射，正常 iOS 启动继续。
- 已发布接口的扫描/加入请求会收到结束事件，后续请求返回 ENETDOWN；不会把崩溃
  固件当作仍在扫描。保留最后八条 WMI 命令编号用于定位真实固件拒绝的命令。
- 去掉共享固件加载器中无必要的校验和循环，遵守本地不校验镜像的要求。

## 验证范围

固件已从连接的 Pong 当前 B 槽提取。完整 Darwin24 内核编译及预链接打包完成。
services81 临时启动：Sending 100112 KB，2.320 秒；Booting 10.174 秒，均为 OKAY。
没有改动已安装的 services80 OS/Data，没有刷写永久启动分区，也没有在启动后主动重启。
构建记录在本机 `out/ipados18/build-services81.log`，固件来源记录在构建目录
`build/pong-wifi/firmware-source.json`，启动结果在 `out/ipados18/boot-services81/fastboot.log`。
这些结果尚不能作为 Wi-Fi 设置不卡死、扫描或联网成功的证据，需本轮实际现象及日志确认。
