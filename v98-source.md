# v98 source set

This repository snapshots the Pong device drivers and userland adapter used for the v98 boot image. The kernel build used XNU 8796.141.3 / Darwin 22.6.0, iOS deployment target 16.0, AArch64 Pong, native 4 KiB pages and 8 configured CPUs. The v99 scanout kernel patch and v99 source changes are excluded.

The exact v98 XNU patch set is in `patches/xnu/`. It is the set recorded by the successful v98 build. `drivers/PongAdreno` and `drivers/PongMetal` come from the v98 source snapshot. Build configuration and dependency pins are kept in `configs/`, `feeds.toml`, `board.toml` and `unixnu.lock`.

This records a source/build configuration, not a promise that every optional device feature works. v98 reached the iOS main interface; Adreno/Metal acceleration remained unconfirmed.
