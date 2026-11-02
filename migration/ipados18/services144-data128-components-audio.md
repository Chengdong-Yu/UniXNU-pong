# Pong 128GB Data、软件组件标识与音频停启

## Data 实际迁移

从 REC 保存本轮现有 8GiB APFS Data，包含其真实文件和 UUID。使用 APFS 克隆保存原尺寸迁移源，再将独立 sparsebundle 的逻辑尺寸扩为 128,000,000,000 字节；APFS 物理存储扩大到 127,790,243,840 字节，卷 UUID 仍为 952D79E9-0CD4-47B2-B817-22889BF8AC50。

新 SHNGROOT Data 窗口从 userdata 内 40GiB 开始，与原 Data、OS 和 SystemOS 无交集。先对这个新范围执行 BLKZEROOUT，再仅传输 sparsebundle 的已分配 band，最后提交 4KiB SHNGROOT 目录。原窗口保留，用户文件未重建或清空。无镜像哈希或镜像回读比较。Mac 原生 APFS 扩容工具会执行其自身要求的文件系统元数据检查。

新增 `scripts/expand-pong-ufs-data.py` 完成带断点记录的迁移；新增 `scripts/install-pong-sparse-windows.py` 支持稀疏 Data 输入，原有用户安装脚本保留。

## MobileGestalt

保留当前 Finder 配对使用的稳定 UDID 和板级序列号。按用户请求，为主板、电池、前后相机和面板添加持久的软件组件标识，使用 PONG 前缀并在配置中明确标记 synthetic。二进制与字符串问题按各自类型返回，不声明相机、蓝牙或基带已经实现。这些值不能修改爱思服务端的苹果出厂数据库。

DiskUsage 的 TotalDataCapacity/Available 使用实际 Data statfs；TotalDiskCapacity 改读真实 Pong Data IOMedia 的 Size，不再继承旧的 5GiB 答案。

## 扬声器审计和修补

直接读取实际 22H374 CoreAudio 的 `AMCP::IOAudio2::make_level_map`，确认它遍历所有 range-map 项，累计 integer steps 与 32.32 dB 值。16 段曲线没有把最大音量限制为 1；本轮保留其 0..16 范围、S16 到 Q31 的显式转换和最大单位增益。

对照 Nothing SM8475 源码的 TFA9873 B0 定义：32bit stereo 的 TDMNBCK=2、slot length=31、sample size=31；Pong 左右功放分别从 slot0/slot1 取音。保留原厂 CNT 增益、MTP 和保护设置，不通过提升功放增益掩盖问题。参考 [Nothing 的实际音频实现](https://github.com/NothingOSS/android_kernel_msm-5.10_nothing_sm8475/blob/c402fcdb3a1523d10b15ef1123bced45456f7592/vendor/qcom/opensource/audio-kernel/asoc/codecs/tfa98xx/tfa_dsp.c)。

停止输出原先立即停止 DSP 并关闭功放，可能丢弃已接收但尚未播放的短提示音。本轮先停止新提交，最多 100ms 处理真实 WRITE_DONE，再保留最多 80ms 已有物理队列尾部。错误停止不等待尾音；没有生成测试音或新 PCM。进入停止状态先撤销旧 Running 标志，避免新的 StartIO 误用正在关闭的旧图。

构建成功不代表扬声器已实际出声；设备容量、配对和播放结果在临时启动后独立记录。自动重启关闭。

## 后续设备结果

已完成 128,000,000,000 字节 Data 窗口迁移。配对后的原生查询返回 `TotalDiskCapacity=128000000000`、`TotalDataCapacity=127790243840`，可用空间约 125GB；原有 Data 文件保留。Finder 的原生连接、配对验证和会话启动均返回 0。

用户确认能听到较小的锁屏音效，媒体及其他声音仍未恢复正常；后续转入 services145 的原生类别音量修复。此结果只证明已经有实际声音输出，不代表所有播放场景完成。
