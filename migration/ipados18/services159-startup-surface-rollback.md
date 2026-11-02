# services159：恢复已可启动的 IOSurface 分配

158用户报告启动冻结。REC原生日志保留GMU、CP、第一条硬件fence成功，以及cameracaptured创建真实Vulkan设备；在backboardd早期初始化阶段停止，未保留有效panic或SMMU故障。不能将其误报为Safari故障已修复。

撤回158新加入的全局MiniIOSurface WC/pageable backing以及相应物理aperture helper，恢复157已运行桌面的分配、客户端映射及缓存维护策略。158独立的Metal compute/render useResource(s)、useHeap(s)、scope/resource barrier接口保留；这些接口接入实际Rust驻留、地址翻译和引用，不再空返回。

未修改用户提供的构建脚本。移除末尾补丁触发了已有脚本基础补丁的反向处理问题，已经在生成的XNU工作树中保留修复前diff，并从记录的基础检查点、固定原版源码和仓库补丁重建受影响文件，保持所有功能补丁。之后构建记录明确显示基础补丁Already applied及后续补丁逐个正常应用。

159仅更新内核启动镜像，沿用已经增量安装的158 Metal库，不重新写OS/Data。仍是Pong A730、Darwin24、4KiB和ABI1；保留GPU scanout、已有异步编译/完成QoS、缓存复用、原生USB配对、Wi-Fi、音频和128GB Data。自动重启关闭，临时启动后保留手机运行。

实际设备状态单独记录，编译成功不等于桌面或Safari已正常。

## 设备实际结果

159 Fastboot Sending2.246s / Booting10.174s均OKAY。原生USB自动枚举Apple/iPhone，lockdown身份查询回复。第一段45秒CDC共680604字节，实际出现多次GPU scanout完成、1080x2412 BGRA swap连续推进，越过158启动停点。捕获内没有CP/SMMU fault或native connection refused。启动帧日志仍12–18fps，不能宣称性能问题解决；Safari新的间接资源负载仍待观察。
