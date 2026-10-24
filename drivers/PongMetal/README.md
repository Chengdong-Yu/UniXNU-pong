# Pong Metal / iOS16.6.1 development

This is a local fork of Sheng's NativeMetal frontend and object bridge over
metal2vulkan and Turnip. The compiled path now includes stock20G81 Metal
discovery, a genuine IOAcceleratorES parent, native BO clients and GPU scanout.
**Actual Pong GPU execution and accelerated presentation remain unconfirmed.**
The most recent phone log stops before CP initialization completes.

## Actual adaptations

- `PongMetalPlugin.mm`:20G81 discovery principal class and real NativeAdreno
  ABI preflight, matching Pong0x07030002/2MiB GMEM. It follows Sheng's
  MetalPluginName/NSBundle/accelerator-port route without its diagnostics,
  private iOS17 offsets or fake capabilities. The platform must publish
  actual completed CP and qualified20G81 runtime policy before this entry
  can create a device. The plugin cannot set those readiness properties.
  `build-pong-metal-plugin.py` links a self-contained development bundle
  beside the Rust bridge and Turnip. The PID1 helper presents the bundle
  through a read-only union at `/System/Library/Extensions`, as Sheng does.
  The original Metal discovery loads its principal class; manual MTLAddDevice
  injection has been removed.
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

## Hardware integration and remaining confirmation

Pong CP/GPU-VM/ZAP/native-client startup is now connected, with actual
timestamp completion required before publication. Phone execution is still
unconfirmed. `PongAdrenoAccelerator` uses the supplied20G81 base metaclass,
constructor and destructor. It initializes only that empty original base;
loading every IOGPUFamily dependency would duplicate the native IOSurface
provider. The native GPU service and accelerator share one BO/VM/submission
backend. The compositor owns its large address window and scanout connection.
`PongAdrenoMetal.publish` and `.clients` enable discovery for backboardd,
SpringBoard, CollectionsPoster and WidgetRenderer_Default. The seed file is the publication
policy; there is no second compiled list rejecting wallpaper clients. BOs and surfaces are imported
through the native ABI, and only completed GPU work retires displayed frames.
Successful compilation or a staged bundle does not establish acceleration.

## Rebuild, without image verification or device operations

From the main project root:

```sh
/opt/local/bin/python3.12 scripts/build-pong-metal-bridge.py \
  --translator /Volumes/PongDev/upstream/gpu-pong/metal2vulkan \
  --output-dir /Volumes/PongDev/build/Pong-iOS16-adreno-development/metal

/opt/local/bin/python3.12 scripts/build-pong-turnip.py \
  --mesa /Volumes/PongDev/upstream/gpu-pong/mesa \
  --abi-directory /Volumes/PongDev/upstream/gpu-pong/NativeAdreno \
  --tools-directory /Volumes/PongDev/build/Pong-iOS16-adreno-development/tools \
  --venv /Volumes/PongDev/build/Pong-iOS16-adreno-development/venv \
  --output-dir /Volumes/PongDev/build/Pong-iOS16-adreno-development/turnip
```

The translator uses NativeMetal's seven bridge patches at its pinned
720ef1ce revision. Mesa uses UniXNU's nine Darwin/compiler/import patches
at85fda3b3. Source pin identifiers identify upstream code; no image hashes
or readback checks are performed. Dependency licenses remain in their
source trees: NativeMetal MIT, metal2vulkan GPL/LGPL, and Mesa per-file
licenses. Product archives, frontend objects and exact build commands stay
beside the local libraries; no phone installation or runtime test is run by
these scripts.
