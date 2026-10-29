/*
 * Copyright © 2021 Collabora Ltd.
 * Copyright © 2025 Arm Ltd.
 *
 * Derived from tu_wsi.c:
 * Copyright © 2016 Red Hat
 * Copyright © 2015 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 */

#include "panvk_wsi.h"
#include "panvk_instance.h"
#include "panvk_physical_device.h"
#include "vk_fence.h"

#include <stdlib.h>
#include <string.h>

#if defined(HAVE_PAN_KMOD_KBASE)
#include "lib/kmod/kbase_kmod.h"
#endif

#include "vk_util.h"
#include "wsi_common.h"
#include "util/os_time.h"

static VKAPI_PTR PFN_vkVoidFunction
panvk_wsi_proc_addr(VkPhysicalDevice physicalDevice, const char *pName)
{
   VK_FROM_HANDLE(panvk_physical_device, pdevice, physicalDevice);
   struct panvk_instance *instance = to_panvk_instance(pdevice->vk.instance);

   return vk_instance_get_proc_addr_unchecked(&instance->vk, pName);
}

static bool
panvk_can_present_on_device(VkPhysicalDevice pdevice, int fd)
{
   drmDevicePtr device;
   if (drmGetDevice2(fd, 0, &device) != 0)
      return false;
   /* Allow on-device presentation for all devices with bus type PLATFORM.
    * Other device types such as PCI or USB should use the PRIME blit path. */
   bool match = device->bustype == DRM_BUS_PLATFORM;

   drmFreeDevice(&device);

   return match;
}

#ifdef VK_USE_PLATFORM_ANDROID_KHR
#include <android/hardware_buffer.h>
#include <system/window.h>
#include <android/native_window.h>
#include <unistd.h>
#include "util/libsync.h"
#include "wsi_common_private.h"
#include "panvk_android.h"
#include "panvk_device.h"
#include "panvk_entrypoints.h"
#include "panvk_image.h"
#include "vk_object.h"

#define PANVK_ANDROID_MAX_SWAPCHAIN_BUFFERS 8

AHardwareBuffer *
ANativeWindowBuffer_getHardwareBuffer(struct ANativeWindowBuffer *buffer);

struct panvk_android_swapchain_image {
   struct wsi_image base;
   struct ANativeWindowBuffer *buffer;
   AHardwareBuffer *ahb;
   int acquire_fence_fd;
   bool dequeued;
   bool initial_dequeue;
};

struct panvk_android_surface {
   VkIcdSurfaceAndroid base;
   bool window_connected;
   uint32_t swapchain_count;
};

struct panvk_android_swapchain {
   struct wsi_swapchain base;
   struct panvk_android_surface *surface;
   ANativeWindow *window;
   bool surface_registered;
   uint64_t completed_present_id;
   struct panvk_android_swapchain_image
      images[PANVK_ANDROID_MAX_SWAPCHAIN_BUFFERS];
};

VKAPI_ATTR void VKAPI_CALL
wsi_DestroySurfaceKHR(VkInstance _instance,
                      VkSurfaceKHR _surface,
                      const VkAllocationCallbacks *pAllocator);

static VkResult
panvk_android_surface_connect(VkIcdSurfaceAndroid *surface)
{
   struct panvk_android_surface *panvk_surface =
      (struct panvk_android_surface *)surface;
   if (panvk_surface->window_connected)
      return VK_SUCCESS;

   int result =
      native_window_api_connect(surface->window, NATIVE_WINDOW_API_EGL);
   if (result != 0) {
      mesa_loge("panvk: Android surface producer connect failed (%d)", result);
      return VK_ERROR_SURFACE_LOST_KHR;
   }

   panvk_surface->window_connected = true;
   return VK_SUCCESS;
}

static VkResult
panvk_android_surface_disconnect(struct panvk_android_surface *surface)
{
   if (!surface->window_connected)
      return VK_SUCCESS;

   int result = native_window_api_disconnect(surface->base.window,
                                             NATIVE_WINDOW_API_EGL);
   if (result != 0) {
      mesa_loge("panvk: Android surface producer disconnect failed (%d)",
                result);
      return VK_ERROR_SURFACE_LOST_KHR;
   }

   surface->window_connected = false;
   return VK_SUCCESS;
}

static VkResult
panvk_wsi_android_get_support(VkIcdSurfaceBase *surface,
                              struct wsi_device *wsi_device,
                              uint32_t queueFamilyIndex,
                              VkBool32 *pSupported)
{
   *pSupported = VK_TRUE;
   return VK_SUCCESS;
}

static VkResult
panvk_wsi_android_get_capabilities2(VkIcdSurfaceBase *surface,
                                    struct wsi_device *wsi_device,
                                    const void *info_next,
                                    VkSurfaceCapabilities2KHR *pSurfaceCapabilities)
{
   VkIcdSurfaceAndroid *s = (VkIcdSurfaceAndroid *)surface;
   int min_undequeued = 0;
   if (!s->window)
      return VK_ERROR_SURFACE_LOST_KHR;

   int query_result =
      s->window->query(s->window, NATIVE_WINDOW_MIN_UNDEQUEUED_BUFFERS,
                       &min_undequeued);
   if (query_result != 0) {
      VkResult connect_result = panvk_android_surface_connect(s);
      if (connect_result != VK_SUCCESS)
         return connect_result;
      query_result =
         s->window->query(s->window, NATIVE_WINDOW_MIN_UNDEQUEUED_BUFFERS,
                          &min_undequeued);
   }
   if (query_result != 0 || min_undequeued < 0 ||
       min_undequeued >= PANVK_ANDROID_MAX_SWAPCHAIN_BUFFERS)
      return VK_ERROR_SURFACE_LOST_KHR;

   int w = ANativeWindow_getWidth(s->window);
   int h = ANativeWindow_getHeight(s->window);
   if (w <= 0) w = 64;
   if (h <= 0) h = 64;

   pSurfaceCapabilities->surfaceCapabilities = (VkSurfaceCapabilitiesKHR){
      .minImageCount = 2,
      .maxImageCount = PANVK_ANDROID_MAX_SWAPCHAIN_BUFFERS - min_undequeued,
      .currentExtent = {(uint32_t)w, (uint32_t)h},
      .minImageExtent = {1, 1},
      .maxImageExtent = {4096, 4096},
      .maxImageArrayLayers = 1,
      .supportedTransforms = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
      .currentTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
      .supportedCompositeAlpha = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR |
                                 VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
      .supportedUsageFlags = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                             VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                             VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                             VK_IMAGE_USAGE_SAMPLED_BIT,
   };

   const VkSurfacePresentModeKHR *mode =
      vk_find_struct_const(info_next, SURFACE_PRESENT_MODE_KHR);
   vk_foreach_struct(sType, ext, pSurfaceCapabilities->pNext) {
      if (sType == VK_STRUCTURE_TYPE_SURFACE_PRESENT_MODE_COMPATIBILITY_KHR) {
         VkSurfacePresentModeCompatibilityKHR *compat = (void *)ext;
         const VkPresentModeKHR present_mode =
            mode ? mode->presentMode : VK_PRESENT_MODE_FIFO_KHR;
         VK_OUTARRAY_MAKE_TYPED(VkPresentModeKHR, out,
                                compat->pPresentModes,
                                &compat->presentModeCount);
         vk_outarray_append_typed(VkPresentModeKHR, &out, compatible_mode) {
            *compatible_mode = present_mode;
         }
      }
   }
   return VK_SUCCESS;
}

static VkResult
panvk_wsi_android_get_formats(VkIcdSurfaceBase *surface,
                              struct wsi_device *wsi_device,
                              uint32_t *pSurfaceFormatCount,
                              VkSurfaceFormatKHR *pSurfaceFormats)
{
   static const VkSurfaceFormatKHR formats[] = {
      { VK_FORMAT_R8G8B8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR },
   };
   VK_OUTARRAY_MAKE_TYPED(VkSurfaceFormatKHR, out, pSurfaceFormats, pSurfaceFormatCount);
   for (unsigned i = 0; i < ARRAY_SIZE(formats); i++) {
      vk_outarray_append_typed(VkSurfaceFormatKHR, &out, f) {
         *f = formats[i];
      }
   }
   return vk_outarray_status(&out);
}

static VkResult
panvk_wsi_android_get_formats2(VkIcdSurfaceBase *surface,
                               struct wsi_device *wsi_device,
                               const void *info_next,
                               uint32_t *pSurfaceFormatCount,
                               VkSurfaceFormat2KHR *pSurfaceFormats)
{
   static const VkSurfaceFormatKHR formats[] = {
      { VK_FORMAT_R8G8B8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR },
   };
   VK_OUTARRAY_MAKE_TYPED(VkSurfaceFormat2KHR, out, pSurfaceFormats, pSurfaceFormatCount);
   for (unsigned i = 0; i < ARRAY_SIZE(formats); i++) {
      vk_outarray_append_typed(VkSurfaceFormat2KHR, &out, f) {
         f->surfaceFormat = formats[i];
      }
   }
   return vk_outarray_status(&out);
}

static VkResult
panvk_wsi_android_get_present_modes(VkIcdSurfaceBase *surface,
                                    struct wsi_device *wsi_device,
                                    uint32_t *pPresentModeCount,
                                    VkPresentModeKHR *pPresentModes)
{
   static const VkPresentModeKHR modes[] = {
      VK_PRESENT_MODE_FIFO_KHR,
      VK_PRESENT_MODE_MAILBOX_KHR,
      VK_PRESENT_MODE_IMMEDIATE_KHR,
   };
   VK_OUTARRAY_MAKE_TYPED(VkPresentModeKHR, out, pPresentModes, pPresentModeCount);
   for (unsigned i = 0; i < ARRAY_SIZE(modes); i++) {
      vk_outarray_append_typed(VkPresentModeKHR, &out, m) {
         *m = modes[i];
      }
   }
   return vk_outarray_status(&out);
}

static VkResult
panvk_wsi_android_get_present_rectangles(VkIcdSurfaceBase *surface,
                                         struct wsi_device *wsi_device,
                                         uint32_t *pRectCount,
                                         VkRect2D *pRects)
{
   VkIcdSurfaceAndroid *s = (VkIcdSurfaceAndroid *)surface;
   int w = s->window ? ANativeWindow_getWidth(s->window) : 64;
   int h = s->window ? ANativeWindow_getHeight(s->window) : 64;
   if (w <= 0) w = 64;
   if (h <= 0) h = 64;

   VK_OUTARRAY_MAKE_TYPED(VkRect2D, out, pRects, pRectCount);
   vk_outarray_append_typed(VkRect2D, &out, r) {
      r->offset.x = 0;
      r->offset.y = 0;
      r->extent.width = (uint32_t)w;
      r->extent.height = (uint32_t)h;
   }
   return vk_outarray_status(&out);
}

static struct wsi_image *
panvk_android_swapchain_get_wsi_image(struct wsi_swapchain *swapchain,
                                     uint32_t image_index)
{
   struct panvk_android_swapchain *chain = (struct panvk_android_swapchain *)swapchain;
   return &chain->images[image_index].base;
}

static VkResult
panvk_android_swapchain_acquire_next_image(
   struct wsi_swapchain *swapchain,
   const VkAcquireNextImageInfoKHR *info,
   uint32_t *image_index)
{
   struct panvk_android_swapchain *chain =
      (struct panvk_android_swapchain *)swapchain;

   for (;;) {
      for (uint32_t i = 0; i < chain->base.image_count; i++) {
         struct panvk_android_swapchain_image *img = &chain->images[i];
         if (!img->initial_dequeue || img->base.acquired)
            continue;

         if (img->acquire_fence_fd >= 0) {
            if (sync_wait(img->acquire_fence_fd, -1) != 0) {
               chain->window->cancelBuffer(chain->window, img->buffer,
                                           img->acquire_fence_fd);
               img->dequeued = false;
               img->acquire_fence_fd = -1;
               return panvk_errorf(
                  swapchain->device, VK_ERROR_SURFACE_LOST_KHR,
                  "Waiting for Android acquire fence failed");
            }
            close(img->acquire_fence_fd);
            img->acquire_fence_fd = -1;
         }

         img->initial_dequeue = false;
         img->base.acquired = true;
         *image_index = i;
         return VK_SUCCESS;
      }

      if (info->timeout == 0)
         return VK_NOT_READY;

      struct ANativeWindowBuffer *buffer = NULL;
      int fence_fd = -1;
      int ret = chain->window->dequeueBuffer(chain->window, &buffer, &fence_fd);
      if (ret != 0)
         return panvk_errorf(swapchain->device, VK_ERROR_SURFACE_LOST_KHR,
                             "ANativeWindow_dequeueBuffer failed: %d", ret);

      if (fence_fd >= 0 && sync_wait(fence_fd, -1) != 0) {
         chain->window->cancelBuffer(chain->window, buffer, fence_fd);
         return panvk_errorf(swapchain->device, VK_ERROR_SURFACE_LOST_KHR,
                             "Waiting for Android acquire fence failed");
      }
      if (fence_fd >= 0)
         close(fence_fd);

      for (uint32_t i = 0; i < chain->base.image_count; i++) {
         struct panvk_android_swapchain_image *img = &chain->images[i];
         if (img->buffer != buffer || img->base.acquired)
            continue;

         img->dequeued = true;
         img->base.acquired = true;
         *image_index = i;
         return VK_SUCCESS;
      }

      chain->window->cancelBuffer(chain->window, buffer, -1);
      return panvk_errorf(swapchain->device, VK_ERROR_OUT_OF_DATE_KHR,
                          "Android window returned an unknown swapchain buffer");
   }
}

static VkResult
panvk_android_swapchain_release_images(struct wsi_swapchain *swapchain,
                                       uint32_t count,
                                       const uint32_t *indices)
{
   for (uint32_t i = 0; i < count; i++) {
      if (indices[i] >= swapchain->image_count)
         return VK_ERROR_OUT_OF_DATE_KHR;

      struct panvk_android_swapchain_image *img =
         &((struct panvk_android_swapchain *)swapchain)->images[indices[i]];
      if (img->dequeued) {
         VK_FROM_HANDLE(vk_device, dev, swapchain->device);
         VkFence fence = swapchain->fences[indices[i]];
         if (fence != VK_NULL_HANDLE) {
            VkResult result = dev->dispatch_table.WaitForFences(
               swapchain->device, 1, &fence, VK_TRUE, UINT64_MAX);
            if (result != VK_SUCCESS)
               return result;
         }

         ANativeWindow *window =
            ((struct panvk_android_swapchain *)swapchain)->window;
         int ret = window->cancelBuffer(window, img->buffer, -1);
         if (ret != 0)
            return panvk_errorf(swapchain->device,
                                VK_ERROR_SURFACE_LOST_KHR,
                                "ANativeWindow_cancelBuffer failed: %d", ret);
         img->dequeued = false;
      }
      img->base.acquired = false;
   }

   return VK_SUCCESS;
}

static VkResult
panvk_android_swapchain_wait_for_present2(struct wsi_swapchain *swapchain,
                                          uint64_t present_id,
                                          uint64_t timeout)
{
   struct panvk_android_swapchain *chain =
      (struct panvk_android_swapchain *)swapchain;

   return present_id <= chain->completed_present_id ? VK_SUCCESS : VK_TIMEOUT;
}

static VkResult
panvk_android_swapchain_queue_present(
   struct wsi_swapchain *swapchain,
   uint32_t image_index,
   uint64_t present_id,
   const VkPresentRegionKHR *damage)
{
   struct panvk_android_swapchain *chain = (struct panvk_android_swapchain *)swapchain;
   if (image_index >= chain->base.image_count)
      return VK_ERROR_OUT_OF_DATE_KHR;

   VkFence present_fence = swapchain->fences[image_index];
   if (present_fence == VK_NULL_HANDLE)
      return panvk_errorf(swapchain->device, VK_ERROR_INITIALIZATION_FAILED,
                          "Android present has no image fence");

   struct panvk_android_swapchain_image *img = &chain->images[image_index];
   if (!chain->window || !img->buffer || !img->dequeued)
      return panvk_errorf(swapchain->device, VK_ERROR_SURFACE_LOST_KHR,
                          "Android swapchain image is not dequeued");

   VK_FROM_HANDLE(vk_device, dev, swapchain->device);
   VK_FROM_HANDLE(vk_fence, fence, present_fence);
   int present_fence_fd = -1;
   VkResult result = vk_sync_export_sync_file(
      dev, vk_fence_get_active_sync(fence), &present_fence_fd);
   if (result != VK_SUCCESS) {
      if (present_fence_fd >= 0)
         close(present_fence_fd);
      present_fence_fd = -1;
      mesa_logw("Android present sync-file export failed (%d); "
                "falling back to a blocking fence wait",
                result);
      result = dev->dispatch_table.WaitForFences(
         swapchain->device, 1, &present_fence, VK_TRUE, UINT64_MAX);
      if (result != VK_SUCCESS)
         return result;
   }

   int ret = chain->window->queueBuffer(chain->window, img->buffer,
                                        present_fence_fd);
   if (ret != 0)
      return panvk_errorf(swapchain->device, VK_ERROR_SURFACE_LOST_KHR,
                          "ANativeWindow_queueBuffer failed: %d", ret);

   img->dequeued = false;
   img->base.acquired = false;

   chain->completed_present_id = MAX2(chain->completed_present_id, present_id);

   return VK_SUCCESS;
}

static VkResult
panvk_android_swapchain_destroy(
   struct wsi_swapchain *swapchain,
   const VkAllocationCallbacks *pAllocator)
{
   struct panvk_android_swapchain *chain = (struct panvk_android_swapchain *)swapchain;
   VK_FROM_HANDLE(vk_device, dev, swapchain->device);

   for (uint32_t i = 0; i < chain->base.image_count; i++) {
      if (chain->images[i].dequeued) {
         chain->window->cancelBuffer(chain->window, chain->images[i].buffer,
                                     chain->images[i].acquire_fence_fd);
         chain->images[i].dequeued = false;
         chain->images[i].acquire_fence_fd = -1;
      }
      if (chain->images[i].base.image != VK_NULL_HANDLE)
         dev->dispatch_table.DestroyImage(swapchain->device, chain->images[i].base.image, pAllocator);
      if (chain->images[i].base.memory != VK_NULL_HANDLE)
         dev->dispatch_table.FreeMemory(swapchain->device, chain->images[i].base.memory, pAllocator);
      if (chain->images[i].ahb)
         AHardwareBuffer_release(chain->images[i].ahb);
      if (chain->images[i].buffer)
         chain->images[i].buffer->common.decRef(
            &chain->images[i].buffer->common);
   }

   if (chain->window)
      ANativeWindow_release(chain->window);

   if (chain->surface_registered) {
      assert(chain->surface->swapchain_count > 0);
      chain->surface->swapchain_count--;
      if (chain->surface->swapchain_count == 0)
         panvk_android_surface_disconnect(chain->surface);
   }

   wsi_swapchain_finish(&chain->base);
   vk_free2(&dev->alloc, pAllocator, chain);
   return VK_SUCCESS;
}

static VkResult
panvk_wsi_android_create_swapchain(
   VkIcdSurfaceBase *surface,
   VkDevice device,
   struct wsi_device *wsi_device,
   const VkSwapchainCreateInfoKHR *pCreateInfo,
   const VkAllocationCallbacks *pAllocator,
   struct wsi_swapchain **swapchain_out)
{
    VK_FROM_HANDLE(vk_device, dev, device);
    const VkImageCompressionControlEXT *swapchain_compression =
       vk_find_struct_const(pCreateInfo->pNext, IMAGE_COMPRESSION_CONTROL_EXT);
    const VkImageFormatListCreateInfo *swapchain_format_list =
       vk_find_struct_const(pCreateInfo->pNext, IMAGE_FORMAT_LIST_CREATE_INFO);
    const bool mutable_format =
       (pCreateInfo->flags & VK_SWAPCHAIN_CREATE_MUTABLE_FORMAT_BIT_KHR) != 0;
    if (swapchain_format_list &&
        (!swapchain_format_list->viewFormatCount ||
         !swapchain_format_list->pViewFormats))
       return VK_ERROR_FORMAT_NOT_SUPPORTED;

    if (swapchain_compression &&
        (swapchain_compression->flags &
         (VK_IMAGE_COMPRESSION_FIXED_RATE_DEFAULT_EXT |
          VK_IMAGE_COMPRESSION_FIXED_RATE_EXPLICIT_EXT)))
       return VK_ERROR_FORMAT_NOT_SUPPORTED;

    if (mutable_format &&
        (!swapchain_format_list || !swapchain_format_list->viewFormatCount))
       return VK_ERROR_FORMAT_NOT_SUPPORTED;

    if (mutable_format) {
       bool base_format_in_list = false;
       for (uint32_t i = 0; i < swapchain_format_list->viewFormatCount; i++)
          base_format_in_list |=
             swapchain_format_list->pViewFormats[i] == pCreateInfo->imageFormat;
       if (!base_format_in_list)
          return VK_ERROR_FORMAT_NOT_SUPPORTED;
    }

    if (pCreateInfo->imageFormat != VK_FORMAT_R8G8B8A8_UNORM)
       return VK_ERROR_FORMAT_NOT_SUPPORTED;

    VkIcdSurfaceAndroid *android_surface = (VkIcdSurfaceAndroid *)surface;
    int min_undequeued = 0;
    if (!android_surface->window ||
        android_surface->window->query(
           android_surface->window, NATIVE_WINDOW_MIN_UNDEQUEUED_BUFFERS,
           &min_undequeued) != 0 ||
        min_undequeued < 0 ||
        min_undequeued >= PANVK_ANDROID_MAX_SWAPCHAIN_BUFFERS ||
        pCreateInfo->minImageCount < 2 ||
        pCreateInfo->minImageCount >
           PANVK_ANDROID_MAX_SWAPCHAIN_BUFFERS - min_undequeued)
       return panvk_errorf(device, VK_ERROR_INITIALIZATION_FAILED,
                           "Unsupported Android swapchain buffer count");

    VkResult result;
    uint32_t num_images = pCreateInfo->minImageCount + min_undequeued;
    uint32_t buffer_count = num_images;

    struct panvk_android_swapchain *chain = vk_zalloc2(
       &dev->alloc, pAllocator, sizeof(*chain), 8,
       VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
    if (!chain)
       return VK_ERROR_OUT_OF_HOST_MEMORY;

    vk_object_base_init(dev, &chain->base.base, VK_OBJECT_TYPE_SWAPCHAIN_KHR);
    chain->base.wsi = wsi_device;
    chain->base.device = device;
    chain->base.alloc = *pAllocator;
    chain->base.blit.type = WSI_SWAPCHAIN_NO_BLIT;
    chain->base.destroy = panvk_android_swapchain_destroy;
    chain->base.get_wsi_image = panvk_android_swapchain_get_wsi_image;
   chain->base.acquire_next_image = panvk_android_swapchain_acquire_next_image;
   chain->base.queue_present = panvk_android_swapchain_queue_present;
   chain->base.wait_for_present2 = panvk_android_swapchain_wait_for_present2;
   chain->base.release_images = panvk_android_swapchain_release_images;

   chain->surface = (struct panvk_android_surface *)android_surface;
   chain->window = android_surface->window;
   if (!chain->window) {
      panvk_android_swapchain_destroy(&chain->base, pAllocator);
      return VK_ERROR_SURFACE_LOST_KHR;
   }
   ANativeWindow_acquire(chain->window);
   if (pCreateInfo->oldSwapchain != VK_NULL_HANDLE) {
      VK_FROM_HANDLE(wsi_swapchain, old_base, pCreateInfo->oldSwapchain);
      struct panvk_android_swapchain *old_chain =
         container_of(old_base, struct panvk_android_swapchain, base);
      bool old_images_acquired = false;
      for (uint32_t i = 0; i < old_chain->base.image_count; i++)
         old_images_acquired |= old_chain->images[i].base.acquired;

      struct panvk_android_surface *panvk_surface =
         (struct panvk_android_surface *)android_surface;
      if (!old_images_acquired && panvk_surface->window_connected) {
         VkResult disconnect_result =
            panvk_android_surface_disconnect(panvk_surface);
         if (disconnect_result != VK_SUCCESS) {
            panvk_android_swapchain_destroy(&chain->base, pAllocator);
            return panvk_errorf(device, disconnect_result,
                                "ANativeWindow API disconnect failed");
         }
      }
   }
   VkResult connect_result = panvk_android_surface_connect(android_surface);
   if (connect_result != VK_SUCCESS) {
      panvk_android_swapchain_destroy(&chain->base, pAllocator);
      return panvk_errorf(device, connect_result,
                          "ANativeWindow API connect failed");
   }
   chain->base.image_count = num_images;
   {
      int usage_result = chain->window->perform(
         chain->window, NATIVE_WINDOW_SET_USAGE,
         AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT |
            AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE);
      if (usage_result != 0) {
         mesa_loge("panvk: Android swapchain set usage failed (%d)",
                   usage_result);
         panvk_android_swapchain_destroy(&chain->base, pAllocator);
         return panvk_errorf(device, VK_ERROR_SURFACE_LOST_KHR,
                             "ANativeWindow_setUsage failed: %d",
                             usage_result);
      }

      int geometry_result = ANativeWindow_setBuffersGeometry(
         chain->window, pCreateInfo->imageExtent.width,
         pCreateInfo->imageExtent.height, WINDOW_FORMAT_RGBA_8888);
      if (geometry_result != 0) {
         mesa_loge("panvk: Android swapchain set geometry to %ux%u failed (%d)",
                   pCreateInfo->imageExtent.width,
                   pCreateInfo->imageExtent.height, geometry_result);
         panvk_android_swapchain_destroy(&chain->base, pAllocator);
         return panvk_errorf(device, VK_ERROR_SURFACE_LOST_KHR,
                             "ANativeWindow_setBuffersGeometry failed: %d",
                             geometry_result);
      }

      int count_result =
         native_window_set_buffer_count(chain->window, buffer_count);
      if (count_result != 0) {
         mesa_loge("panvk: Android swapchain set buffer count to %u failed (%d)",
                   buffer_count, count_result);
         panvk_android_swapchain_destroy(&chain->base, pAllocator);
         return panvk_errorf(device, VK_ERROR_SURFACE_LOST_KHR,
                             "ANativeWindow_setBufferCount failed: %d",
                             count_result);
      }
   }

   for (uint32_t i = 0; i < num_images; i++) {
      struct panvk_android_swapchain_image *img = &chain->images[i];
      img->acquire_fence_fd = -1;
      int dequeue_result = chain->window->dequeueBuffer(
         chain->window, &img->buffer, &img->acquire_fence_fd);
      if (dequeue_result != 0 || !img->buffer) {
         mesa_loge("panvk: Android swapchain dequeue buffer %u/%u failed (%d)",
                   i, num_images, dequeue_result);
         panvk_android_swapchain_destroy(&chain->base, pAllocator);
         return panvk_errorf(device, VK_ERROR_SURFACE_LOST_KHR,
                             "ANativeWindow_dequeueBuffer failed: %d",
                             dequeue_result);
      }
      img->dequeued = true;
      img->initial_dequeue = true;
      img->buffer->common.incRef(&img->buffer->common);

      img->ahb = ANativeWindowBuffer_getHardwareBuffer(img->buffer);
      if (!img->ahb) {
         panvk_android_swapchain_destroy(&chain->base, pAllocator);
         return panvk_errorf(device, VK_ERROR_INVALID_EXTERNAL_HANDLE,
                             "Android window buffer has no AHardwareBuffer");
      }
      AHardwareBuffer_acquire(img->ahb);

      AHardwareBuffer_Desc desc;
      AHardwareBuffer_describe(img->ahb, &desc);
      if (desc.width != pCreateInfo->imageExtent.width ||
          desc.height != pCreateInfo->imageExtent.height ||
          desc.format != AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM ||
          desc.layers != 1) {
         mesa_loge("panvk: unsupported Android window buffer %ux%u format %u layers %u; expected %ux%u RGBA8",
                   desc.width, desc.height, desc.format, desc.layers,
                   pCreateInfo->imageExtent.width,
                   pCreateInfo->imageExtent.height);
         panvk_android_swapchain_destroy(&chain->base, pAllocator);
         return panvk_errorf(
            device, VK_ERROR_FORMAT_NOT_SUPPORTED,
            "Unsupported Android window buffer: %ux%u format %u layers %u",
            desc.width, desc.height, desc.format, desc.layers);
      }

      VkExternalMemoryImageCreateInfo emici = {
         .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
         .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID,
      };
      VkImageCompressionControlEXT image_compression = {
         .sType = VK_STRUCTURE_TYPE_IMAGE_COMPRESSION_CONTROL_EXT,
         .flags = swapchain_compression ? swapchain_compression->flags
                                        : VK_IMAGE_COMPRESSION_DEFAULT_EXT,
         .compressionControlPlaneCount =
            swapchain_compression
               ? swapchain_compression->compressionControlPlaneCount
               : 0,
         .pFixedRateFlags =
            swapchain_compression ? swapchain_compression->pFixedRateFlags
                                  : NULL,
      };
      VkImageFormatListCreateInfo image_format_list = {
         .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO,
         .viewFormatCount = swapchain_format_list
                               ? swapchain_format_list->viewFormatCount
                               : 0,
         .pViewFormats = swapchain_format_list
                            ? swapchain_format_list->pViewFormats
                            : NULL,
      };
      if (swapchain_compression) {
         emici.pNext = &image_compression;
         if (mutable_format)
            image_compression.pNext = &image_format_list;
      } else if (mutable_format) {
         emici.pNext = &image_format_list;
      }
      VkImageCreateInfo ici = {
         .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
         .pNext = &emici,
         .flags = mutable_format
                     ? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT |
                          VK_IMAGE_CREATE_EXTENDED_USAGE_BIT
                     : 0,
         .imageType = VK_IMAGE_TYPE_2D,
         .format = pCreateInfo->imageFormat,
         .extent = {pCreateInfo->imageExtent.width, pCreateInfo->imageExtent.height, 1},
         .mipLevels = 1,
         .arrayLayers = 1,
         .samples = VK_SAMPLE_COUNT_1_BIT,
         .tiling = VK_IMAGE_TILING_OPTIMAL,
         .usage = pCreateInfo->imageUsage,
         .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
         .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
      };
      result = dev->dispatch_table.CreateImage(device, &ici, pAllocator, &img->base.image);
      if (result != VK_SUCCESS) {
         panvk_android_swapchain_destroy(&chain->base, pAllocator);
         return result;
      }

      VkAndroidHardwareBufferPropertiesANDROID ahb_props = {
         .sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID,
      };
      result = panvk_GetAndroidHardwareBufferPropertiesANDROID(device, img->ahb, &ahb_props);
      if (result != VK_SUCCESS) {
         panvk_android_swapchain_destroy(&chain->base, pAllocator);
         return result;
      }

      uint32_t chosen_mi = 0;
      bool found_memory_type = false;
      for (uint32_t m = 0; m < 32; m++) {
         if (ahb_props.memoryTypeBits & (1u << m)) {
            chosen_mi = m;
            found_memory_type = true;
            break;
         }
      }
      if (!found_memory_type) {
         panvk_android_swapchain_destroy(&chain->base, pAllocator);
         return panvk_errorf(
            device, VK_ERROR_INVALID_EXTERNAL_HANDLE,
            "AHardwareBuffer exposes no compatible Vulkan memory type");
      }

      VkImportAndroidHardwareBufferInfoANDROID imp = {
         .sType = VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID,
         .buffer = img->ahb,
      };
      VkMemoryDedicatedAllocateInfo dai = {
         .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
         .pNext = &imp,
         .image = img->base.image,
      };
      VkMemoryAllocateInfo mai = {
         .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
         .pNext = &dai,
         .allocationSize = ahb_props.allocationSize,
         .memoryTypeIndex = chosen_mi,
      };
      result = dev->dispatch_table.AllocateMemory(device, &mai, pAllocator, &img->base.memory);
      if (result != VK_SUCCESS) {
         panvk_android_swapchain_destroy(&chain->base, pAllocator);
         return result;
      }

      VkBindImageMemoryInfo bmi = {
         .sType = VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_INFO,
         .image = img->base.image,
         .memory = img->base.memory,
         .memoryOffset = 0,
      };
      result = dev->dispatch_table.BindImageMemory2(device, 1, &bmi);
      if (result != VK_SUCCESS) {
         panvk_android_swapchain_destroy(&chain->base, pAllocator);
         return result;
      }
   }

   chain->surface->swapchain_count++;
   chain->surface_registered = true;
   *swapchain_out = &chain->base;
   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
panvk_CreateAndroidSurfaceKHR(
   VkInstance _instance,
   const VkAndroidSurfaceCreateInfoKHR *pCreateInfo,
   const VkAllocationCallbacks *pAllocator,
   VkSurfaceKHR *pSurface)
{
   VK_FROM_HANDLE(vk_instance, instance, _instance);
   if (!pCreateInfo || !pCreateInfo->window)
      return VK_ERROR_INITIALIZATION_FAILED;

   struct panvk_android_surface *surface = vk_zalloc2(
      &instance->alloc, pAllocator, sizeof(*surface), 8,
      VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!surface)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   surface->base.base.platform = VK_ICD_WSI_PLATFORM_ANDROID;
   surface->base.window = pCreateInfo->window;
   ANativeWindow_acquire(surface->base.window);

   *pSurface = VkIcdSurfaceBase_to_handle(&surface->base.base);
   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
panvk_DestroySurfaceKHR(VkInstance _instance,
                        VkSurfaceKHR _surface,
                        const VkAllocationCallbacks *pAllocator)
{
   VK_FROM_HANDLE(vk_instance, instance, _instance);
   ICD_FROM_HANDLE(VkIcdSurfaceBase, surface, _surface);

   if (!surface)
      return;

   if (surface->platform == VK_ICD_WSI_PLATFORM_ANDROID) {
      struct panvk_android_surface *android_surface =
         (struct panvk_android_surface *)surface;
      panvk_android_surface_disconnect(android_surface);
      if (android_surface->base.window)
         ANativeWindow_release(android_surface->base.window);
      vk_free2(&instance->alloc, pAllocator, surface);
      return;
   }

   wsi_DestroySurfaceKHR(_instance, _surface, pAllocator);
}

static struct wsi_interface panvk_android_wsi_interface = {
   .get_support = panvk_wsi_android_get_support,
   .get_capabilities2 = panvk_wsi_android_get_capabilities2,
   .get_formats = panvk_wsi_android_get_formats,
   .get_formats2 = panvk_wsi_android_get_formats2,
   .get_present_modes = panvk_wsi_android_get_present_modes,
   .get_present_rectangles = panvk_wsi_android_get_present_rectangles,
   .create_swapchain = panvk_wsi_android_create_swapchain,
};
#endif /* VK_USE_PLATFORM_ANDROID_KHR */

VkResult
panvk_wsi_init(struct panvk_physical_device *physical_device)
{
   struct panvk_instance *instance =
      to_panvk_instance(physical_device->vk.instance);
   const bool uses_kbase = physical_device->kbase_node_path[0] != '\0';
   const char *dri3_option = getenv("PANVK_KBASE_DRI3");
   /* The proprietary Termux:X11 layer uses WSI_X11_TERMUX=1 as its
    * process-wide switch.  Accept the same switch here when no explicit
    * PanVK override is present, so both ICDs exercise the same raw-FD DRI3
    * transport instead of silently falling back to the slower SHM path. */
   const char *termux_wsi = getenv("WSI_X11_TERMUX");
   const bool termux_raw_dri3 =
      uses_kbase && !dri3_option && termux_wsi && strcmp(termux_wsi, "0") != 0;
   const bool kbase_raw_dri3 =
      termux_raw_dri3 ||
      (uses_kbase && dri3_option &&
       (!strcmp(dri3_option, "raw") || !strcmp(dri3_option, "termux")));
   const bool kbase_dmabuf =
#if defined(HAVE_PAN_KMOD_KBASE)
      uses_kbase &&
      ((!dri3_option && termux_raw_dri3) ||
       (dri3_option && strcmp(dri3_option, "0") != 0)) &&
      kbase_kmod_supports_dmabuf(physical_device->kmod.dev);
#else
      false;
#endif
   VkResult result;

   result = wsi_device_init(&physical_device->wsi_device,
                            panvk_physical_device_to_handle(physical_device),
                            panvk_wsi_proc_addr, &instance->vk.alloc, -1,
                            &instance->drirc.options,
                            &(struct wsi_device_options){
                               .sw_device = uses_kbase && !kbase_dmabuf,
                            });
   if (result != VK_SUCCESS)
      return result;

   /* kbase syncs carry GPU seqno state in userspace and cannot be copied via
    * DRM syncobj fd payloads.  Keep even empty WSI submits on the real queue
    * so present fences are backed by the kbase submission timeline.
    */
   physical_device->wsi_device.disable_unordered_submits = uses_kbase;
   const char *ema_pacing = getenv("PANVK_KBASE_EMA_PACING");
   physical_device->wsi_device.enable_ema_frame_pacing =
      uses_kbase && (!ema_pacing || strcmp(ema_pacing, "0") != 0);

   /* Kbase presentation has not been validated with queueing present modes.
    * Keep display ownership serialized until its present synchronization is
    * proven to avoid reusing a buffer while it is still being scanned out.
    */
   if (uses_kbase)
      physical_device->wsi_device.override_present_mode =
         VK_PRESENT_MODE_FIFO_KHR;

   /* kbase is not a DRM fd.  The default CPU WSI path presents through
    * MIT-SHM.  Android dma-heaps can also provide shareable
    * allocations, but some Android X servers advertise DRI3 while rejecting
    * standard PixmapFromBuffer.  PANVK_KBASE_DRI3=raw (or termux) uses the
    * Termux:X11/Winlator private raw-FD modifier instead of DRM modifiers.
    */
   physical_device->wsi_device.supports_modifiers =
      !uses_kbase || (kbase_dmabuf && !kbase_raw_dri3);
   physical_device->wsi_device.can_present_on_device =
      panvk_can_present_on_device;

   physical_device->vk.wsi_device = &physical_device->wsi_device;

#ifdef VK_USE_PLATFORM_ANDROID_KHR
   physical_device->wsi_device.wsi[VK_ICD_WSI_PLATFORM_ANDROID] = &panvk_android_wsi_interface;
#endif

   return VK_SUCCESS;
}

void
panvk_wsi_finish(struct panvk_physical_device *physical_device)
{
   struct panvk_instance *instance =
      to_panvk_instance(physical_device->vk.instance);

   physical_device->vk.wsi_device = NULL;
   wsi_device_finish(&physical_device->wsi_device, &instance->vk.alloc);
}
