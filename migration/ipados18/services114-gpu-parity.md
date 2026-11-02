# iPadOS18 GPU parity 合并记录

来源为 UniXNU `feat/ipados27-gpu-parity` 的 `87bf1f9`；完整依赖提交在
`configs/pong-gpu-parity.toml`。目标仍为 Pong SM8475 / A730 / 22H374 / 原生4KiB。

## 合并与板级适配

- Adreno ABI2 的128个IB提交、按需增长的对象/队列/作业表、系统内存BO、锁外准备页面、导入别名检查与内存压力反馈。
- 每个连接独立的GPU页表和ASID。采用Pong已初始化的bank0及四条SID路由，保留32位输入地址宽度；客户端从0x20000000起获得自己的地址空间。
- TTBR0使用等待GPU空闲后的CPU切换。未借用Sheng的安全监控调用结果或CP aperture配置。
- 保留A730的SQE固件、pre-ZAP无fence bootstrap、实测电源/时钟配置、扫描输出连接及实际映射撤销规则。
- ArmSmmu4K按需页表、批量映射和TTBR切换接口。Pong的显式TBI、共享ASID和轮询故障策略保留；未加入宽泛的寄存器匹配豁免。
- Metal前端的系统编译器、Core Image内核库、链接函数、曲面细分、PVRTC解码、pipeline reflection、独立编码线程、IOSurface导入保留/修剪、实际shader cache及遥测。
- translator更新到624bab39，并应用功能分支20个补丁及Pong的22H374格式描述ABI。运行时仍使用真实Vulkan能力判断是否能创建资源。
- Turnip BO缓存、红黑树GPU VA分配、内存压力回收，以及对导入纹理边界使用精确texel的GMEM load/store。
- 新增half-float scanout转换AIR及公共显示转换表，Pong稳定的BGRA扫描输出路径保留。
- shader priming只编译实际guest metallib，空闲时进行，有单次预算及退出开关；不运行benchmark或合成绘图。

## 平台差异

iPadOS27的arm64e PAC thunk和QuartzCore固定地址不用于22H374。当前guest仍采用
`PongMetalAbi.h` 中的600字节device ABI及已适配的任务归属、线性纹理布局、颜色矩阵与
CPU/GPU ownership handoff。其他DRAM bank在BO别名位图外时使用完整检查，不错误地当作无别名。

Darwin24没有上游可选的`native_pmap_user_mappings`诊断导出，因此该诊断后端不可用；
客户端映射实际撤销、BO引用计数、DMA/fence和失败保留逻辑没有移除。
GPU重启/replay的公共接口已合并；Pong尚未提供经过设备确认的板级restart回调，不能据此宣称自动恢复已生效。

## 同轮运行时修补

原System角色的OS卷改为普通未密封APFS卷，保留UUID及内容，允许原生rootful remount路径继续尝试；
独立SystemOS维持只读。音频路由使用22H374 MediaExperience真实回调及物理端口转换，不伪造音量能力。

## 设备验证

尚待本轮匹配内核、Turnip及Metal库安装后自动临时启动并抓取REC pstore。
主机构建成功不代表设备渲染、音频或rootful安装已成功。

## 主机构建与自动回收配置

Darwin24内核、LTO Metal bridge、插件和Turnip均完成编译链接。
为Turnip固定源码构建ID，避免同一编译器因安装时间变化而使持久pipeline缓存失效。
services114-auto镜像使用8核和UFS Data；自动回REC时限240秒，id为`services114`，
由`scripts/capture-pong-boot.py`读取现有`/sys/fs/pstore`路径。
Fastboot本轮Sending 2.324秒、Booting 10.172秒均为OKAY。设备结果待回收。
