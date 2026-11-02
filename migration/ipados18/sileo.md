# 内置 Sileo（22H374 开发环境）

当前 rootful 集成已继续补入真实 ElleKit 1.2 (`1.2+pong1` 本地集成版本)，详情和恢复方法见
[原生音频与 rootful 记录](services127-native-audio-rootful.md)。默认按真实应用进程名加载，
保留UI库、tweak plist过滤和safe mode；不是同名空库或假jailbreakd。
本轮保留设备生成的实际 firmware/Gestalt 包记录，不重写域名解析。

当前候选已按用户要求迁移 rootful，构建和实机边界见
[services113 记录](services113-rootful-adsp-gpu.md)。下文保留原 rootless 版本的
恢复方式；不要将其 `refresh_runtime --sileo-stage` 再用于 rootful 候选，否则会
重新安装旧版应用和首启任务。rootful 在 `refresh_runtime` 后调用
`userland/rootful18.py` 的 `install()`，最终覆盖为匹配的真实启动任务。

此接入使用官方 Sileo 2.5.1 `iphoneos-arm64` 包及 Procursus 官方 rootless bootstrap。Sileo 管理器内置于 OS 的 `/Applications/Sileo.app`；Procursus、APT、dpkg 与可安装的软件保留在可写 APFS Data 的 `/private/var/jb`。它是当前 Pong 开发内核的包管理环境，不需要 jailbreakd/libjailbreak，不替换 Apple 的系统库。

## 构建

```sh
python3.12 scripts/prepare-pong-sileo.py stage \
  --out out/ipados18/sileo-stage \
  --developer /path/to/Xcode.app/Contents/Developer

python3.12 userland/build-ipados18.py helper \
  --out /path/to/sileo-runtime \
  --developer /path/to/Xcode.app/Contents/Developer

sudo python3.12 scripts/prepare-pong-sileo.py embed \
  --out out/ipados18/sileo-stage \
  --prepared /path/to/prepared-22H374 \
  --runtime /path/to/sileo-runtime
```

`stage`下载并暂存到忽略目录，生成4KiB签名。可用`--bootstrap`、`--sileo`复用已下载文件。不会在macOS执行iOS的包脚本。提取器只接受`var/jb`内容，先处理常规文件，最后建立guest符号链接，避免跟随绝对链接写入主机。

`embed`只更新明确选择的本机OS/Data副本，保留原始restore和模板；保留旧init备份。Data已有`jb`时停止，不覆盖既有包环境。`--fresh-data`另建独立`PongSileoData18.sparseimage`，由原始模板、当前Data种子和Sileo组成，保留当前Data镜像。之后仍需按实际安装流程转换/安装该候选，不能把sparseimage存储文件直接写成手机块设备内容。

## 启动与接口

- 首次配置由真正的 launchd job 执行；不再在 PID1 交接前 fork，以免继承旧 bootstrap namespace。Data 没有 `.pong-sileo-staged` 时退出。
- job 等待原生服务初始化，准备Procursus账户数据库和现有维护脚本，配置Sileo的dpkg状态。执行OS上的`/pong/sileo-firstboot.sh`，更新脚本无需覆盖手机现有Data。日志为`/private/var/logs/pong-sileo.log`。
- 包配置使用`--no-triggers`，不让bootstrap中的旧uicache重新注册全部系统应用。`/pong/sileo-register`切换到mobile用户，通过22H374真实`LSApplicationWorkspace`注册Sileo，再核对有效的`LSApplicationProxy`和实际bundle路径。
- `.pong-sileo-configured`只在包配置成功后创建。注册每次启动都重新确认；`.pong-sileo-registered`只在代理确认成功后创建，不信任旧uicache的零退出码。失败保持可重试。不运行密码提示，不部署SSH服务。
- 新`giveMeRoot`使用普通XNU setuid凭据，限定调用方为安装目录中root所有、不可由普通用户写入的Sileo可执行文件；安装器设置root所有权和4755权限。
- Sileo 2.5.1 在现代 iOS 上直接请求 persona99 / uid0 / gid0。Darwin24 原生 `spawn_validate_persona` 拒绝非root调用者请求 uid0/gid0，因此不再通过注册器创建身份来试图绕过这一规则。`sileo_spawn.c` 仅在实际安装的 Sileo 进程中，将这组明确的 root spawn 请求交给上述 helper；保留文件操作、其它 spawn 属性、环境和命令返回。普通 spawn 和其它进程走原实现。
- 系统应用由原生 LS 枚举或注册器登记；persona 准备不会再阻止应用图标注册。代理有效且路径匹配才写注册完成标记，不伪造有效代理。
- 389个暂存Mach-O生成4KiB CodeDirectory页签名，保留原始XML entitlement；原生OS共享缓存路径仍为匹配的22H374 SystemOS。

本机原有Data副本挂载时被macOS拒绝，原文件保留。另建候选镜像，未因这个错误清空或修复旧Data。

最初 `services77` 的暂存与独立 Data 候选记录保存在忽略目录 `out/ipados18/services77-sileo.json`。后续已部署该包环境，但 services79 实机仍没有有效 Sileo 代理。services80 将管理器真正安装到系统 Applications，并包含原生 spawn 适配；运行结果继续独立记录。

已有 Data 的更新方式：编译 `helper` 后，以 `refresh_runtime --enable-audio-hal --sileo-stage <stage目录>` 更新本机 OS 副本，再通过跟踪的 OS delta 安装。此路径保留手机当前 Data，不重新执行 bootstrap 解包。

## 适用范围

Procursus官方归档路径使用`1900`，不是iPadOS18专版支持声明。已完成rootless路径、依赖文件、签名与提权接口的主机接入；Sileo界面、iPadOS18 LaunchServices、APT联网与实际安装/卸载尚需实机确认。当前Wi-Fi尚未有成功联网结果，源刷新可能需要后续网络适配。

Sileo使用原有APT信任和仓库签名规则；没有禁用仓库认证。仅安装包管理器不会自动提供tweak注入器、Substrate/ElleKit、SSH、越狱守护进程或所有插件的iPadOS18兼容性。

来源：[Sileo官方发布](https://github.com/Sileo/Sileo/releases/tag/2.5.1)、[Procursus rootless归档](https://apt.procurs.us/bootstraps/1900/)、[Sileo源码](https://github.com/Sileo/Sileo)。二进制留在忽略目录，Git只记录脚本和本项目适配源码。
