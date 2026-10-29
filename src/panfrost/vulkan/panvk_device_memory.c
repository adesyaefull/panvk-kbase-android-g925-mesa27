/*
 * Copyright © 2021 Collabora Ltd.
 * SPDX-License-Identifier: MIT
 */

#include "genxml/decode.h"

#include "vulkan/util/vk_util.h"

#include "panvk_android.h"
#include "panvk_device.h"
#include "panvk_device_memory.h"
#include "panvk_entrypoints.h"

#include "pan_props.h"

#include "util/u_atomic.h"
#include "vk_debug_utils.h"
#include "vk_log.h"

#define PANVK_MEMORY_POOL_CHUNK_SIZE     (32ull * 1024 * 1024)
#define PANVK_MEMORY_POOL_MAX_ALLOC_SIZE (PANVK_MEMORY_POOL_CHUNK_SIZE / 2)
#define PANVK_MEMORY_POOL_MIN_ALIGNMENT  (64ull * 1024)

struct panvk_memory_pool_chunk {
   struct list_head node;
   struct pan_kmod_bo *bo;
   struct util_vma_heap heap;
   uint64_t dev_addr;
   uint64_t size;
   uint64_t usable_size;
   uint32_t memory_type_index;
};

void
panvk_memory_pool_init(struct panvk_device *device)
{
   simple_mtx_init(&device->memory_pool.lock, mtx_plain);
   list_inithead(&device->memory_pool.chunks);
}

static void
panvk_memory_pool_destroy_chunk(struct panvk_device *device,
                                struct panvk_memory_pool_chunk *chunk)
{
   struct panvk_physical_device *pdev =
      to_panvk_physical_device(device->vk.physical);
   struct pan_kmod_vm_op op = {
      .type = PAN_KMOD_VM_OP_TYPE_UNMAP,
      .va = {
         .start = chunk->dev_addr,
         .size = chunk->size,
      },
   };
   ASSERTED int ret =
      pan_kmod_vm_bind(device->kmod.vm, PAN_KMOD_VM_OP_MODE_IMMEDIATE, &op, 1);
   assert(!ret);

   if (!(device->kmod.vm->flags & PAN_KMOD_VM_FLAG_AUTO_VA))
      panvk_as_free(device, chunk->dev_addr, chunk->size);

   util_vma_heap_finish(&chunk->heap);
   pan_kmod_bo_put(chunk->bo);
   const uint32_t heap_index =
      pdev->memory.types[chunk->memory_type_index].heapIndex;
   p_atomic_add(&pdev->memory.heap_used[heap_index],
                -((int64_t)chunk->size));
   list_del(&chunk->node);
   vk_free(&device->vk.alloc, chunk);
}

void
panvk_memory_pool_cleanup(struct panvk_device *device)
{
   simple_mtx_lock(&device->memory_pool.lock);
   list_for_each_entry_safe(struct panvk_memory_pool_chunk, chunk,
                            &device->memory_pool.chunks, node)
      panvk_memory_pool_destroy_chunk(device, chunk);
   simple_mtx_unlock(&device->memory_pool.lock);
   simple_mtx_destroy(&device->memory_pool.lock);
}

static VkResult
panvk_memory_pool_create_chunk(struct panvk_device *device,
                               uint32_t memory_type_index,
                               struct panvk_memory_pool_chunk **out)
{
   struct panvk_physical_device *pdev =
      to_panvk_physical_device(device->vk.physical);
   const uint64_t page_size = panvk_get_gpu_page_size(device);
   const uint64_t suballoc_alignment =
      MAX2(page_size, PANVK_MEMORY_POOL_MIN_ALIGNMENT);
   struct panvk_memory_pool_chunk *chunk = vk_zalloc(
      &device->vk.alloc, sizeof(*chunk), 8, VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
   if (!chunk)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   uint32_t bo_flags = panvk_device_adjust_bo_flags(device, 0);
   chunk->bo = pan_kmod_bo_alloc(
      device->kmod.dev,
      (device->kmod.vm->flags & PAN_KMOD_VM_FLAG_AUTO_VA) ? NULL
                                                          : device->kmod.vm,
      PANVK_MEMORY_POOL_CHUNK_SIZE, bo_flags);
   if (!chunk->bo) {
      vk_free(&device->vk.alloc, chunk);
      return VK_ERROR_OUT_OF_DEVICE_MEMORY;
   }

   struct pan_kmod_vm_op op = {
      .type = PAN_KMOD_VM_OP_TYPE_MAP,
      .va = {
         .start = PAN_KMOD_VM_MAP_AUTO_VA,
         .size = pan_kmod_bo_size(chunk->bo),
      },
      .map = {
         .bo = chunk->bo,
         .bo_offset = 0,
      },
   };

   if (!(device->kmod.vm->flags & PAN_KMOD_VM_FLAG_AUTO_VA)) {
      uint64_t alignment =
         pan_choose_gpu_va_alignment(device->kmod.vm, op.va.size);
      unsigned arch = pan_arch(device->kmod.dev->props.gpu_id);
      if (arch >= 10 && op.va.size >= 64 * 1024)
         alignment = MAX2(alignment, 64 * 1024);
      op.va.start = panvk_as_alloc(device, PANVK_NO_EXEC_VA_HEAP,
                                   op.va.size, alignment);
      if (!op.va.start) {
         pan_kmod_bo_put(chunk->bo);
         vk_free(&device->vk.alloc, chunk);
         return VK_ERROR_OUT_OF_DEVICE_MEMORY;
      }
   }

   int ret =
      pan_kmod_vm_bind(device->kmod.vm, PAN_KMOD_VM_OP_MODE_IMMEDIATE, &op, 1);
   if (ret) {
      if (!(device->kmod.vm->flags & PAN_KMOD_VM_FLAG_AUTO_VA))
         panvk_as_free(device, op.va.start, op.va.size);
      pan_kmod_bo_put(chunk->bo);
      vk_free(&device->vk.alloc, chunk);
      return VK_ERROR_OUT_OF_DEVICE_MEMORY;
   }

   chunk->dev_addr = op.va.start;
   chunk->size = pan_kmod_bo_size(chunk->bo);
   chunk->usable_size = chunk->size - suballoc_alignment;
   chunk->memory_type_index = memory_type_index;
   util_vma_heap_init(&chunk->heap, suballoc_alignment, chunk->usable_size);
   list_addtail(&chunk->node, &device->memory_pool.chunks);
   const uint32_t heap_index =
      pdev->memory.types[memory_type_index].heapIndex;
   p_atomic_add(&pdev->memory.heap_used[heap_index], (int64_t)chunk->size);
   *out = chunk;
   return VK_SUCCESS;
}

static VkResult
panvk_memory_pool_alloc(struct panvk_device *device, uint32_t memory_type_index,
                        VkDeviceSize size,
                        struct panvk_memory_pool_chunk **chunk_out,
                        uint64_t *offset_out, uint64_t *alloc_size_out)
{
   const uint64_t page_size = panvk_get_gpu_page_size(device);
   if (size > UINT64_MAX - (page_size - 1))
      return VK_ERROR_OUT_OF_DEVICE_MEMORY;

   const uint64_t alloc_size = ALIGN_POT(size, page_size);
   if (alloc_size > PANVK_MEMORY_POOL_MAX_ALLOC_SIZE)
      return VK_ERROR_OUT_OF_DEVICE_MEMORY;

   simple_mtx_lock(&device->memory_pool.lock);
   struct panvk_memory_pool_chunk *chunk = NULL;
   uint64_t offset = 0;
   list_for_each_entry(struct panvk_memory_pool_chunk, candidate,
                       &device->memory_pool.chunks, node)
   {
      if (candidate->memory_type_index != memory_type_index)
         continue;
      offset = util_vma_heap_alloc(
         &candidate->heap, alloc_size,
         MAX2(page_size, PANVK_MEMORY_POOL_MIN_ALIGNMENT));
      if (offset) {
         chunk = candidate;
         break;
      }
   }

   VkResult result = VK_SUCCESS;
   if (!chunk) {
      result = panvk_memory_pool_create_chunk(device, memory_type_index,
                                              &chunk);
      if (result == VK_SUCCESS) {
         offset = util_vma_heap_alloc(
            &chunk->heap, alloc_size,
            MAX2(page_size, PANVK_MEMORY_POOL_MIN_ALIGNMENT));
         if (!offset)
            result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
      }
   }

   if (result == VK_SUCCESS) {
      *chunk_out = chunk;
      *offset_out = offset;
      *alloc_size_out = alloc_size;
   }
   simple_mtx_unlock(&device->memory_pool.lock);
   return result;
}

static void
panvk_memory_pool_free(struct panvk_device *device,
                       struct panvk_device_memory *mem)
{
   struct panvk_memory_pool_chunk *chunk = mem->pool_chunk;
   simple_mtx_lock(&device->memory_pool.lock);
   util_vma_heap_free(&chunk->heap, mem->bo_offset, mem->bo_alloc_size);
   mem->pool_chunk = NULL;

   unsigned empty_chunks = 0;
   list_for_each_entry(struct panvk_memory_pool_chunk, candidate,
                       &device->memory_pool.chunks, node)
   {
      if (candidate->memory_type_index == chunk->memory_type_index &&
          candidate->heap.free_size == candidate->usable_size)
         empty_chunks++;
   }

   if (chunk->heap.free_size == chunk->usable_size && empty_chunks > 1)
      panvk_memory_pool_destroy_chunk(device, chunk);
   simple_mtx_unlock(&device->memory_pool.lock);
}

static bool
panvk_memory_pool_eligible(struct panvk_device *device,
                           const VkMemoryAllocateInfo *alloc_info,
                           const VkMemoryType *type, bool can_be_exported,
                           const VkImportMemoryFdInfoKHR *fd_info,
                           VkMemoryAllocateFlags alloc_flags)
{
   const VkMemoryDedicatedAllocateInfo *dedicated =
      vk_find_struct_const(alloc_info->pNext, MEMORY_DEDICATED_ALLOCATE_INFO);
   return alloc_info->allocationSize <= PANVK_MEMORY_POOL_MAX_ALLOC_SIZE &&
          (type->propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) &&
          !(type->propertyFlags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                   VK_MEMORY_PROPERTY_PROTECTED_BIT |
                                   VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT)) &&
          !can_be_exported && !fd_info && !dedicated &&
          !(alloc_flags &
            VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_CAPTURE_REPLAY_BIT) &&
          !device->vk.memory_reports && !device->debug.decode_ctx &&
          panvk_sparse_va_heap(device) != PANVK_FIXED_VA_HEAP;
}

static void
panvk_memory_emit_report(struct panvk_device *device,
                         const struct panvk_device_memory *mem,
                         const VkMemoryAllocateInfo *alloc_info,
                         VkResult result)
{
   struct panvk_physical_device *pdev =
      to_panvk_physical_device(device->vk.physical);

   const bool is_alloc = alloc_info != NULL;

   if (result != VK_SUCCESS) {
      const uint32_t heap_index =
         pdev->memory.types[alloc_info->memoryTypeIndex].heapIndex;
      vk_device_memory_report_emit(&device->vk, result, is_alloc,
                                   /* is_import */ false,
                                   /* mem_obj_id */ 0,
                                   alloc_info->allocationSize,
                                   VK_OBJECT_TYPE_DEVICE_MEMORY,
                                   /* obj_handle */ 0, heap_index);
      return;
   }

   const uint32_t heap_index =
      pdev->memory.types[mem->vk.memory_type_index].heapIndex;
   vk_device_memory_report_emit(
      &device->vk, result, is_alloc, mem->vk.import_handle_type != 0,
      mem->bo->handle, mem->bo->size, VK_OBJECT_TYPE_DEVICE_MEMORY,
      (uintptr_t)(mem), heap_index);
}

VKAPI_ATTR VkResult VKAPI_CALL
panvk_AllocateMemory(VkDevice _device,
                     const VkMemoryAllocateInfo *pAllocateInfo,
                     const VkAllocationCallbacks *pAllocator,
                     VkDeviceMemory *pMem)
{
   if (panvk_android_is_ahb_memory(pAllocateInfo)) {
      return panvk_android_allocate_ahb_memory(_device, pAllocateInfo,
                                               pAllocator, pMem);
   }

   VK_FROM_HANDLE(panvk_device, device, _device);
   struct panvk_physical_device *physical_device =
      to_panvk_physical_device(device->vk.physical);
   struct panvk_device_memory *mem;
   bool can_be_exported = false;
   VkResult result;

   assert(pAllocateInfo->sType == VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO);

   const VkMemoryType *type =
      &physical_device->memory.types[pAllocateInfo->memoryTypeIndex];

   const VkExportMemoryAllocateInfo *export_info =
      vk_find_struct_const(pAllocateInfo->pNext, EXPORT_MEMORY_ALLOCATE_INFO);

   if (export_info) {
      if (export_info->handleTypes &
          ~(VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT |
            VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT))
         return panvk_error(device, VK_ERROR_INVALID_EXTERNAL_HANDLE);
      else if (export_info->handleTypes)
         can_be_exported = true;
   }

   mem = vk_device_memory_create(&device->vk, pAllocateInfo, pAllocator,
                                 sizeof(*mem));
   if (mem == NULL)
      return panvk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   const VkImportMemoryFdInfoKHR *fd_info =
      vk_find_struct_const(pAllocateInfo->pNext, IMPORT_MEMORY_FD_INFO_KHR);

   if (fd_info && !fd_info->handleType)
      fd_info = NULL;

   if (panvk_memory_pool_eligible(device, pAllocateInfo, type, can_be_exported,
                                  fd_info, mem->vk.alloc_flags)) {
      struct panvk_memory_pool_chunk *chunk;
      uint64_t offset, alloc_size;
      result = panvk_memory_pool_alloc(device, pAllocateInfo->memoryTypeIndex,
                                       pAllocateInfo->allocationSize, &chunk,
                                       &offset, &alloc_size);
      if (result == VK_SUCCESS) {
         mem->bo = chunk->bo;
         mem->pool_chunk = chunk;
         mem->bo_offset = offset;
         mem->bo_alloc_size = alloc_size;
         mem->addr.dev = chunk->dev_addr + offset;
         panvk_address_binding_report(device, &mem->vk.base, mem->addr.dev,
                                      mem->vk.size,
                                      VK_DEVICE_ADDRESS_BINDING_TYPE_BIND_EXT);
         *pMem = panvk_device_memory_to_handle(mem);
         return VK_SUCCESS;
      }
   }

   if (fd_info) {
      assert(
         fd_info->handleType == VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT ||
         fd_info->handleType == VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT);

      mem->bo = pan_kmod_bo_import(device->kmod.dev, fd_info->fd);
      if (!mem->bo) {
         result = panvk_error(device, VK_ERROR_INVALID_EXTERNAL_HANDLE);
         goto err_destroy_mem;
      }
   } else {
      uint32_t bo_flags = 0;

      /* We don't do cached on exported buffers to keep the pre-WB_MMAP
       * behavior.
       */
      if (!can_be_exported &&
          (type->propertyFlags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT))
         bo_flags |= PAN_KMOD_BO_FLAG_WB_MMAP;

      /* A placed map aliases the BO at a caller-chosen address; on kbase
       * only shareable (dma-buf backed) BOs can be mapped twice. */
      bool shareable =
         can_be_exported ||
         (device->vk.enabled_features.memoryMapPlaced &&
          (type->propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT));

      bo_flags = panvk_device_adjust_bo_flags(device, bo_flags);
      mem->bo = pan_kmod_bo_alloc(device->kmod.dev,
                                  shareable ? NULL : device->kmod.vm,
                                  pAllocateInfo->allocationSize, bo_flags);
      if (!mem->bo) {
         result = panvk_error(device, VK_ERROR_OUT_OF_DEVICE_MEMORY);
         goto err_destroy_mem;
      }
   }

   /* Always GPU-map at creation time. */
   struct pan_kmod_vm_op op = {
      .type = PAN_KMOD_VM_OP_TYPE_MAP,
      .va = {
         .start = PAN_KMOD_VM_MAP_AUTO_VA,
         .size = pan_kmod_bo_size(mem->bo),
      },
      .map = {
         .bo = mem->bo,
         .bo_offset = 0,
      },
   };

   if (!(device->kmod.vm->flags & PAN_KMOD_VM_FLAG_AUTO_VA)) {
      uint64_t alignment =
         pan_choose_gpu_va_alignment(device->kmod.vm, op.va.size);
      unsigned arch = pan_arch(device->kmod.dev->props.gpu_id);
      /* For sizes larger than or equal to 64k, align the VA on 64k to meet the
       * requirement for interleaved_64k images (added in v10). */
      if (arch >= 10 && op.va.size >= 64 * 1024)
         alignment = MAX2(alignment, 64 * 1024);

      if (unlikely(mem->vk.alloc_flags &
                   VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_CAPTURE_REPLAY_BIT)) {
         const VkMemoryOpaqueCaptureAddressAllocateInfo *capture_alloc_info =
            vk_find_struct_const(pAllocateInfo->pNext,
                                 MEMORY_OPAQUE_CAPTURE_ADDRESS_ALLOCATE_INFO);
         if (capture_alloc_info == NULL ||
             capture_alloc_info->opaqueCaptureAddress == 0) {
            op.va.start = panvk_as_alloc(device, PANVK_FIXED_VA_HEAP,
                                         op.va.size, alignment);
         } else {
            op.va.start = panvk_as_alloc_fixed_address(
               device, capture_alloc_info->opaqueCaptureAddress, op.va.size);
         }
      } else {
         op.va.start = panvk_as_alloc(device, PANVK_NO_EXEC_VA_HEAP, op.va.size,
                                      alignment);
      }

      if (!op.va.start) {
         result = panvk_error(device, VK_ERROR_OUT_OF_DEVICE_MEMORY);
         goto err_put_bo;
      }
   }

   int ret =
      pan_kmod_vm_bind(device->kmod.vm, PAN_KMOD_VM_OP_MODE_IMMEDIATE, &op, 1);
   if (ret) {
      result = panvk_error(device, VK_ERROR_OUT_OF_DEVICE_MEMORY);
      goto err_return_va;
   }

   mem->addr.dev = op.va.start;

   panvk_address_binding_report(device, &mem->vk.base, mem->addr.dev,
                                pan_kmod_bo_size(mem->bo),
                                VK_DEVICE_ADDRESS_BINDING_TYPE_BIND_EXT);

   if (fd_info) {
      /* From the Vulkan spec:
       *
       *    "Importing memory from a file descriptor transfers ownership of
       *    the file descriptor from the application to the Vulkan
       *    implementation. The application must not perform any operations on
       *    the file descriptor after a successful import."
       *
       * If the import fails, we leave the file descriptor open.
       */
      close(fd_info->fd);
   }

   if (device->debug.decode_ctx) {
      if (PANVK_DEBUG(DUMP) || PANVK_DEBUG(TRACE)) {
         void *cpu =
            pan_kmod_bo_mmap(mem->bo, PROT_READ | PROT_WRITE, MAP_SHARED, NULL);
         if (cpu != MAP_FAILED)
            mem->debug.host_mapping = cpu;
         else
            vk_logw(VK_LOG_OBJS(_device),
                    "failed to map VkMemory for dump or trace.\n");
      }

      pandecode_inject_mmap(device->debug.decode_ctx, mem->addr.dev,
                            mem->debug.host_mapping, pan_kmod_bo_size(mem->bo),
                            NULL);
   }

   panvk_memory_emit_report(device, mem, pAllocateInfo, VK_SUCCESS);

   p_atomic_add(&physical_device->memory.heap_used[type->heapIndex],
                (uint64_t)pan_kmod_bo_size(mem->bo));

   *pMem = panvk_device_memory_to_handle(mem);

   return VK_SUCCESS;

err_return_va:
   if (!(device->kmod.vm->flags & PAN_KMOD_VM_FLAG_AUTO_VA)) {
      panvk_as_free(device, op.va.start, op.va.size);
   }

err_put_bo:
   pan_kmod_bo_put(mem->bo);

err_destroy_mem:
   vk_device_memory_destroy(&device->vk, pAllocator, &mem->vk);
   panvk_memory_emit_report(device, /* mem */ NULL, pAllocateInfo, result);

   return result;
}

VKAPI_ATTR void VKAPI_CALL
panvk_FreeMemory(VkDevice _device, VkDeviceMemory _mem,
                 const VkAllocationCallbacks *pAllocator)
{
   VK_FROM_HANDLE(panvk_device, device, _device);
   VK_FROM_HANDLE(panvk_device_memory, mem, _mem);

   if (mem == NULL)
      return;

   struct panvk_physical_device *physical_device =
      to_panvk_physical_device(device->vk.physical);

   if (mem->pool_chunk) {
      panvk_address_binding_report(device, &mem->vk.base, mem->addr.dev,
                                   mem->vk.size,
                                   VK_DEVICE_ADDRESS_BINDING_TYPE_UNBIND_EXT);
      panvk_memory_pool_free(device, mem);
      vk_device_memory_destroy(&device->vk, pAllocator, &mem->vk);
      return;
   }

   const uint32_t heap_index =
      physical_device->memory.types[mem->vk.memory_type_index].heapIndex;
   p_atomic_add(&physical_device->memory.heap_used[heap_index],
                -((int64_t)pan_kmod_bo_size(mem->bo)));

   if (device->debug.decode_ctx) {
      pandecode_inject_free(device->debug.decode_ctx, mem->addr.dev,
                            pan_kmod_bo_size(mem->bo));

      if (mem->debug.host_mapping)
         pan_kmod_bo_munmap(mem->bo, mem->debug.host_mapping,
                            pan_kmod_bo_size(mem->bo));
   }

   if (mem->addr.host) {
      ASSERTED int ret = pan_kmod_bo_munmap(mem->bo, mem->addr.host,
                                            pan_kmod_bo_size(mem->bo));
      assert(!ret);
   }

   panvk_address_binding_report(device, &mem->vk.base, mem->addr.dev,
                                pan_kmod_bo_size(mem->bo),
                                VK_DEVICE_ADDRESS_BINDING_TYPE_UNBIND_EXT);

   struct pan_kmod_vm_op op = {
      .type = PAN_KMOD_VM_OP_TYPE_UNMAP,
      .va = {
         .start = mem->addr.dev,
         .size = pan_kmod_bo_size(mem->bo),
      },
   };

   ASSERTED int ret =
      pan_kmod_vm_bind(device->kmod.vm, PAN_KMOD_VM_OP_MODE_IMMEDIATE, &op, 1);
   assert(!ret);

   if (!(device->kmod.vm->flags & PAN_KMOD_VM_FLAG_AUTO_VA))
      panvk_as_free(device, op.va.start, op.va.size);

   panvk_memory_emit_report(device, mem, /* alloc_info */ NULL, VK_SUCCESS);

   pan_kmod_bo_put(mem->bo);
   vk_device_memory_destroy(&device->vk, pAllocator, &mem->vk);
}

VKAPI_ATTR VkResult VKAPI_CALL
panvk_MapMemory2KHR(VkDevice _device, const VkMemoryMapInfoKHR *pMemoryMapInfo,
                    void **ppData)
{
   VK_FROM_HANDLE(panvk_device, device, _device);
   VK_FROM_HANDLE(panvk_device_memory, mem, pMemoryMapInfo->memory);

   if (mem == NULL) {
      *ppData = NULL;
      return VK_SUCCESS;
   }

   const VkDeviceSize offset = pMemoryMapInfo->offset;
   const VkDeviceSize size = vk_device_memory_range(
      &mem->vk, pMemoryMapInfo->offset, pMemoryMapInfo->size);

   /* From the Vulkan spec version 1.0.32 docs for MapMemory:
    *
    *  * If size is not equal to VK_WHOLE_SIZE, size must be greater than 0
    *    assert(size != 0);
    *  * If size is not equal to VK_WHOLE_SIZE, size must be less than or
    *    equal to the size of the memory minus offset
    */
   assert(size > 0);
   assert(offset + size <= mem->bo->size);

   if (size != (size_t)size) {
      return panvk_errorf(device, VK_ERROR_MEMORY_MAP_FAILED,
                          "requested size 0x%" PRIx64
                          " does not fit in %u bits",
                          size, (unsigned)(sizeof(size_t) * 8));
   }

   /* From the Vulkan 1.2.194 spec:
    *
    *    "memory must not be currently host mapped"
    */
   if (mem->addr.host)
      return panvk_errorf(device, VK_ERROR_MEMORY_MAP_FAILED,
                          "Memory object already mapped.");

   void *host_addr = NULL;
   int map_flags = MAP_SHARED;

   if (pMemoryMapInfo->flags & VK_MEMORY_MAP_PLACED_BIT_EXT) {
      const VkMemoryMapPlacedInfoEXT *placed_info = vk_find_struct_const(
         pMemoryMapInfo->pNext, MEMORY_MAP_PLACED_INFO_EXT);
      assert(placed_info != NULL);
      assert(offset == 0 && size == mem->bo->size);
      host_addr = placed_info->pPlacedAddress;
      map_flags |= MAP_FIXED;
   }

   void *addr =
      pan_kmod_bo_mmap(mem->bo, PROT_READ | PROT_WRITE, map_flags, host_addr);
   if (addr == MAP_FAILED)
      return panvk_errorf(device, VK_ERROR_MEMORY_MAP_FAILED,
                          "Memory object couldn't be mapped.");

   assert(!host_addr || addr == host_addr);
   mem->addr.host = addr;
   *ppData = (char *)addr + offset;

   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
panvk_UnmapMemory2KHR(VkDevice _device,
                      const VkMemoryUnmapInfoKHR *pMemoryUnmapInfo)
{
   VK_FROM_HANDLE(panvk_device, device, _device);
   VK_FROM_HANDLE(panvk_device_memory, mem, pMemoryUnmapInfo->memory);

   if (pMemoryUnmapInfo->flags & VK_MEMORY_UNMAP_RESERVE_BIT_EXT) {
      void *reserved =
         os_mmap(mem->addr.host, pan_kmod_bo_size(mem->bo), PROT_NONE,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
      if (reserved == MAP_FAILED)
         return panvk_errorf(device, VK_ERROR_MEMORY_MAP_FAILED,
                             "Failed to reserve VA on unmap.");
   } else {
      ASSERTED int ret = pan_kmod_bo_munmap(mem->bo, mem->addr.host,
                                            pan_kmod_bo_size(mem->bo));
      assert(!ret);
   }

   mem->addr.host = NULL;

   return VK_SUCCESS;
}

static VkResult
sync_memory_ranges(struct panvk_device *device, uint32_t memoryRangeCount,
                   const VkMappedMemoryRange *pMemoryRanges,
                   enum pan_kmod_bo_sync_type type)
{
   for (uint32_t i = 0; i < memoryRangeCount; i++) {
      const VkMappedMemoryRange *r = &pMemoryRanges[i];
      if (r->size == 0)
         continue;

      VK_FROM_HANDLE(panvk_device_memory, memory, r->memory);

      pan_kmod_queue_bo_map_sync(
         memory->bo, r->offset, (char *)memory->addr.host + r->offset,
         vk_device_memory_range(&memory->vk, r->offset, r->size), type);
   }

   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
panvk_FlushMappedMemoryRanges(VkDevice _device, uint32_t memoryRangeCount,
                              const VkMappedMemoryRange *pMemoryRanges)
{
   VK_FROM_HANDLE(panvk_device, device, _device);

   return sync_memory_ranges(device, memoryRangeCount, pMemoryRanges,
                             PAN_KMOD_BO_SYNC_CPU_CACHE_FLUSH);
}

VKAPI_ATTR VkResult VKAPI_CALL
panvk_InvalidateMappedMemoryRanges(VkDevice _device, uint32_t memoryRangeCount,
                                   const VkMappedMemoryRange *pMemoryRanges)
{
   VK_FROM_HANDLE(panvk_device, device, _device);
   VkResult result =
      sync_memory_ranges(device, memoryRangeCount, pMemoryRanges,
                         PAN_KMOD_BO_SYNC_CPU_CACHE_FLUSH_AND_INVALIDATE);

   if (result == VK_SUCCESS)
      pan_kmod_flush_bo_map_syncs(device->kmod.dev);

   return result;
}

VKAPI_ATTR VkResult VKAPI_CALL
panvk_GetMemoryFdKHR(VkDevice _device, const VkMemoryGetFdInfoKHR *pGetFdInfo,
                     int *pFd)
{
   VK_FROM_HANDLE(panvk_device, device, _device);
   VK_FROM_HANDLE(panvk_device_memory, memory, pGetFdInfo->memory);

   assert(pGetFdInfo->sType == VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR);

   /* At the moment, we support only the below handle types. */
   assert(
      pGetFdInfo->handleType == VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT ||
      pGetFdInfo->handleType == VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT);

   int prime_fd = pan_kmod_bo_export(memory->bo);
   if (prime_fd < 0)
      return panvk_error(device, VK_ERROR_OUT_OF_DEVICE_MEMORY);

   *pFd = prime_fd;
   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
panvk_GetMemoryFdPropertiesKHR(VkDevice _device,
                               VkExternalMemoryHandleTypeFlagBits handleType,
                               int fd,
                               VkMemoryFdPropertiesKHR *pMemoryFdProperties)
{
   VK_FROM_HANDLE(panvk_device, device, _device);
   const struct panvk_physical_device *phys_dev =
      to_panvk_physical_device(device->vk.physical);

   assert(handleType == VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT);

#if defined(HAVE_PAN_KMOD_KBASE)
   /* On kbase, a full transient import per query is not free: repeated
    * import/free cycles of one dma-buf alias to a single GPU VA, and the
    * frees can tear down GPU PTEs that re-import does not restore,
    * breaking the later real import (GPU TRANSLATION_FAULT). Answer from
    * the same assumptions the fd-import path uses instead.
    */
   if (phys_dev->kbase_node_path[0]) {
      if (fd < 0 || lseek(fd, 0, SEEK_END) < 0)
         return VK_ERROR_INVALID_EXTERNAL_HANDLE;

      /* Matches kbase_kmod_bo_import_fd(..., kmod_flags=0, external): */
      const bool can_do_host_visible = true;
      const bool can_do_host_coherent = true;
      const bool can_do_host_cached = false;
      pMemoryFdProperties->memoryTypeBits = 0;
      for (uint32_t i = 0; i < phys_dev->memory.type_count; i++) {
         if (!can_do_host_visible && (phys_dev->memory.types[i].propertyFlags &
                                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))
            continue;
         if (!can_do_host_coherent && (phys_dev->memory.types[i].propertyFlags &
                                       VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
            continue;
         if (!can_do_host_cached && (phys_dev->memory.types[i].propertyFlags &
                                     VK_MEMORY_PROPERTY_HOST_CACHED_BIT))
            continue;
         pMemoryFdProperties->memoryTypeBits |= BITFIELD_BIT(i);
      }
      return VK_SUCCESS;
   }
#endif

   struct pan_kmod_bo *bo = pan_kmod_bo_import(device->kmod.dev, fd);
   if (!bo)
      return VK_ERROR_INVALID_EXTERNAL_HANDLE;

   pMemoryFdProperties->memoryTypeBits = 0;

   /* Keep things simple by only allowing host-visible if the BO doesn't require
    * kernel-side synchronization going through the dma-buf exporter, which is
    * reflected through the PAN_KMOD_BO_FLAG_FORCE_FULL_KERNEL_SYNC flag.
    */
   const bool can_do_host_visible = !(bo->flags & PAN_KMOD_BO_FLAG_NO_MMAP);
   const bool can_do_host_coherent = !(bo->flags & PAN_KMOD_BO_FLAG_WB_MMAP) ||
                                     (bo->flags & PAN_KMOD_BO_FLAG_IO_COHERENT);
   const bool can_do_host_cached = (bo->flags & PAN_KMOD_BO_FLAG_WB_MMAP);

   pMemoryFdProperties->memoryTypeBits = 0;
   for (uint32_t i = 0; i < phys_dev->memory.type_count; i++) {
      if (!can_do_host_visible && (phys_dev->memory.types[i].propertyFlags &
                                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))
         continue;
      if (!can_do_host_coherent && (phys_dev->memory.types[i].propertyFlags &
                                    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
         continue;
      if (!can_do_host_cached && (phys_dev->memory.types[i].propertyFlags &
                                  VK_MEMORY_PROPERTY_HOST_CACHED_BIT))
         continue;

      pMemoryFdProperties->memoryTypeBits |= BITFIELD_BIT(i);
   }

   pan_kmod_bo_put(bo);
   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
panvk_GetDeviceMemoryCommitment(VkDevice device, VkDeviceMemory memory,
                                VkDeviceSize *pCommittedMemoryInBytes)
{
   *pCommittedMemoryInBytes = 0;
}

VKAPI_ATTR uint64_t VKAPI_CALL
panvk_GetDeviceMemoryOpaqueCaptureAddress(
   VkDevice _device, const VkDeviceMemoryOpaqueCaptureAddressInfo *pInfo)
{
   VK_FROM_HANDLE(panvk_device_memory, memory, pInfo->memory);

   return memory->addr.dev;
}

VKAPI_ATTR void VKAPI_CALL
panvk_SetDeviceMemoryPriorityEXT(VkDevice device, VkDeviceMemory memory,
                                 float priority)
{
}
