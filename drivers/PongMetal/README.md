# Pong Metal / iOS16.6.1 development

This is a local fork of Sheng's NativeMetal frontend and object bridge over
metal2vulkan and Turnip. The libraries compile and link; **no Metal device is
registered and actual Pong GPU execution is not yet confirmed**. This is not
an automatically exposed hardware acceleration plugin yet.

## Actual adaptations

- `PongMetalPlugin.mm`:20G81 discovery principal class and real NativeAdreno
  ABI preflight, matching Pong0x07030002/2MiB GMEM. It follows Sheng's
  MetalPluginName/NSBundle/accelerator-port route without its diagnostics,
  private iOS17 offsets or fake capabilities. The platform must publish
  actual completed CP and qualified20G81 runtime policy before this entry
  can create a device. The plugin cannot set those readiness properties.
  `build-pong-metal-plugin.py` links a self-contained development bundle
  beside the Rust bridge and Turnip. v96 stages it under private RAM Data
  `/pong/PongAdrenoMetal.bundle`; it is not a system discovery/injection path.
- Read the supplied20G81 cache's Objective-C metadata: `_MTLDevice` has
 528bytes and523 own methods, rather than Sheng21H461's536byte extent.
 `PongMetalAbi.h` requires the actual size and selector return/argument ABI
 before creating a Vulkan runtime or allocating the dynamic subclass.
 No private superclass ivars are written.
- Removed Sheng's iOS17 QuartzCore private context/global-offset diagnostic
 reads. Removed its CPU/memory/SHA benchmark and embedded AIR fixtures.
 Ordinary API calls still report real failures; no diagnostic workload runs.
- `supportsFamily:` uses the runtime's real answer. The original reference
 family report is retained as information, without forced Apple GPU claims.
- Restored bounded universal metallib AIR64 extraction for actual libraries.
 GPU binary members are never substituted for AIR.
- Build the Objective-C++ frontend and Rust product bridge for
 `arm64-apple-ios16.6.1`, then link `PongMetalBridge.dylib` against actual
 frameworks, with no undefined-symbol escape in this bridge link.
- Turnip builds the exact0x07030002/FD725 compiler profile and Darwin client
 backend using the shared fixed-size NativeAdreno wire ABI and4KiB BO
 alignment. It does not change the chip to A740/generic FD730.
 The optional raw builtin variant pack is absent; normal runtime IR3
 compilation remains available. No image hashing or shader pack relabelling.

## v96 actual20G81 runtime policy

The local runtime patch adds `MetalLevel::IOS16_6_1`; the Pong bridge selects
it explicitly. Pixel metadata uses20G81's actual exported
`MTLPixelFormatGetInfo` seven-word ABI, with its null feature-set query.
No macOS26/27 flags/cast classes are relabelled as iOS. Vulkan capabilities
still decide which textures the real device can use. Unknown private
reference counts/flags do not borrow Apple M3/M4 values; the device's own
implemented limits/capabilities remain the runtime's real answers.
Unqualified Apple compressed-heap layouts and lossless compressed blits
stay unsupported. The reproducible source patch is
`patches/metal2vulkan/0008-pong-ios16-20G81-policy.patch`.

## Remaining integration

Pong CP/GPU-VM/ZAP/native-client startup is now connected, with actual
timestamp completion required before publication. Phone execution is still
unconfirmed. The plugin's standard Metal accelerator discovery and GPU
IOSurface presentation remain separate work. Its policy-ready property
stays false until that integration exists, so merely staging these files
does not replace the functioning software renderer. Sheng's iOS17 private
offsets, diagnostic workloads and A740 identity are not used. v95 software
rendering/touch/About remain the known working baseline.

## Build integration

The final bundle is assembled from locally built NativeMetal bridge and Turnip
libraries. These third-party source trees and compiled libraries are not
vendored here. The repository script packages the local products without
installing them on a phone:

```sh
python3 scripts/build-pong-metal-plugin.py \
  --bridge-directory "$NATIVE_METAL_BUILD" \
  --turnip-library "$TURNIP_BUILD/libvulkan_freedreno.dylib" \
  --output-dir out/metal
```

Set `DEVELOPER_DIR` or pass `--developer-dir` when Xcode is not selected by
`xcode-select`. No generated libraries, firmware blobs or Apple restore files
are committed.
