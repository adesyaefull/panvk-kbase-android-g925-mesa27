# PanVK environment variables

This guide lists the environment variables and debug switches recognized by
the current PanVK G925 Android build and the Mesa compiler/cache code it uses.
Options are build- and platform-dependent: for example, X11/Termux WSI options
do not apply to an Android-native surface. Unset options use the defaults
listed below.

## `PANVK_DEBUG`

Set `PANVK_DEBUG` to a comma-separated list of PanVK debug options. For
example:

```sh
PANVK_DEBUG=trace,cs
```

Debug switches are primarily for diagnostics and controlled comparisons; they
are not general performance presets. The parser accepts only the options listed
below; unrecognized names such as `heap_stats` and `rt_test` do not enable a
feature. Older changelog entries mentioning `bc_expansion` or `heap_stats`
describe historical builds; neither option is recognized by the current
parser.

| Option | Effect |
|---|---|
| `startup` | Log the active PanVK debug switches during instance creation. |
| `nir` | Enable NIR-related diagnostics. |
| `trace` | Enable PanVK tracing diagnostics. |
| `sync` | Enable synchronization diagnostics. |
| `noafbc` | Disable AFBC image-modifier selection. |
| `linear` | Request the linear image path where the affected code supports it. |
| `dump` | Enable driver dump diagnostics. |
| `cs` | Enable command-stream diagnostics. |
| `copy_gfx` | Select the graphics path for eligible copy operations. |
| `force_simultaneous` | Force simultaneous queue behavior for diagnostics. |
| `implicit_others_inv` | Enable the implicit-other-invocations behavior used for testing. |
| `force_blackhole` | Force the blackhole path for diagnostics. |
| `wsi_no_afbc` | Disable AFBC for WSI images. |
| `no_wb_mmap` | Disable write-back mmap behavior. |
| `no_user_mmap_sync` | Disable the userspace mmap synchronization path. |
| `cached_before_coherent` | Enable a cache/coherency diagnostic mode. |
| `no_extended_va_range` | Disable the extended GPU virtual-address range. |
| `hsr_prepass` | Force the HSR prepass on. |
| `no_crc` | Disable the CRC path where supported. |
| `kbase_diag` | Enable kbase diagnostics. |
| `gpu_prerast` | Enable the GPU pre-raster path/diagnostics. |
| `no_bc_emul` | Disable BC format emulation. |
| `no_gs` | Disable the geometry-shader path. |
| `robust_ssbo_vec` | Enable the robust SSBO vector diagnostic path. |
| `spoof_nvidia` | Report an NVIDIA RTX 3060 identity for compatibility experiments; does not add NVIDIA features. Do not use for normal gameplay. |
| `spoof_amd` | Report an AMD RX 6600 identity for compatibility experiments; does not add AMD features. Do not use for normal gameplay. If both spoof switches are present, AMD takes precedence. |

BC options can affect image handling. `no_bc_emul` disables the emulation path
and can make BC resources unusable when the hardware path is unavailable.
Identity spoofing changes reported properties only; it does not turn PanVK
into an AMD or NVIDIA driver. Applications may nevertheless choose vendor-
specific paths based on the spoofed identity, so leave both spoof switches
disabled when diagnosing stability.

## PanVK BC, KRAID, and CSF options

| Variable | Values and default | Description |
|---|---|---|
| `PANVK_BC_ETC` | `1` (default); set `0` to disable | Uses ETC2 for BC1-BC3 by default. With ETC2 disabled, BC1 uses ASTC 4x4 LDR; BC2/BC3 use AFRC when an eligible AFRC coding unit is selected, otherwise ASTC 4x4 LDR. |
| `PANVK_BC_EAC` | `1` (default); set `0` to enable R8/RG8 | Uses compact EAC R11/RG11 transcodes for BC4/BC5 by default. Set to `0` for exact R8/RG8 decoded values, at the cost of a larger decoded payload. |
| `PANVK_BC_DIRECT_UPLOAD` | `1` (default); set `0` to disable when raw BC storage is retained | Decodes BC1-BC7 directly from the upload buffer into the GPU image used for sampling. Set to `0` to compare the copy-to-raw-image-then-decode path. Images that omit raw BC storage always decode directly, regardless of this setting. This selects the upload route; it does not enable native BC hardware decoding. |
| `PANVK_BC_MIP_PRIORITY` | `1` (default); set `0` to disable | For BC decode batches containing multiple mip levels, processes the smallest mip levels first and inserts a write dependency between levels. This prioritizes low-resolution levels; it is not residency streaming and does not defer the full-resolution mip based on visibility. |
| `PANVK_BC_LINEAR` | `0` (default); set `1` to enable | Forces the decoded BC shadow to use a linear layout. This prevents raw-plane elision, so eligible images also retain their compressed source plane. |
| `PANVK_BC_KEEP_RAW` | `0` (default); set `1` to enable | Retains the original compressed BC plane even when the image is eligible to omit it. Eligible images omit raw storage when their image allocation is laid out; uploads transcode directly from the copy source into the decoded plane. Transfer-source, external, sparse, storage, and attachment images are not eligible for raw-plane elision. This uses additional memory. |
| `PANVK_BC_AFRC` | `0` (default); `0`, `16`, `24`, `32`, `lowmem` | `0` disables AFRC. Numeric values select that coding unit for eligible BC2/BC3 images when `PANVK_BC_ETC=0`. `lowmem` selects 24-byte units for BC2 and 32-byte units for BC3. AFRC is lossy and opt-in; image eligibility depends on GPU and image properties. |
| `PAN_USE_KRAID` | `all` (default), `cs`, `fs`, `vs`, `internal` | Selects KRAID compiler stages: compute, fragment, vertex, and internal shaders. KRAID is used only on supported architecture v9+ builds. |
| `PAN_KRAID_TIMING` | `false` (default); set `1` to enable | Logs KRAID shader compile time and generated binary size. |
| `PANVK_HSR_PREPASS` | `true` (default); set `0` to disable | Enables/disables the architecture-supported HSR prepass. `PANVK_DEBUG=hsr_prepass` forces it on. |
| `PANVK_ZS_PRELOAD_ALWAYS` | `false` (default); set `1` to enable on `PAN_ARCH == 11` | Forces depth/stencil preload on the v11 path. It has no effect on Mali-G925 (`PAN_ARCH == 13`), whose code uses `PREPASS_ALWAYS` independently of this option. |
| `PANVK_KBASE_HEAP_RENEW_INTERVAL` | `128` (default); `0` disables this trigger | Sets the tiler-submission interval for proactive kbase tiler-heap renewal. The work-based trigger remains active unless separately disabled. |
| `PANVK_KBASE_HEAP_RENEW_WORK` | `65536` (default); `0` disables this trigger | Sets the accumulated tiler-work threshold for proactive heap renewal. |
| `PANVK_KBASE_GPU_SEMAPHORE_WAITS` | `false` (default); set `1` to opt in | Enables in-ring GPU semaphore waits where supported. Keep disabled for normal use: this path has had CTS/device-loss regressions. |
| `PANVK_RP_MERGE` | `false` (default); set `1` to opt in | Enables experimental CSF render-pass merging for eligible primary-command-buffer rendering scopes. Ineligible state or commands flush the pending pass. Keep disabled unless deliberately testing; runtime validation is pending. |
| `PANVK_LAZY_VTWAIT` | `false` (default); set `1` to opt in | Enables experimental deferred fragment waits for eligible vertex/tiler-subqueue dependencies. The deferred wait is emitted before dependent work or command-buffer completion. Keep disabled unless deliberately testing; runtime validation is pending. |
| `PANVK_LAZY_CWAIT` | `false` (default); set `1` to opt in | Enables experimental deferred fragment waits for eligible compute-subqueue dependencies. Keep disabled unless deliberately testing; runtime validation is pending. |
| `PANVK_LAZY_VTWAIT_FILE` | Unset (default); filesystem path | Optional live toggle for `PANVK_LAZY_VTWAIT`: the feature is enabled only while the named file exists. The path is checked at most once per second; when configured, it controls the toggle instead of the boolean variable. |
| `PANVK_LAZY_CWAIT_FILE` | Unset (default); filesystem path | Optional live toggle for `PANVK_LAZY_CWAIT`, with the same file-exists and once-per-second behavior as `PANVK_LAZY_VTWAIT_FILE`. |
| `PANVK_LAZY_TRACE` | `0` (default); non-negative integer | Logs up to this many rejected barriers from the lazy vertex/tiler-wait eligibility check. Diagnostic only; it does not enable deferred waits. |
| `PANVK_LAZY_TRACE_SKIP` | `0` (default); non-negative integer | Skips this many rejected barriers before `PANVK_LAZY_TRACE` starts logging. |
| `PANVK_DESC_TRACEBUF_SIZE` | `2 MiB` (default) | Sets the descriptor trace buffer size in bytes when descriptor tracing is active. |
| `PANVK_CS_TRACEBUF_SIZE` | `2 MiB` (default) | Sets the CSF command-stream trace buffer size in bytes. The implementation requires a value greater than 512 KiB. |
| `PANVK_UTRACE_CLONE_MEM_SIZE` | `0xa00000` bytes (10 MiB, default) | Sets the CSF utrace cloned-memory pool size. Increase it only when utrace reports that the pool cannot satisfy an allocation; this reserves additional memory. |

## PanVK kbase and WSI options

| Variable | Values and default | Description |
|---|---|---|
| `PANVK_KBASE_DMA_HEAP` | `/dev/dma_heap/system` (default) | Selects the dma-heap device used for kbase buffer export. The path must exist and be accessible to the application. |
| `PANVK_KBASE_DVFS` | `max` by default on Mali-G925; `none`, `auto`, `default`, or a frequency in kHz | On G925, `max` fixes the GPU at its highest available frequency. This affects the device's global GPU DVFS policy, not only the game, and can increase heat and power use. `none` preserves the existing policy; `auto` selects the full available frequency range and a dynamic governor (`quickstep_use_mcu`, falling back to `capacity_use_mcu`); `default` selects the `quickstep` governor. Unset on non-G925 Kbase devices leaves their policy unchanged. |
| `PANVK_KBASE_USER_CACHE_SYNC` | Unset (default); `0` selects kernel sync | With `0`, cached BO synchronization uses the kernel ioctl path rather than userspace cache maintenance. |
| `PANVK_KBASE_KCPU_SYNC` | Unset (default); set `1` to opt in | Attempts to create/use a CSF KCPU sync queue when the kernel supports it. |
| `PANVK_KBASE_IMPORT_CLEAR` | Unset; hexadecimal/integer mask | Clears the selected bits from kbase dma-buf import flags. Bring-up/debug option; changing flags can break imports. |
| `PANVK_KBASE_IMPORT_SET` | Unset; hexadecimal/integer mask | Sets the selected bits in kbase dma-buf import flags. Bring-up/debug option; changing flags can break imports. |
| `PANVK_KBASE_DRI3` | Unset; `raw`, `termux`, or another non-`0` value | X11/kbase WSI override. `raw` and `termux` request the raw-FD DRI3 transport; another nonzero value opts into the kbase dma-buf path without selecting that raw transport. |
| `PANVK_KBASE_EMA_PACING` | Enabled by default; set `0` to disable | Applies per-swapchain EMA pacing before infinite-timeout image acquisition on Kbase, unless the application uses explicit present timing. The EMA is based on successful WSI present-callback completion intervals (a cadence proxy, not a display scanout timestamp) and resets after 250 ms idle. |
| `WSI_X11_TERMUX` | Unset; any non-`0` value enables | Termux:X11 compatibility switch. When no explicit `PANVK_KBASE_DRI3` override is set, enables the raw-FD DRI3 path on kbase. X11/Termux only. |
| `MESA_VK_WSI_PRESENT_MODE` | Overridden to `fifo` on Kbase | Kbase devices force FIFO presentation until queued present-mode synchronization is validated; other backends honor this Mesa option as usual. |

The G925 Kbase heap limit is a build-time setting, not an environment-variable
override: 8 MiB per chunk, two initial chunks, and 64 maximum chunks (512 MiB
maximum). The renewal variables above control when a heap is renewed; they do
not raise that maximum. The reference baseline also keeps
`target_in_flight=65535`.

## Shader compiler debug options

These are Mesa Panfrost compiler options, not PanVK-only Vulkan controls. They
are useful for shader compiler debugging and can produce substantial logs.

| Variable | Accepted comma-separated options | Description |
|---|---|---|
| `BIFROST_MESA_DEBUG` | `shaders`, `shaderdb`, `verbose`, `internal`, `nosched`, `nopsched`, `inorder`, `novalidate`, `noopt`, `noidvs`, `nosb`, `nopreload`, `spill`, `nossara`, `statsabs`, `statsfull`, `debuginfo` | Bifrost/Valhall compiler diagnostics and testing. Includes shader dumps/statistics, scheduling and validation toggles, and register-spill testing. Some options intentionally alter compiler behavior. |
| `MIDGARD_MESA_DEBUG` | `shaders`, `shaderdb`, `inorder`, `verbose`, `internal` | Midgard compiler shader dumps, statistics, and scheduling diagnostics. Not used by the G925 (`PAN_ARCH == 13`) compiler path. |

## Mesa shader and disk-cache options

On Android, Winlator runs the ICD as an untrusted app. Android denies that
process access to `vendor.mesa.*` properties; the driver therefore does not
probe that property namespace. Mesa still reads option values from the process
environment first, and from `debug.mesa.*` / `mesa.*` properties as fallbacks.
Set these variables in the Winlator container or game environment (before
launching it), not in a separate shell:

| Environment variable | Example | Corresponding debug option |
|---|---|---|
| `MESA_VK_TRACE` | `rmv` | Vulkan trace mode (`rmv` is the mode supported by this Mesa source). |
| `MESA_VK_TRACE_PER_SUBMIT` | `1` | Write Vulkan trace data per queue submission. |
| `MESA_VK_TRACE_FRAME` | `120` | Limit Vulkan tracing to the selected frame. |
| `MESA_VK_TRACE_TRIGGER` | `/path/to/trigger` | Trigger trace capture when the named file exists and is writable; Mesa removes it when tracing is triggered. |
| `PAN_CPU_TRACE` | `vk.csf,lib.kmod` | Enable Panfrost CPU trace categories; `vk.csf` and `lib.kmod` are useful PanVK diagnostics. |
| `PANVK_DEBUG` | `trace` | Enable PanVK trace diagnostics and synchronous submissions. Kbase allocates trace-buffer GPU addresses automatically; command-stream binaries and hardware trace records are decoded as on other backends. The decoder safely reports and skips optional framebuffer/texture fields whose GPU address is null rather than dereferencing address zero. Tracing can add substantial overhead. Unset it for normal play. |
| `GALLIUM_PRINT_OPTIONS` | `1` | Print Gallium option values for diagnostics. Does not apply to this Vulkan-only PanVK build (Gallium drivers are not built). |

For example, to request PanVK's CSF trace plus Mesa Vulkan trace capture, add
these to the Winlator environment:

```sh
PANVK_DEBUG=trace
MESA_VK_TRACE=rmv
MESA_VK_TRACE_PER_SUBMIT=1
PAN_CPU_TRACE=vk.csf,lib.kmod
```

`MESA_VK_TRACE_PER_SUBMIT=1` and full PanVK tracing may generate large logs and
slow execution. Enable only the tracing needed for a focused capture. This
change does not grant the app SELinux permission to read `vendor.*`; it avoids
the denied reads and preserves configuration through environment variables.

| Variable | Values and default | Description |
|---|---|---|
| `MESA_SHADER_CACHE_DIR` | Unset (Mesa chooses its normal cache path) | Sets the shader-cache directory. It must be writable by the application. |
| `MESA_SHADER_CACHE_MAX_SIZE` | Mesa default; the launcher may use `2G` | Sets the maximum shader-cache size. Mesa accepts size strings such as `2G`. |
| `MESA_SHADER_CACHE_DISABLE` | Unset | Disables Mesa disk caches that honor the opt-out. PanVK creates its physical-device cache with `disk_cache_create_forced()`, which bypasses this opt-out; this variable does **not** disable that PanVK cache. |
| `MESA_GLSL_CACHE_DIR` | Deprecated alias for `MESA_SHADER_CACHE_DIR` | Used only when the newer variable is not set. |
| `MESA_GLSL_CACHE_MAX_SIZE` | Deprecated alias for `MESA_SHADER_CACHE_MAX_SIZE` | Mesa may print a deprecation warning; use the `MESA_SHADER_CACHE_*` name instead. |
| `MESA_GLSL_CACHE_DISABLE` | Deprecated alias on non-Android builds | The fallback is compiled out on Android, so this variable does **not** disable the cache in the G925 Android build. Use `MESA_SHADER_CACHE_DISABLE` for caches that honor the opt-out; note the PanVK forced-cache exception above. |
| `MESA_DISK_CACHE_DATABASE_NUM_PARTS` | `50` (default) | Sets the number of parts in Mesa's disk-cache database. Only applies when the database backend is selected (for example, with `MESA_DISK_CACHE_DATABASE=1`). |
| `MESA_DISK_CACHE_DATABASE_EVICTION_SCORE_2X_PERIOD` | `2592000` seconds (30 days, default) | Sets database eviction scoring period. Only applies when the database backend is selected. |

## Example

To retain compressed BC data for raw readback:

```sh
PANVK_BC_KEEP_RAW=1
```

ETC2 for BC1-BC3 and EAC for BC4/BC5 are the defaults. To explicitly select
these settings:

```sh
PANVK_BC_ETC=1 PANVK_BC_EAC=1
```

For normal gameplay, leave AFRC, GPU semaphore waits, compiler
debugging, and identity spoof options unset. The environment must be set on
the process that loads the Vulkan ICD; setting it in a separate shell does not
affect an already-running emulator or game.
