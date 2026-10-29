# PanVK for Mali-G925 — Mesa 27

PanVK is Mesa's Vulkan driver for Mali GPUs. This Android build targets the **Mali-G925 / Immortalis MC11** using the **Mali Kbase CSF (Command Stream Frontend)** backend. The published ADPKG is an experimental G925 build, not a Vulkan-conformant release.

The **Android AArch64 ADPKG** is attached to the [Mesa 27 G925 release](https://github.com/adesyaefull/panvk-kbase-android-g925-mesa27/releases/tag/mesa27-g925-bc-mip-priority-20261029). That release page is the download location.

## Target

| Item | G925 runtime capture |
|---|---|
| GPU | Mali-G925 / Immortalis MC11 (`0xd8300010`) |
| Driver | PanVK Kbase beta.13; Mesa 27.0.0-devel (`e2d83eff1c`) |
| Backend | **CSF only** |
| Architecture coverage | CSF `PAN_ARCH` 11–14 in this source build; released package is for G925 `PAN_ARCH=13` |
| Not included | Job Manager (JM) and architectures below 11 |
| Kernel interface | Mali Kbase (`/dev/mali0`) |
| Package | Android AArch64, API 35 minimum |
| Vulkan API reported | 1.4.363 |
| Vulkan conformance version | `0.0.0.0` |

## Driver highlights

- **Kbase CSF execution:** Vulkan command buffers are translated into Mali CSF command streams. Graphics uses the vertex/tiler path; compute uses the compute path. The driver tracks queue completion and handles Kbase CSF notifications.
- **Android graphics integration:** native-window swapchains, Android native-buffer presentation, and `AHardwareBuffer` / gralloc import paths are present in the G925 driver.
- **Modern Vulkan workload support:** the runtime advertises Vulkan 1.4 and extensions for dynamic rendering, synchronization2, timeline semaphores, descriptor indexing/update-after-bind, multiview, indirect draw counts, buffer device address, pipeline libraries, and host image copy. Applications must still query the exact features they need.
- **Robustness and diagnostics:** robustness features and Kbase device-fault reporting are exposed. Focused device tests exist for selected paths, but a passing focused test is not full extension conformance.
- **Shading rate:** the G925 path supports tested static and dynamic fragment shading rates up to 4x4. Primitive shading rate and shading-rate attachments are not supported by this path.
- **Compressed textures:** BC1–BC7 have GPU decode/transcode paths for formats that cannot be sampled directly; default target formats are listed below. AFRC is opt-in and lossy.
- **Windows-game compatibility checks:** the current runtime capture reports passing hard-requirement matrices for DXVK, vkd3d, and Bachata S4. These checks show compatibility requirements reported by those matrices; they do not prove every title renders correctly or runs without glitches.

## Renderer status and visual limitations

The G925 Vulkan capture identifies the active Vulkan driver as **PanVK-Mali-G925** (`driverName=panvk`) and reports Vulkan API **1.4.363**. Its compatibility summaries report:

| Runtime requirement matrix | Hard requirements | Soft requirements |
|---|---:|---:|
| DXVK | 56/56 available | 33/38 available |
| vkd3d / Proton path | 24/24 available | 67/76 available |
| Bachata S4 | 0 hard missing | 7 soft missing |

**Renderer detection and passing hard-requirement checks do not mean image rendering is fully fixed.** The Vulkan renderer is recognized on the tested G925, and the reported matrices pass their hard requirements, but **visible graphical glitches/artifacts can still occur in rendered images**. Visual correctness is workload- and application-dependent; this build does not claim that remaining image glitches are fixed. The capture reports conformance version `0.0.0.0`; these matrices are not Vulkan CTS results.

## Memory, limits, and G925 tiler heap

The runtime Vulkan capture reports two device-local memory heaps and three memory types. Heap sizes are the values returned by the driver; they are not a promise that all memory is free to allocate at once.

| Vulkan memory heap | Reported size | Approximate size | Flags |
|---|---:|---:|---|
| 0 | 11,759,542,272 bytes | 10.95 GiB | `DEVICE_LOCAL` |
| 1 | 104,857,600 bytes | 100 MiB | `DEVICE_LOCAL` |

| Memory type | Heap | Reported properties |
|---|---:|---|
| 0 | 0 | `DEVICE_LOCAL`, `HOST_VISIBLE`, `HOST_COHERENT` |
| 1 | 0 | `DEVICE_LOCAL`, `HOST_VISIBLE`, `HOST_CACHED` |
| 2 | 0 | `DEVICE_LOCAL`, `LAZILY_ALLOCATED` |

These Vulkan memory heaps are separate from the Kbase tiler heap, which backs render-workload tiling. The G925 Kbase tiler-heap profile is:

| Kbase tiler-heap setting | Value |
|---|---:|
| Chunk size | 8 MiB |
| Initial chunks | 2 (16 MiB) |
| Maximum chunks | 64 (512 MiB) |
| `target_in_flight` | 65,535 |
| Android WSI swapchain buffer limit | 8 |

The 512 MiB is the tiler-heap cap, **not additional memory on top of the Vulkan heaps**. Heap growth can still fail if a workload exceeds the cap or system memory is under pressure; previous G925 runs recorded tiler-heap OOM/device-loss behavior. The configured cap is not a guarantee that every render pass fits.

Other values from the runtime capture and G925 build profile include:

| Limit | Value | Source |
|---|---:|---|
| Maximum allocation size | 8 GiB | G925 profile cap; not a `VkPhysicalDeviceLimits` field in the capture |
| Maximum storage-buffer range | 2 GiB |
| Maximum texel-buffer elements | 268,435,456 |
| Maximum memory allocations | 16,384 |
| Maximum framebuffer / viewport dimensions | 16,384 × 16,384 |
| Maximum push constants | 256 bytes |
| Maximum bound descriptor sets | 7 |
| Maximum compute workgroup invocations | 1,024 |
| Compute shared memory | 32 KiB |
| Timestamp period | 76.9231 ns |
| Queue families / queues | 1 family / 2 queues |

Reported Vulkan capacities and limits describe what the driver advertises. Actual allocations depend on Android memory pressure, other GPU clients, image layouts, and application workload.

## Vulkan features and important gaps

Feature groups reported by the G925 capture include geometry and tessellation shaders, multi-viewport rendering, sample-rate shading, wide lines, logic operations, pipeline statistics queries, 8/16/64-bit shader data paths, descriptor indexing and update-after-bind, buffer device address, timeline semaphores, Vulkan memory model, dynamic rendering, synchronization2, multiview, host image copy, inline uniform blocks, and ASTC HDR/decode-mode support. Feature bits are queried individually; the presence of a corresponding extension in the inventory does not imply all optional features are enabled.

Known scope limits for this build include:

- Vulkan conformance has not been established; conformance version remains `0.0.0.0`.
- `VK_KHR_ray_tracing_pipeline` is not supported. Acceleration-structure and ray-query paths are constrained/experimental and are not general DXR support.
- Protected pipeline access can be enabled, but protected-memory allocation is not supported by the tested Kbase configuration.
- Kbase sparse residency is not enabled; sparse support depends on kernel capabilities and is not a general residency path.
- Custom sample locations are not advertised. Fragment shading-rate attachments and primitive shading rates are unavailable on the tested G925 path.
- Image correctness, stability, and performance can vary; visible glitches and workload-specific device-loss/OOM conditions remain possible.

## BCn texture emulation

When direct sampling is unavailable, the driver decodes BC texture data into a GPU shadow image:

| Input format | Default decoded representation | Nominal decoded rate |
|---|---|---:|
| BC1 | ETC2 RGB | 4 bits/texel |
| BC2 / BC3 | ETC2 RGBA | 8 bits/texel |
| BC4 | EAC R11 | 4 bits/texel |
| BC5 | EAC RG11 | 8 bits/texel |
| BC6H | ASTC 4x4 HDR | 8 bits/texel |
| BC7 | ASTC 4x4 LDR | 8 bits/texel |

### Nominal decoded footprint per mip level

For square images, the following estimates count complete 4x4 output blocks (8 bytes at 4 bits/texel; 16 bytes at 8 bits/texel). Dimensions below 4x4 still occupy at least one block.

| Image dimensions | 4 bits/texel | 8 bits/texel |
|---:|---:|---:|
| 1x1 | 8 B | 16 B |
| 2x2 | 8 B | 16 B |
| 4x4 | 8 B | 16 B |
| 8x8 | 32 B | 64 B |
| 16x16 | 128 B | 256 B |
| 32x32 | 512 B | 1 KiB |
| 64x64 | 2 KiB | 4 KiB |
| 128x128 | 8 KiB | 16 KiB |
| 256x256 | 32 KiB | 64 KiB |
| 512x512 | 128 KiB | 256 KiB |
| 1Kx1K | 512 KiB | 1 MiB |
| 2Kx2K | 2 MiB | 4 MiB |
| 4Kx4K | 8 MiB | 16 MiB |
| 8Kx8K | 32 MiB | 64 MiB |

These are per-level estimates, not measured allocations. Sum the mip levels for a full chain. They exclude tiling/alignment padding and any retained raw BC source plane. `PANVK_BC_ETC=0` selects ASTC for BC1 (8 bits/texel); BC2/BC3 use ASTC unless an eligible AFRC coding unit is selected. AFRC allocation depends on its coding unit and image layout. `PANVK_BC_EAC=0` selects R8/RG8 for BC4/BC5 (8/16 bits per texel, or 64/128 MiB at 8Kx8K).

`PANVK_BC_MIP_PRIORITY=1` (default) schedules the smallest mip levels first for multi-mip BC upload, copy, and zero-initialization decode batches, with a write dependency between levels. Set it to `0` to disable the ordering. This prioritizes decode work only: it is not residency streaming, does not defer full-resolution allocation, and all requested mips still need decoding. `PANVK_BC_DIRECT_UPLOAD`, `PANVK_BC_KEEP_RAW`, `PANVK_BC_LINEAR`, and `PANVK_BC_AFRC` control other paths; see [runtime options](./envar.md). BC image quality and performance are workload-dependent. The new mip-priority path has not yet been validated on the target G925 device.

## Vulkan extension inventory

<details>
<summary>Show all Vulkan extensions — 202 device + 16 instance</summary>

The names and `specVersion` values below are from the G925 Mesa 27 runtime capture. Advertising an extension does not by itself establish complete feature support or conformance.

### Device extensions (202)

| Extension | specVersion |
|---|---:|
| `VK_ANDROID_external_format_resolve` | 1 |
| `VK_ANDROID_external_memory_android_hardware_buffer` | 5 |
| `VK_ANDROID_native_buffer` | 8 |
| `VK_ARM_rasterization_order_attachment_access` | 1 |
| `VK_ARM_scheduling_controls` | 2 |
| `VK_ARM_shader_core_builtins` | 2 |
| `VK_ARM_shader_core_properties` | 1 |
| `VK_EXT_4444_formats` | 1 |
| `VK_EXT_astc_decode_mode` | 1 |
| `VK_EXT_attachment_feedback_loop_dynamic_state` | 1 |
| `VK_EXT_attachment_feedback_loop_layout` | 2 |
| `VK_EXT_border_color_swizzle` | 1 |
| `VK_EXT_buffer_device_address` | 2 |
| `VK_EXT_calibrated_timestamps` | 2 |
| `VK_EXT_color_write_enable` | 1 |
| `VK_EXT_conditional_rendering` | 2 |
| `VK_EXT_conservative_rasterization` | 1 |
| `VK_EXT_custom_border_color` | 12 |
| `VK_EXT_debug_marker` | 4 |
| `VK_EXT_depth_bias_control` | 1 |
| `VK_EXT_depth_clamp_control` | 1 |
| `VK_EXT_depth_clamp_zero_one` | 1 |
| `VK_EXT_depth_clip_control` | 1 |
| `VK_EXT_depth_clip_enable` | 1 |
| `VK_EXT_descriptor_indexing` | 2 |
| `VK_EXT_device_address_binding_report` | 1 |
| `VK_EXT_device_fault` | 2 |
| `VK_EXT_device_memory_report` | 2 |
| `VK_EXT_dynamic_rendering_unused_attachments` | 1 |
| `VK_EXT_extended_dynamic_state` | 1 |
| `VK_EXT_extended_dynamic_state2` | 1 |
| `VK_EXT_extended_dynamic_state3` | 2 |
| `VK_EXT_external_memory_acquire_unmodified` | 1 |
| `VK_EXT_external_memory_dma_buf` | 1 |
| `VK_EXT_frame_boundary` | 1 |
| `VK_EXT_global_priority` | 2 |
| `VK_EXT_global_priority_query` | 1 |
| `VK_EXT_graphics_pipeline_library` | 1 |
| `VK_EXT_hdr_metadata` | 3 |
| `VK_EXT_host_image_copy` | 1 |
| `VK_EXT_host_query_reset` | 1 |
| `VK_EXT_image_2d_view_of_3d` | 1 |
| `VK_EXT_image_compression_control` | 1 |
| `VK_EXT_image_compression_control_swapchain` | 1 |
| `VK_EXT_image_drm_format_modifier` | 2 |
| `VK_EXT_image_robustness` | 1 |
| `VK_EXT_image_sliced_view_of_3d` | 1 |
| `VK_EXT_image_view_min_lod` | 1 |
| `VK_EXT_index_type_uint8` | 1 |
| `VK_EXT_inline_uniform_block` | 1 |
| `VK_EXT_legacy_dithering` | 2 |
| `VK_EXT_line_rasterization` | 1 |
| `VK_EXT_load_store_op_none` | 1 |
| `VK_EXT_map_memory_placed` | 1 |
| `VK_EXT_memory_budget` | 1 |
| `VK_EXT_memory_priority` | 1 |
| `VK_EXT_multi_draw` | 1 |
| `VK_EXT_multisampled_render_to_single_sampled` | 1 |
| `VK_EXT_mutable_descriptor_type` | 1 |
| `VK_EXT_nested_command_buffer` | 1 |
| `VK_EXT_non_seamless_cube_map` | 1 |
| `VK_EXT_pageable_device_local_memory` | 1 |
| `VK_EXT_physical_device_drm` | 1 |
| `VK_EXT_pipeline_creation_cache_control` | 3 |
| `VK_EXT_pipeline_creation_feedback` | 1 |
| `VK_EXT_pipeline_protected_access` | 1 |
| `VK_EXT_pipeline_robustness` | 1 |
| `VK_EXT_present_timing` | 3 |
| `VK_EXT_primitive_topology_list_restart` | 1 |
| `VK_EXT_primitives_generated_query` | 1 |
| `VK_EXT_private_data` | 1 |
| `VK_EXT_provoking_vertex` | 1 |
| `VK_EXT_queue_family_foreign` | 1 |
| `VK_EXT_rasterization_order_attachment_access` | 1 |
| `VK_EXT_rgba10x6_formats` | 1 |
| `VK_EXT_robustness2` | 1 |
| `VK_EXT_sampler_filter_minmax` | 2 |
| `VK_EXT_scalar_block_layout` | 1 |
| `VK_EXT_separate_stencil_usage` | 1 |
| `VK_EXT_shader_atomic_float` | 1 |
| `VK_EXT_shader_demote_to_helper_invocation` | 1 |
| `VK_EXT_shader_image_atomic_int64` | 1 |
| `VK_EXT_shader_module_identifier` | 1 |
| `VK_EXT_shader_replicated_composites` | 1 |
| `VK_EXT_shader_stencil_export` | 1 |
| `VK_EXT_shader_subgroup_ballot` | 1 |
| `VK_EXT_shader_subgroup_vote` | 1 |
| `VK_EXT_shader_tile_image` | 1 |
| `VK_EXT_shader_uniform_buffer_unsized_array` | 1 |
| `VK_EXT_shader_viewport_index_layer` | 1 |
| `VK_EXT_subgroup_size_control` | 2 |
| `VK_EXT_subpass_merge_feedback` | 2 |
| `VK_EXT_swapchain_maintenance1` | 1 |
| `VK_EXT_texel_buffer_alignment` | 1 |
| `VK_EXT_texture_compression_astc_hdr` | 1 |
| `VK_EXT_tooling_info` | 1 |
| `VK_EXT_transform_feedback` | 1 |
| `VK_EXT_vertex_attribute_divisor` | 3 |
| `VK_EXT_vertex_attribute_robustness` | 1 |
| `VK_EXT_vertex_input_dynamic_state` | 2 |
| `VK_EXT_ycbcr_2plane_444_formats` | 1 |
| `VK_EXT_ycbcr_image_arrays` | 1 |
| `VK_EXT_zero_initialize_device_memory` | 1 |
| `VK_GOOGLE_decorate_string` | 1 |
| `VK_GOOGLE_hlsl_functionality1` | 1 |
| `VK_GOOGLE_user_type` | 1 |
| `VK_INTEL_shader_integer_functions2` | 1 |
| `VK_KHR_16bit_storage` | 1 |
| `VK_KHR_8bit_storage` | 1 |
| `VK_KHR_bind_memory2` | 1 |
| `VK_KHR_buffer_device_address` | 1 |
| `VK_KHR_calibrated_timestamps` | 1 |
| `VK_KHR_compute_shader_derivatives` | 1 |
| `VK_KHR_cooperative_matrix` | 2 |
| `VK_KHR_copy_commands2` | 1 |
| `VK_KHR_copy_memory_indirect` | 1 |
| `VK_KHR_create_renderpass2` | 1 |
| `VK_KHR_dedicated_allocation` | 3 |
| `VK_KHR_depth_clamp_zero_one` | 1 |
| `VK_KHR_depth_stencil_resolve` | 1 |
| `VK_KHR_descriptor_update_template` | 1 |
| `VK_KHR_device_group` | 4 |
| `VK_KHR_draw_indirect_count` | 1 |
| `VK_KHR_driver_properties` | 1 |
| `VK_KHR_dynamic_rendering` | 1 |
| `VK_KHR_dynamic_rendering_local_read` | 1 |
| `VK_KHR_external_fence` | 1 |
| `VK_KHR_external_fence_fd` | 1 |
| `VK_KHR_external_memory` | 1 |
| `VK_KHR_external_memory_fd` | 1 |
| `VK_KHR_external_semaphore` | 1 |
| `VK_KHR_external_semaphore_fd` | 1 |
| `VK_KHR_format_feature_flags2` | 2 |
| `VK_KHR_fragment_shading_rate` | 2 |
| `VK_KHR_get_memory_requirements2` | 1 |
| `VK_KHR_global_priority` | 1 |
| `VK_KHR_image_format_list` | 1 |
| `VK_KHR_imageless_framebuffer` | 1 |
| `VK_KHR_incremental_present` | 2 |
| `VK_KHR_index_type_uint8` | 1 |
| `VK_KHR_internally_synchronized_queues` | 1 |
| `VK_KHR_line_rasterization` | 1 |
| `VK_KHR_load_store_op_none` | 1 |
| `VK_KHR_maintenance1` | 2 |
| `VK_KHR_maintenance10` | 1 |
| `VK_KHR_maintenance11` | 1 |
| `VK_KHR_maintenance2` | 1 |
| `VK_KHR_maintenance3` | 1 |
| `VK_KHR_maintenance4` | 2 |
| `VK_KHR_maintenance5` | 1 |
| `VK_KHR_maintenance6` | 1 |
| `VK_KHR_maintenance7` | 1 |
| `VK_KHR_maintenance8` | 1 |
| `VK_KHR_maintenance9` | 1 |
| `VK_KHR_map_memory2` | 1 |
| `VK_KHR_multiview` | 1 |
| `VK_KHR_pipeline_executable_properties` | 1 |
| `VK_KHR_pipeline_library` | 1 |
| `VK_KHR_present_id` | 1 |
| `VK_KHR_present_id2` | 1 |
| `VK_KHR_present_wait` | 1 |
| `VK_KHR_present_wait2` | 1 |
| `VK_KHR_push_descriptor` | 2 |
| `VK_KHR_relaxed_block_layout` | 1 |
| `VK_KHR_robustness2` | 1 |
| `VK_KHR_sampler_mirror_clamp_to_edge` | 3 |
| `VK_KHR_sampler_ycbcr_conversion` | 14 |
| `VK_KHR_separate_depth_stencil_layouts` | 1 |
| `VK_KHR_shader_atomic_int64` | 1 |
| `VK_KHR_shader_clock` | 1 |
| `VK_KHR_shader_constant_data` | 1 |
| `VK_KHR_shader_draw_parameters` | 1 |
| `VK_KHR_shader_expect_assume` | 1 |
| `VK_KHR_shader_float16_int8` | 1 |
| `VK_KHR_shader_float_controls` | 4 |
| `VK_KHR_shader_float_controls2` | 1 |
| `VK_KHR_shader_fma` | 1 |
| `VK_KHR_shader_integer_dot_product` | 1 |
| `VK_KHR_shader_maximal_reconvergence` | 1 |
| `VK_KHR_shader_non_semantic_info` | 1 |
| `VK_KHR_shader_quad_control` | 1 |
| `VK_KHR_shader_relaxed_extended_instruction` | 1 |
| `VK_KHR_shader_subgroup_extended_types` | 1 |
| `VK_KHR_shader_subgroup_rotate` | 2 |
| `VK_KHR_shader_subgroup_uniform_control_flow` | 1 |
| `VK_KHR_shader_terminate_invocation` | 1 |
| `VK_KHR_shader_untyped_pointers` | 1 |
| `VK_KHR_spirv_1_4` | 1 |
| `VK_KHR_storage_buffer_storage_class` | 1 |
| `VK_KHR_swapchain` | 70 |
| `VK_KHR_swapchain_maintenance1` | 1 |
| `VK_KHR_swapchain_mutable_format` | 1 |
| `VK_KHR_synchronization2` | 1 |
| `VK_KHR_timeline_semaphore` | 2 |
| `VK_KHR_unified_image_layouts` | 1 |
| `VK_KHR_uniform_buffer_standard_layout` | 1 |
| `VK_KHR_variable_pointers` | 1 |
| `VK_KHR_vertex_attribute_divisor` | 1 |
| `VK_KHR_vulkan_memory_model` | 3 |
| `VK_KHR_workgroup_memory_explicit_layout` | 1 |
| `VK_KHR_zero_initialize_workgroup_memory` | 1 |
| `VK_VALVE_mutable_descriptor_type` | 1 |

### Instance extensions (16)

| Extension | specVersion |
|---|---:|
| `VK_EXT_debug_report` | 10 |
| `VK_EXT_debug_utils` | 2 |
| `VK_EXT_headless_surface` | 1 |
| `VK_EXT_surface_maintenance1` | 1 |
| `VK_EXT_swapchain_colorspace` | 5 |
| `VK_KHR_android_surface` | 6 |
| `VK_KHR_device_group_creation` | 1 |
| `VK_KHR_external_fence_capabilities` | 1 |
| `VK_KHR_external_memory_capabilities` | 1 |
| `VK_KHR_external_semaphore_capabilities` | 1 |
| `VK_KHR_get_physical_device_properties2` | 2 |
| `VK_KHR_get_surface_capabilities2` | 1 |
| `VK_KHR_surface` | 25 |
| `VK_KHR_surface_maintenance1` | 1 |
| `VK_KHR_xcb_surface` | 6 |
| `VK_KHR_xlib_surface` | 6 |

</details>

The standalone [Vulkan extension inventory](./VULKAN-EXTENSIONS.md) includes capture metadata and the same extension list.

## Credits

Credits and source references for the driver work:

- **Mesa source:** [Mesa upstream](https://gitlab.freedesktop.org/mesa/mesa), base commit [`e2d83eff1c715abf1bbad64fd0607a048c1336e6`](https://gitlab.freedesktop.org/mesa/mesa/-/tree/e2d83eff1c715abf1bbad64fd0607a048c1336e6).
- **Earlier G925 project / Mesa predecessor:** [adesyaefull/panvk-kbase-android-g925](https://github.com/adesyaefull/panvk-kbase-android-g925) documents the experimental Mali-G925 (`PAN_ARCH=13`) Kbase CSF work, based on the G615 Kbase CSF beta.8 foundation from [zenithblue-oss/panvk-kbase-android](https://github.com/zenithblue-oss/panvk-kbase-android). Its README identifies Mesa commit [`5a07217f034b3e50d8c7c7794f97a2df1742613b`](https://gitlab.freedesktop.org/mesa/mesa/-/tree/5a07217f034b3e50d8c7c7794f97a2df1742613b) as its source base. This Mesa 27 publication uses the newer Mesa base above and is a separate source snapshot, not a continuation of that repository’s Git history.
- **Kbase project foundation:** [zenithblue-oss/panvk-kbase-android](https://github.com/zenithblue-oss/panvk-kbase-android).
- **CSF / Valhall reference:** [wonderkast02/panvk-g720-kbase-csf](https://github.com/wonderkast02/panvk-g720-kbase-csf/tree/d9cbb91e5df720f0a62c4b5010d96c327aec315f).
- **Additional Kbase/PanVK development history and references:** [nangitagamer777-art/PanVK-kbase](https://github.com/nangitagamer777-art/PanVK-kbase), [nangitagamer777-art/Panvk_Kmod](https://github.com/nangitagamer777-art/Panvk_Kmod), and [leegao/mesa-funnymdzz](https://github.com/leegao/mesa-funnymdzz).
- **BC texture transcode implementation reference:** [GunaCharanTeja/panvk-kbase-android](https://github.com/GunaCharanTeja/panvk-kbase-android); selected implementation work was adapted for this source tree.
- **Optional BC compatibility reference:** [leegao/bcn_layer](https://github.com/leegao/bcn_layer/tree/50993a2d51772567de9c36de4d523652773f0899), pinned to commit `50993a2d51772567de9c36de4d523652773f0899`; reference only, not bundled.
- **Kbase UAPI/register headers:** [Arm Mali software](https://github.com/ARM-software) and the other original authors named in the respective source headers.

These are direct links to project and source references. Credits identify foundations and specific references; they do not claim every referenced repository is bundled or that every source file was copied. Mesa, Arm UAPI, and third-party components keep their own copyright and license terms. See [NOTICE.md](./NOTICE.md) for provenance and license details.

## History and support

- [CHANGELOG](./CHANGELOG.md) — complete local driver changelog, including experiments, rollbacks, build records, and validation history.
- [SUPPORT](./SUPPORT.md) — reproduction and PanProbe, PanPlay, or ADB log collection steps.
- [Report an issue](https://github.com/adesyaefull/panvk-kbase-android-g925-mesa27/issues/new?template=bug_report.yml) — include the exact driver build, reproduction steps, and relevant logs.
