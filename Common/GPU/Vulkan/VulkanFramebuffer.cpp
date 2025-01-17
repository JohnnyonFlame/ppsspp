#include "Common/StringUtils.h"
#include "Common/GPU/Vulkan/VulkanFramebuffer.h"
#include "Common/GPU/Vulkan/VulkanQueueRunner.h"

VKRFramebuffer::VKRFramebuffer(VulkanContext *vk, VkCommandBuffer initCmd, VKRRenderPass *compatibleRenderPass, int _width, int _height, int _numLayers, bool createDepthStencilBuffer, const char *tag)
	: vulkan_(vk), tag_(tag), width(_width), height(_height), numLayers(_numLayers) {

	_dbg_assert_(tag);

	CreateImage(vulkan_, initCmd, color, width, height, numLayers, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, true, tag);
	if (createDepthStencilBuffer) {
		CreateImage(vulkan_, initCmd, depth, width, height, numLayers, vulkan_->GetDeviceInfo().preferredDepthStencilFormat, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, false, tag);
	}

	UpdateTag(tag);

	// We create the actual framebuffer objects on demand, because some combinations might not make sense.
	// Framebuffer objects are just pointers to a set of images, so no biggie.
}

void VKRFramebuffer::UpdateTag(const char *newTag) {
	char name[128];
	snprintf(name, sizeof(name), "fb_color_%s", tag_.c_str());
	vulkan_->SetDebugName(color.image, VK_OBJECT_TYPE_IMAGE, name);
	vulkan_->SetDebugName(color.rtView, VK_OBJECT_TYPE_IMAGE_VIEW, name);
	if (depth.image) {
		snprintf(name, sizeof(name), "fb_depth_%s", tag_.c_str());
		vulkan_->SetDebugName(depth.image, VK_OBJECT_TYPE_IMAGE, name);
		vulkan_->SetDebugName(depth.rtView, VK_OBJECT_TYPE_IMAGE_VIEW, name);
	}
	for (size_t rpType = 0; rpType < (size_t)RenderPassType::TYPE_COUNT; rpType++) {
		if (framebuf[rpType]) {
			snprintf(name, sizeof(name), "fb_%s", tag_.c_str());
			vulkan_->SetDebugName(framebuf[(int)rpType], VK_OBJECT_TYPE_FRAMEBUFFER, name);
		}
	}
}

VkFramebuffer VKRFramebuffer::Get(VKRRenderPass *compatibleRenderPass, RenderPassType rpType) {
	bool multiview = RenderPassTypeHasMultiView(rpType);

	if (framebuf[(int)rpType]) {
		return framebuf[(int)rpType];
	}

	VkFramebufferCreateInfo fbci{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
	VkImageView views[2]{};

	bool hasDepth = RenderPassTypeHasDepth(rpType);
	views[0] = color.rtView;  // 2D array texture if multilayered.
	if (hasDepth) {
		if (!depth.rtView) {
			WARN_LOG(G3D, "depth render type to non-depth fb: %p %p fmt=%d (%s %dx%d)", depth.image, depth.texAllLayersView, depth.format, tag_.c_str(), width, height);
			// Will probably crash, depending on driver.
		}
		views[1] = depth.rtView;
	}
	fbci.renderPass = compatibleRenderPass->Get(vulkan_, rpType);
	fbci.attachmentCount = hasDepth ? 2 : 1;
	fbci.pAttachments = views;
	fbci.width = width;
	fbci.height = height;
	fbci.layers = 1;  // With multiview, this should be set as 1.

	VkResult res = vkCreateFramebuffer(vulkan_->GetDevice(), &fbci, nullptr, &framebuf[(int)rpType]);
	_assert_(res == VK_SUCCESS);

	if (!tag_.empty() && vulkan_->Extensions().EXT_debug_utils) {
		vulkan_->SetDebugName(framebuf[(int)rpType], VK_OBJECT_TYPE_FRAMEBUFFER, StringFromFormat("fb_%s", tag_.c_str()).c_str());
	}

	return framebuf[(int)rpType];
}

VKRFramebuffer::~VKRFramebuffer() {
	// Get rid of the views first, feels cleaner (but in reality doesn't matter).
	if (color.rtView)
		vulkan_->Delete().QueueDeleteImageView(color.rtView);
	if (depth.rtView)
		vulkan_->Delete().QueueDeleteImageView(depth.rtView);
	if (color.texAllLayersView)
		vulkan_->Delete().QueueDeleteImageView(color.texAllLayersView);
	if (depth.texAllLayersView)
		vulkan_->Delete().QueueDeleteImageView(depth.texAllLayersView);
	for (int i = 0; i < 2; i++) {
		if (color.texLayerViews[i]) {
			vulkan_->Delete().QueueDeleteImageView(color.texLayerViews[i]);
		}
		if (depth.texLayerViews[i]) {
			vulkan_->Delete().QueueDeleteImageView(depth.texLayerViews[i]);
		}
	}

	if (color.image) {
		_dbg_assert_(color.alloc);
		vulkan_->Delete().QueueDeleteImageAllocation(color.image, color.alloc);
	}
	if (depth.image) {
		_dbg_assert_(depth.alloc);
		vulkan_->Delete().QueueDeleteImageAllocation(depth.image, depth.alloc);
	}
	for (auto &fb : framebuf) {
		if (fb) {
			vulkan_->Delete().QueueDeleteFramebuffer(fb);
		}
	}
}

// NOTE: If numLayers > 1, it will create an array texture, rather than a normal 2D texture.
// This requires a different sampling path!
void VKRFramebuffer::CreateImage(VulkanContext *vulkan, VkCommandBuffer cmd, VKRImage &img, int width, int height, int numLayers, VkFormat format, VkImageLayout initialLayout, bool color, const char *tag) {
	// We don't support more exotic layer setups for now. Mono or stereo.
	_dbg_assert_(numLayers == 1 || numLayers == 2);

	VkImageCreateInfo ici{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
	ici.arrayLayers = numLayers;
	ici.mipLevels = 1;
	ici.extent.width = width;
	ici.extent.height = height;
	ici.extent.depth = 1;
	ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	ici.imageType = VK_IMAGE_TYPE_2D;
	ici.samples = VK_SAMPLE_COUNT_1_BIT;
	ici.tiling = VK_IMAGE_TILING_OPTIMAL;
	ici.format = format;
	// Strictly speaking we don't yet need VK_IMAGE_USAGE_SAMPLED_BIT for depth buffers since we do not yet sample depth buffers.
	ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	if (color) {
		ici.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
	} else {
		ici.usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
	}

	VmaAllocationCreateInfo allocCreateInfo{};
	allocCreateInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;
	VmaAllocationInfo allocInfo{};

	VkResult res = vmaCreateImage(vulkan->Allocator(), &ici, &allocCreateInfo, &img.image, &img.alloc, &allocInfo);
	_dbg_assert_(res == VK_SUCCESS);

	VkImageAspectFlags aspects = color ? VK_IMAGE_ASPECT_COLOR_BIT : (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);

	VkImageViewCreateInfo ivci{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
	ivci.components = { VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY };
	ivci.format = ici.format;
	ivci.image = img.image;
	ivci.viewType = numLayers == 1 ? VK_IMAGE_VIEW_TYPE_2D : VK_IMAGE_VIEW_TYPE_2D_ARRAY;
	ivci.subresourceRange.aspectMask = aspects;
	ivci.subresourceRange.layerCount = numLayers;
	ivci.subresourceRange.levelCount = 1;
	res = vkCreateImageView(vulkan->GetDevice(), &ivci, nullptr, &img.rtView);
	vulkan->SetDebugName(img.rtView, VK_OBJECT_TYPE_IMAGE_VIEW, tag);
	_dbg_assert_(res == VK_SUCCESS);

	// Separate view for texture sampling all layers together.
	if (!color) {
		ivci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
	}

	ivci.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;  // layered for consistency, even if single image.
	res = vkCreateImageView(vulkan->GetDevice(), &ivci, nullptr, &img.texAllLayersView);
	vulkan->SetDebugName(img.texAllLayersView, VK_OBJECT_TYPE_IMAGE_VIEW, tag);

	// Create 2D views for both layers.
	// Useful when multipassing shaders that don't yet exist in a single-pass-stereo version.
	for (int i = 0; i < numLayers; i++) {
		ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
		ivci.subresourceRange.layerCount = 1;
		ivci.subresourceRange.baseArrayLayer = i;
		res = vkCreateImageView(vulkan->GetDevice(), &ivci, nullptr, &img.texLayerViews[i]);
		if (vulkan->DebugLayerEnabled()) {
			char temp[128];
			snprintf(temp, sizeof(temp), "%s_layer%d", tag, i);
			vulkan->SetDebugName(img.texLayerViews[i], VK_OBJECT_TYPE_IMAGE_VIEW, temp);
		}
		_dbg_assert_(res == VK_SUCCESS);
	}

	VkPipelineStageFlags dstStage;
	VkAccessFlagBits dstAccessMask;
	switch (initialLayout) {
	case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
		dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		dstStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
		break;
	case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
		dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		dstStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
		break;
	case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
		dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
		dstStage = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
		break;
	default:
		Crash();
		return;
	}

	TransitionImageLayout2(cmd, img.image, 0, 1, numLayers, aspects,
		VK_IMAGE_LAYOUT_UNDEFINED, initialLayout,
		VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, dstStage,
		0, dstAccessMask);
	img.layout = initialLayout;
	img.format = format;
	img.tag = tag ? tag : "N/A";
	img.numLayers = numLayers;
}

static VkAttachmentLoadOp ConvertLoadAction(VKRRenderPassLoadAction action) {
	switch (action) {
	case VKRRenderPassLoadAction::CLEAR:     return VK_ATTACHMENT_LOAD_OP_CLEAR;
	case VKRRenderPassLoadAction::KEEP:      return VK_ATTACHMENT_LOAD_OP_LOAD;
	case VKRRenderPassLoadAction::DONT_CARE: return VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	}
	return VK_ATTACHMENT_LOAD_OP_DONT_CARE;  // avoid compiler warning
}

static VkAttachmentStoreOp ConvertStoreAction(VKRRenderPassStoreAction action) {
	switch (action) {
	case VKRRenderPassStoreAction::STORE:     return VK_ATTACHMENT_STORE_OP_STORE;
	case VKRRenderPassStoreAction::DONT_CARE: return VK_ATTACHMENT_STORE_OP_DONT_CARE;
	}
	return VK_ATTACHMENT_STORE_OP_DONT_CARE;  // avoid compiler warning
}

// Self-dependency: https://github.com/gpuweb/gpuweb/issues/442#issuecomment-547604827
// Also see https://www.khronos.org/registry/vulkan/specs/1.3-extensions/html/vkspec.html#synchronization-pipeline-barriers-subpass-self-dependencies

VkRenderPass CreateRenderPass(VulkanContext *vulkan, const RPKey &key, RenderPassType rpType) {
	bool selfDependency = RenderPassTypeHasInput(rpType);
	bool isBackbuffer = rpType == RenderPassType::BACKBUFFER;
	bool hasDepth = RenderPassTypeHasDepth(rpType);
	bool multiview = RenderPassTypeHasMultiView(rpType);

	if (multiview) {
		// TODO: Assert that the device has multiview support enabled.
	}

	VkAttachmentDescription attachments[2] = {};
	attachments[0].format = isBackbuffer ? vulkan->GetSwapchainFormat() : VK_FORMAT_R8G8B8A8_UNORM;
	attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
	attachments[0].loadOp = ConvertLoadAction(key.colorLoadAction);
	attachments[0].storeOp = ConvertStoreAction(key.colorStoreAction);
	attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	attachments[0].initialLayout = isBackbuffer ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	attachments[0].finalLayout = isBackbuffer ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	attachments[0].flags = 0;

	if (hasDepth) {
		attachments[1].format = vulkan->GetDeviceInfo().preferredDepthStencilFormat;
		attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
		attachments[1].loadOp = ConvertLoadAction(key.depthLoadAction);
		attachments[1].storeOp = ConvertStoreAction(key.depthStoreAction);
		attachments[1].stencilLoadOp = ConvertLoadAction(key.stencilLoadAction);
		attachments[1].stencilStoreOp = ConvertStoreAction(key.stencilStoreAction);
		attachments[1].initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
		attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
		attachments[1].flags = 0;
	}

	VkAttachmentReference color_reference{};
	color_reference.attachment = 0;
	color_reference.layout = selfDependency ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

	VkAttachmentReference depth_reference{};
	depth_reference.attachment = 1;
	depth_reference.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

	VkSubpassDescription subpass{};
	subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	subpass.flags = 0;
	if (selfDependency) {
		subpass.inputAttachmentCount = 1;
		subpass.pInputAttachments = &color_reference;
	} else {
		subpass.inputAttachmentCount = 0;
		subpass.pInputAttachments = nullptr;
	}
	subpass.colorAttachmentCount = 1;
	subpass.pColorAttachments = &color_reference;
	subpass.pResolveAttachments = nullptr;
	if (hasDepth) {
		subpass.pDepthStencilAttachment = &depth_reference;
	}
	subpass.preserveAttachmentCount = 0;
	subpass.pPreserveAttachments = nullptr;

	// Not sure if this is really necessary.
	VkSubpassDependency deps[2]{};
	size_t numDeps = 0;

	VkRenderPassCreateInfo rp{ VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
	rp.attachmentCount = hasDepth ? 2 : 1;
	rp.pAttachments = attachments;
	rp.subpassCount = 1;
	rp.pSubpasses = &subpass;

	VkRenderPassMultiviewCreateInfoKHR mv{ VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO_KHR };
	uint32_t viewMask = 0x3;  // Must be outside the 'if (multiview)' scope!
	int viewOffset = 0;
	if (multiview) {
		rp.pNext = &mv;
		mv.subpassCount = 1;
		mv.pViewMasks = &viewMask;
		mv.dependencyCount = 0;
		mv.pCorrelationMasks = &viewMask; // same masks
		mv.correlationMaskCount = 1;
		mv.pViewOffsets = &viewOffset;
	}

	if (isBackbuffer) {
		deps[numDeps].srcSubpass = VK_SUBPASS_EXTERNAL;
		deps[numDeps].dstSubpass = 0;
		deps[numDeps].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
		deps[numDeps].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
		deps[numDeps].srcAccessMask = 0;
		deps[numDeps].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		numDeps++;
	}

	if (selfDependency) {
		deps[numDeps].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;
		deps[numDeps].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		deps[numDeps].dstAccessMask = VK_ACCESS_INPUT_ATTACHMENT_READ_BIT;
		deps[numDeps].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
		deps[numDeps].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
		deps[numDeps].srcSubpass = 0;
		deps[numDeps].dstSubpass = 0;
		numDeps++;
	}

	if (numDeps > 0) {
		rp.dependencyCount = (u32)numDeps;
		rp.pDependencies = deps;
	}

	VkRenderPass pass;
	VkResult res = vkCreateRenderPass(vulkan->GetDevice(), &rp, nullptr, &pass);
	_assert_(res == VK_SUCCESS);
	_assert_(pass != VK_NULL_HANDLE);
	return pass;
}

VkRenderPass VKRRenderPass::Get(VulkanContext *vulkan, RenderPassType rpType) {
	// When we create a render pass, we create all "types" of it immediately,
	// practical later when referring to it. Could change to on-demand if it feels motivated
	// but I think the render pass objects are cheap.
	if (!pass[(int)rpType]) {
		pass[(int)rpType] = CreateRenderPass(vulkan, key_, (RenderPassType)rpType);
	}
	return pass[(int)rpType];
}
