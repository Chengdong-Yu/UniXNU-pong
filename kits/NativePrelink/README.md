# iOS 16 PONG continuation

这两个工具从保留的 UniXNU 基线复制到设备目录，避免修改共享工具。它们只生成和静态检查候选。原输入 Apple 20G81 corecrypto 12.0 为普通 ARM64。iOS 16 的 sparse-index 源码补丁来自原工具的已有实现，已在固定 XNU 8796.141.3 上无 fuzz 应用。

`emit_prelink.py` 保留选中组件的原始可执行字节及原始 POST，不替换加密回调。指针绑定、PC 相对编码范围和段布局检查不等于完整 ABI 或运行时自检通过。
