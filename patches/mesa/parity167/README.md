# A730 / 22H374 完整 Mesa 输入

`pong-darwin22h374.patch` 相对公开 Mesa 提交
`85fda3b36405bcaea8bffc64aa4f2651dc90770d`，包含当前实机配套源码的全部
Darwin 后端、A730 格式修正、OOM 处理、128 IB 批次、ABI1 优先级、真实 fence
发布和退休、导入所有权、帧记录及批量主机同步修改。

这是完整替代输入，不与 `parity114/` 或目录上层的历史增量补丁叠加。
历史补丁保留用于查阅每次修复。补丁只包含公开 Mesa/UniXNU 及 Pong 源码；
各文件的版权和许可证声明保留，不含固件、shader 二进制或 Apple 组件。

```sh
python3 scripts/prepare-pong-mesa.py \
  --repository /path/to/mesa-git --out /path/to/build/pong-mesa
```

再通过运行时 source overlay 中的 `scripts/build-pong-turnip.py`，使用
`--guest-build 22H374 --mesa /path/to/build/pong-mesa` 编译。该配方启用真实
IR3 磁盘缓存及 release/O3，不将旧的三个早期补丁当成完整运行时。
