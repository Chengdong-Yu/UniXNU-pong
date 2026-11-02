# services111：22H374关联请求、原生音频路由与上游Metal优化

## services110设备证据

用户确认Wi-Fi能扫描但不能连接、没有声音且界面卡顿。REC console显示实际
Pong QCA6490已完成固件启动和持续扫描，未见驱动join记录。ADSP已越过此前
stage9内存失败，APM、两颗TFA9873、音频物理端点和电池服务已出现。
CoreAudio可查询两个真实输出端口，rtcf外层返回0，但默认输出仍是物理设备，
VirtualAudio的vcsp=0，未见PCM启动。不能把服务注册等同于有声播放。

显示拷贝约0.86–1.07ms；GPU等待常为14–17ms，动画帧间隔约30–45ms。
这是实际启动遥测，并非新跑基准；不能仅凭八核高频请求认定性能正常。

## Wi-Fi

对照本地22H374 IO80211的`__performAssociation`：请求为0x384字节，
SSID长度位于0x10、SSID在0x14、BSSID在0x34、密钥长度在0x40、
密钥类型在0x44、密钥内容在0x4c。旧0x250结构的认证上下层是16位，
新布局为32位；旧解析会把认证字段读作SSID长度，在调用连接逻辑前拒绝。

新增明确的Darwin24布局及静态偏移约束，保留准确长度的旧布局转换。
校验长度、版本及已实现的开放/WPA2个人认证，不把不支持的认证降为开放连接。
32字节PMK仍传入真实station实现，临时密钥和用户请求副本清零。
状态变更请求的有界元数据日志不再受启动前96条查询额度影响；不记录密钥。
每次扫描的逐BSS打印从24条减至3条，扫描数据本身保持完整。
仍使用Pong modem_b提取的QCA6490固件；未替换为Sheng固件。

## 音频

保留实际物理端点优先和正确pibi/rtcf ABI。只在真实端口存在、rtcf成功且
返回有效路由描述后，将系统默认输出/系统提示输出设置为真实原生VAD，读取确认。
失败保留系统错误，不伪造音量能力；成功路由不再每秒重复写rtcf拆建音频图。
实际PCM开始、系统音量能力和扬声器声音仍需本轮设备验证。

## 上游Metal合并

参考组件已快进至NativeMetal ce5e19e33e05e1873d38d818e23cabfe0ac708c3、
NativeAdreno 5c8c1453c67d6ef6b10ec43ab1f8fc570a2f33e1。
UniXNU核心066c8d8和Sheng08cde20已fetch，本轮无新的主分支提交。
没有将独立metal2vulkan参考仓库的775条异步API更改整批覆盖到现用运行时。

- 合并31867dc的带参数设备能力查询缓存，缓存真实运行时结果，含拒绝结果。
- 合并ed654b4/2d35c57的写纹理行别名与16×4 tile store边界检查。
  替换旧的1080×2412几何例外，按每张纹理实际pitch、宽度、行数和分配长度判断；
  不满足边界时仍保留shadow，不取消所有回退。
- 合并461a842的Turnip GMEM/sysmem逐pass自动选择，保留nocb，
  不启用Pong尚无独立调度的并发binning。
- 合并5c8c145的提交优化：map/unmap已有表发布与TLB失效事务，
  不再每次提交重复TLBIALL；超时恢复路径保留。写合并属性修正仍每次执行，
  昂贵IB审计仅前16次，真实fence、所有权和非一致性缓存交接保持。
- 既有拒绝encoder对象、IOSurface导入复用和draw-path缓存已经在Pong中，
  保留Pong 4KiB、22H374格式策略、隔离客户端及设备地址。

重编译完整Rust运行时、Objective-C Metal frontend、插件及内核；不复用旧Rust归档。
用户修改的CI脚本不动。保留Data/SystemOS、亮度、八核、电池与输入法已有更改。
不运行测试、基准、镜像哈希或镜像回读。定时重启保持关闭。

## 交付记录

实际构建、安装、临时启动回执见本机out/ipados18对应services111目录。
构建与Fastboot成功不证明关联、PCM声音或流畅度已实机通过。

本轮Darwin24内核、完整Rust桥、Metal插件和配套运行时均编译成功。
OS差量152MiB写入完成，Data/SystemOS保持。Fastboot临时启动实际结果：
Sending 2.346秒、Booting 10.136秒，均OKAY。未写永久boot分区，未安排自动回REC。
