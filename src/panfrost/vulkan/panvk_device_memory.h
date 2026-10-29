/*
 * Copyright © 2021 Collabora Ltd.
 * SPDX-License-Identifier: MIT
 */

#ifndef PANVK_DEVICE_MEMORY_H
#define PANVK_DEVICE_MEMORY_H

#include <stdint.h>

#include "vk_device_memory.h"

struct panvk_priv_bo;
struct panvk_device;
struct panvk_memory_pool_chunk;

void panvk_memory_pool_init(struct panvk_device *device);
void panvk_memory_pool_cleanup(struct panvk_device *device);

struct panvk_device_memory {
   struct vk_device_memory vk;
   struct pan_kmod_bo *bo;
   struct panvk_memory_pool_chunk *pool_chunk;
   uint64_t bo_offset;
   uint64_t bo_alloc_size;
   struct {
      uint64_t dev;
      void *host;
   } addr;

   struct {
      /* Don't use this pointer, it's only to have user memory dumped when
       * PANVK_DEBUG=dump. */
      void *host_mapping;
   } debug;
};

VK_DEFINE_NONDISP_HANDLE_CASTS(panvk_device_memory, vk.base, VkDeviceMemory,
                               VK_OBJECT_TYPE_DEVICE_MEMORY)

#endif
