# PongPlatform24

The Pong SM8475 platform aggregate: Darwin 24 XNU with the A730, QCA6490, UFS,
input and power work. The sources are the device repository's `drivers/`,
`patches/` and `scripts/`; this directory only carries the component build
recipe that the UniXNU component catalog names.

`build-kernel.py` runs `scripts/build-pong-profile-kernel.py` of the device
repository the kernel step was started from, which is `UNIXNU_DEVICE`. It writes
`kernel/mach.development.pong` under the step's `--artifact-dir`.
