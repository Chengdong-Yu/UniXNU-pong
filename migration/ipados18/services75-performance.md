# SM8475 高频策略与提交开销审计

本轮仅修改本机源码并编译，未读取启动/REC日志、连接设备、更新系统镜像或临时启动，也未运行测试、基准、镜像哈希或回读。此前的接口与服务修补继续保留。

## 已修正的限速路径

| 路径 | 原行为 | 修改后 |
| --- | --- | --- |
| CPU EPSS | 每32ms读取8核空闲时间，按负载降频 | Darwin24默认固定高频请求；每500ms仅检查请求是否丢失，无空闲率采样 |
| CPU逐核DCVS | 只写域内第一个PERF_STATE | 硬件0xb0位0置位时，写齐该域实际4/3/1个核心的PERF_STATE，同时保留对应PDMEM perf-lock请求 |
| CPU worker | 启动入口缺少独占保护，失败映射未释放 | 启动入口原子去重；无有效LUT的映射释放；只访问固件已启用的域 |
| GPU GMU | 冷启动只请求285MHz、DDR索引2；没有后续升频策略 | Darwin24默认选择真实Pong DT档位0：900MHz、DDR索引11，经现有GMU阻塞ACK协议提交 |
| GPU ring发布 | 每次bootstrap/用户提交都清理整个ring | 根据本次命令、prologue与fence覆盖范围清理4KiB页面；环绕时清理两段，重叠时合为整块 |

GPU频率选择与PERF/BW表在同一处完成：Android降序档位0转换为HFI升序索引13，DDR使用该档位的bus=11。电压仍来自真实command-db的gfx/mx/cx依赖表，没有直接写GX PLL或复制A740/SM8850频率。无效档位在硬件初始化前返回。HFI失败仍按原有协议报错、隔离并保留DMA资源。

ring修改保留原生4KiB缓存维护契约、实际范围检查、WPTR前的屏障、BR/BV同步、GPU缓存刷新、fence与故障处理。一次写入不跨页时只发布一个4KiB范围；发生环绕时尾段、首段均在推进WPTR前发布。尚未测量收益。

## 启动参数与频率边界

| 参数 | 含义 |
| --- | --- |
| `pong_cpu_policy=2` | 默认固定最高允许的非boost LUT请求；E核上限1804800kHz，P核2496000kHz，Prime2995200kHz |
| `pong_cpu_policy=1` | 保留原有负载调频；逐核写入遗漏同样已修正 |
| `pong_cpu_policy=0` | 关闭本驱动的CPU频率请求 |
| `pong_gpu_pwrlevel=0` | Darwin24默认900MHz请求，DDR索引11 |
| `pong_gpu_pwrlevel=11` | 恢复此前285MHz请求，DDR索引2 |

`scripts/build-ipados18-prelinked-boot.py`为后续镜像加入CPU和GPU的默认参数，并保留命令行覆盖去重。CPU从当前固件LUT选择真实存在、core_count=4且不超过该域上限的条目；若没有这些条目，则跳过该域，不填造档位。GPU仍使用13个真实Pong档位和原有GMU协议。iOS16 GPU构建默认档位保持11。

这里固定的是性能请求。EPSS/LMh及GMU控制实际电压和时钟；没有清除硬件限频条件，也没有把DOMAIN_STATE瞬时读数声明为实测锁频结果。原有ACD/BCL/IFPC前置条件尚未补齐，本轮没有新增软件热管理或声称这些功能已经可用。频率和持续负载稳定性尚未实机确认。

## 依据

- NothingOSS SM8475源码固定提交 `c402fcdb3a1523d10b15ef1123bced45456f7592`：`qcom-cpufreq-hw.c`的EPSS偏移、4位core_count、PDMEM写入；`waipio.dtsi`的4/3/1拓扑与三个域的max_cores=4；`cape-gpu.dtsi`的13档频率/电压/带宽；`adreno_gen7_gmu.c`的升序HFI频率索引和GX/BW请求。
- [Linux EPSS逐核请求实现](https://github.com/torvalds/linux/blob/v6.18/drivers/cpufreq/qcom-cpufreq-hw.c)：只在硬件DCVS控制位允许时为域内其余CPU写入PERF_STATE。
- UniXNU-sheng的`package/CpuFreqProbe.cpp`提供实现对照；Pong沿用自身SM8475域、PDMEM、上限和core_count规则。

## 构建状态

完整格式2Darwin24内核已在本机编译通过，包含CPU、GMU、ring及此前的接口修补；输出为格式2构建目录中的`kernel/mach.development.pong`。启动参数脚本、JSON与porting TOML解析完成。结果写入忽略目录`out/ipados18/services75-performance.json`，对应内核记录为`kernel/PONG_BUILD_RESULT.json`、编译日志为`build-20261028T052017Z.log`。

本轮没有生成新的预链接启动镜像、更新手机或临时启动。实际锁频、温度、帧率和稳定性均未测量。
