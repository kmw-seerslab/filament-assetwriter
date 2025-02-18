/*
 * Copyright (C) 2021 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

 #ifndef TNT_FILAMENT_DRIVER_VULKANTEXTURE_H
 #define TNT_FILAMENT_DRIVER_VULKANTEXTURE_H

#include "VulkanDriver.h"
#include "VulkanBuffer.h"
#include "VulkanUtility.h"

#include <utils/RangeMap.h>

namespace filament {
namespace backend {

struct VulkanTexture : public HwTexture {
    VulkanTexture(VulkanContext& context, SamplerType target, uint8_t levels,
            TextureFormat format, uint8_t samples, uint32_t w, uint32_t h, uint32_t depth,
            TextureUsage usage, VulkanStagePool& stagePool, VkComponentMapping swizzle = {});
    ~VulkanTexture();

    // Uploads data into a subregion of a 2D or 3D texture.
    void updateImage(const PixelBufferDescriptor& data, uint32_t width, uint32_t height,
            uint32_t depth, uint32_t xoffset, uint32_t yoffset, uint32_t zoffset, uint32_t miplevel);

    // Uploads data into all 6 faces of a cubemap for a given miplevel.
    void updateCubeImage(const PixelBufferDescriptor& data, const FaceOffsets& faceOffsets,
            uint32_t miplevel);

    // Returns the primary image view, which is used for shader sampling.
    VkImageView getPrimaryImageView() const { return mCachedImageViews.at(mPrimaryViewRange); }

    // Sets the min/max range of miplevels in the primary image view.
    void setPrimaryRange(uint32_t minMiplevel, uint32_t maxMiplevel);

    // Gets or creates a cached VkImageView for a single subresource that can be used as a render
    // target attachment.  Unlike the primary image view, this always has type VK_IMAGE_VIEW_TYPE_2D
    // and the identity swizzle.
    VkImageView getAttachmentView(int singleLevel, int singleLayer, VkImageAspectFlags aspect);

    VkFormat getVkFormat() const { return mVkFormat; }
    VkImage getVkImage() const { return mTextureImage; }
    VkImageLayout getVkLayout(uint32_t layer, uint32_t level) const;

    void setSidecar(VulkanTexture* sidecar) { mSidecarMSAA = sidecar; }
    VulkanTexture* getSidecar() const { return mSidecarMSAA; }

    void transitionLayout(VkCommandBuffer commands, const VkImageSubresourceRange& range,
            VkImageLayout newLayout);

private:
    // Gets or creates a cached VkImageView for a range of miplevels and array layers.
    VkImageView getImageView(VkImageSubresourceRange range);

    void updateImageWithBlit(const PixelBufferDescriptor& hostData, uint32_t width,
        uint32_t height, uint32_t depth, uint32_t miplevel);

    VulkanTexture* mSidecarMSAA = nullptr;
    const VkFormat mVkFormat;
    const VkImageAspectFlags mAspect;
    const VkImageViewType mViewType;
    const VkComponentMapping mSwizzle;
    VkImage mTextureImage = VK_NULL_HANDLE;
    VkDeviceMemory mTextureImageMemory = VK_NULL_HANDLE;

    // Track the image layout of each subresource using a sparse range map.
    utils::RangeMap<uint32_t, VkImageLayout> mSubresourceLayouts;

    // Track the range of subresources that define the "primary" image view, which is the special
    // image view that gets bound to an actual texture sampler.
    VkImageSubresourceRange mPrimaryViewRange;

    std::map<VkImageSubresourceRange, VkImageView> mCachedImageViews;
    VulkanContext& mContext;
    VulkanStagePool& mStagePool;
};

} // namespace filament
} // namespace backend

#endif // TNT_FILAMENT_DRIVER_VULKANTEXTURE_H
