services139: physical Apple USB mode negotiation + kernel-origin native sockets
- Observed138 Apple/iPhone05ac12a8 enumeration480Mbps, but native host PreferredConfig0xc0/0x45 timeout.
- Actual host driver disassembly confirms four bytes host/device/default/extended function. Reply1/1/1/0 selects the one existing physical CDC+Mux configuration.
- DWC3 EP0stall corrected to physicalEP0 (Linux dwc3_ep0_stall_and_restart), not direction-selectedEP1.
- Native22H374lockdowndsocket_accept_callback checks TCP_PEERPID0 on loopback before accepting USB origin. Added bounded in-kernel socketKPI backing through root-only generation-checked transport; service pairing/TLS unmodified.
- USB descriptor refuses MG all-f/all-zero placeholders; fallback remains board identity, full UDID parity still pending.
- No framebuffer/WLAN/power/HID/audio routing or user build scripts modified. SD1speaker actualAMPS/SWS fix137 retained. Automatic restart off.


Source inspection also found a disconnect error path where a failed IOKit receive could leave the caller's output capacity unchanged. The final services139b bridge zeros returned length on IOKit failure; only real socket bytes can enter a USB reply. Both daemon compilations use -Wall -Wextra -Werror. No native lockdownd request is answered by the bridge itself.

services139 final kernel and prelinked boot package built successfully. Full matching OS transfer is in progress; Data/SystemOS unchanged. The final139b bridge-only band update will follow this full transfer before any guest boot. Automatic reboot remains disabled.
