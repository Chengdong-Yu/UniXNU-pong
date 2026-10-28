# Data A Live 优化来源

0012、0013、0015–0019 从用户提供的 `data-a-live-cpu-gpu` 源码包移植，原项目 ShueiTsuki/data-a-live（包内标注 a417358）。保留补丁内原始作者和变更内容，适用原 metal2vulkan 许可及包中 MIT 许可。归档原作者说明见 `out/ipados18/services55-reference`。本分支仅导入运行时生产代码，不添加/运行包内的测试、采样器或自动滑动程序。

适用于22H374/SM8475/A730的独立适配：新增可协商的method17/128IB接口、保留method5/16IB接口；不修改A730真实芯片号、不复制SM8850/Adreno840寄存器、固件、SCMI通道或温度常数。padded Alias策略未启用，保留影子图像及边界规则。
