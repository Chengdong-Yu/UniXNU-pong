# services110：ADSP启动内存、壁纸Metal名额和原生输入法

## services109实际回归

用户确认壁纸、电池与输入法仍不可用。REC保留109启动参数（capture_seconds=0），
首帧与持续GPU提交存在；此为手动回REC，不称定时抓取成功。

- ADSP实际失败于stage9、error=-12。之后音频等待GLINK超时，ASPL connected=0，
  电池物理服务未发布；不是有容量却只丢了状态栏图标。
- SBRendererService的040404 IPS在NUDevice_iOS _newRendererWithCIContextOptions:error:断言。
  对照22H374实际代码，断言是Software renderer is unsupported；
  initWithName未取得默认Metal设备。console同时有native connection refused e00002d5。
- GPU板级客户端只有4个独立窗口。Neutrino已缺Metal设备时，CIContext nil时的回退
  根本不会走到，不能用该回退称壁纸已修复。
- 键盘偏好保存成功，REC的AppleKeyboards确有拼音/英文/emoji，kbd也运行，
  所以此前仅写列表的修复效果不成立，用户结果优先。

## 实际修改

### 电池、音频共同依赖

在ADSP probe创建工作线程之前准备64KiB物理连续PAS元数据与256KiB固定页固件bounce，
避免根卷到达后再与大型GPU/WLAN分配竞争。实际Pong元数据5884字节，仍按解析结果
检查容量、物理连续extent、4KiB和36位物理边界，不截断或绕过安全固件认证。
所有成功prepare的临时descriptor配对complete，启动失败与成功均释放未消费scratch。
原有固件分段拷入Pong保留区，首个失败步骤记录准确来源，不复制Sheng地址。
充满标志使用PMIC真实status=4，不以显示100%推断；源状态去抖时同时约束charging/full，
避免无外接却full或充电与充满同时为真的冲突。容量来源保持实测，不虚构服务或电量。

### 壁纸

真实GPU进程名额4→10，在已公布的0x20000000..0xc0000000范围内保持地址不重叠。
首进程1GiB及第二进程512MiB保留，其余1GiB为8个128MiB窗口。
范围仍受32bit输入约束，queue作用域、对象所有权、独立操作锁、真实fence/DMA退休和
隔离失败slot的规则保持。辅助进程可用VA仍有限，不宣称无限名额或完整壁纸效果通过。
不把Neutrino Metal判断硬改true，不取消它的真实断言，不以空context冒充渲染器。

### 系统输入法

保留109的Consumer-only PMIC报告和准确12/1匹配，不重新声明PC文本键盘。
在主队列、dyld加载完成之后调用22H374 TIPreferencesController updateInputModes，
由系统注册语言、更新缓存与发原生通知；读取已存在的用户列表而非覆盖其选择。
调用系统setKeyboardShownByTouch偏好和synchronizePreferences，不伪造输入法控件。
方法参数、返回类型和存在性在运行时匹配后才调用；键盘可见性仍待用户实际测试。

## 构建和保留内容

Darwin24内核及配套运行时编译完成。Wi-Fi原生握手、已确认的亮度、PowerUI注入、
音频插件ABI/默认UID优先、Pong固件、八核与Data保持；不再次重置Data。
原自动重启参数保持0，临时启动后留在系统，不运行REC循环。
上游主分支此前已fetch，无未合入的新提交；用户CI脚本保持。
未运行测试、基准、镜像哈希或镜像回读；编译与Fastboot结果不当作功能通过。

## 临时启动回执

OS差量120MiB安装完成，Data/SystemOS保留。Fastboot Sending/Booting均OKAY；
实际耗时以out中的temporary-boot.json为准。定时重启关闭，设备留在系统；
壁纸、电池、键盘和音频效果待实机确认，不以此回执认定功能完成。
