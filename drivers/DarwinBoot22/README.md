# DarwinBoot22 for Pong

This device-local copy targets XNU 8796.141.3 / Darwin 22. Its boot_args command
line is 608 bytes, memSizeActual is at 0x2d8, and the structure is 0x2e0 bytes.
The older shared DarwinBoot source and saved firmware volume are preserved.

Build with `../../scripts/build-darwinboot22-macos.py` from the device project.
The script builds only this loader in an independent workspace and exports the
EFI binary, log and build record under `mac-build/darwinboot22/`.

An explicit `[[driver]]` entry injects the loader from the temporary FAT payload.
It replaces one matching loader protocol in RAM and restores the older interface
on unload. The MMU-off transition is position-independent code copied into an
extra page within the allocated kernel window. That page is included in
topOfKernelData. No firmware or device partition is written.

The c5c0cba0497729b10699bd23999210700cd22548e2ac5673779be8de8f10456b binary passed
the own diagnostic kernel's window test on Pong. Full XNU progress is tracked
separately in the handoff's evidence directory. The source later corrects the
green-bar message to describe loader entry, without claiming payload execution.

The copied Test fixtures are retained as reference material; their memory-size
offset was updated for Darwin 22. They are excluded from this standalone build
and were not rerun in this Mac round.
