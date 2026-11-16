/*
 * libsmacker - Vulkan playback example.
 *
 * The decoded frame stays palettized: an R8 index image and a 256x1 RGBA
 * palette image go to the GPU, and a fragment shader resolves the palette.
 * SDL provides the window, surface, and audio. On macOS the MoltenVK ICD
 * supplies the Vulkan implementation.
 */

#include "smk_sdl_common.h"
#include "smk_player_common.h"

#include <vulkan/vulkan.h>

#ifdef SMK_USE_SDL3
#include <SDL3/SDL_vulkan.h>
#else
#include <SDL_vulkan.h>
#endif

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern const uint32_t smk_vk_vert_spv[];
extern const unsigned long smk_vk_vert_spv_len;
extern const uint32_t smk_vk_frag_spv[];
extern const unsigned long smk_vk_frag_spv_len;

#ifndef VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME
#define VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME "VK_KHR_portability_subset"
#endif

struct vk_state {
	VkInstance	instance;
	VkSurfaceKHR	surface;
	VkPhysicalDevice physical;
	uint32_t	queue_family;
	VkDevice	device;
	VkQueue		queue;

	VkSwapchainKHR	swapchain;
	VkFormat	format;
	VkExtent2D	extent;
	uint32_t	image_count;
	VkImage		*images;
	VkImageView	*views;
	VkFramebuffer	*framebuffers;
	VkRenderPass	render_pass;

	VkDescriptorSetLayout	set_layout;
	VkPipelineLayout	pipeline_layout;
	VkPipeline		pipeline;
	VkDescriptorPool	descriptor_pool;
	VkDescriptorSet		descriptor_set;

	VkCommandPool		command_pool;
	VkCommandBuffer		command;
	VkFence			fence;
	VkSemaphore		image_available;
	VkSemaphore		render_finished;

	VkImage			frame_image;
	VkDeviceMemory		frame_memory;
	VkImageView		frame_view;
	VkImageLayout		frame_layout;
	VkImage			palette_image;
	VkDeviceMemory		palette_memory;
	VkImageView		palette_view;
	VkImageLayout		palette_layout;
	VkSampler		sampler;

	VkBuffer		staging;
	VkDeviceMemory		staging_memory;
	void			*staging_ptr;
	VkDeviceSize		staging_size;

	unsigned long		w;
	unsigned long		h;
	struct smk_palette_cache palettes;
};

#define VK_FAIL(result, what) \
	do { \
		VkResult r_ = (result); \
		if (r_ != VK_SUCCESS) { \
			fprintf(stderr, "%s failed: %d\n", (what), (int) r_); \
			return -1; \
		} \
	} while (0)

static void
vk_drawable_size(SDL_Window *window, int *w, int *h)
{
#ifdef SMK_USE_SDL3
	if (!SDL_GetWindowSizeInPixels(window, w, h)) {
		*w = 0;
		*h = 0;
	}
#else
	SDL_Vulkan_GetDrawableSize(window, w, h);
#endif
}

static uint32_t
find_memory_type(VkPhysicalDevice physical, uint32_t bits,
	VkMemoryPropertyFlags wanted)
{
	VkPhysicalDeviceMemoryProperties props;
	uint32_t i;

	vkGetPhysicalDeviceMemoryProperties(physical, &props);
	for (i = 0; i < props.memoryTypeCount; i++) {
		if ((bits & (1u << i)) != 0 &&
			(props.memoryTypes[i].propertyFlags & wanted) == wanted)
			return i;
	}
	return 0;
}

static int
create_image(struct vk_state *st, VkFormat format, uint32_t w, uint32_t h,
	VkImage *image, VkDeviceMemory *memory, VkImageView *view)
{
	VkImageCreateInfo info;
	VkMemoryRequirements req;
	VkMemoryAllocateInfo alloc;
	VkImageViewCreateInfo view_info;

	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	info.imageType = VK_IMAGE_TYPE_2D;
	info.format = format;
	info.extent.width = w;
	info.extent.height = h;
	info.extent.depth = 1;
	info.mipLevels = 1;
	info.arrayLayers = 1;
	info.samples = VK_SAMPLE_COUNT_1_BIT;
	info.tiling = VK_IMAGE_TILING_OPTIMAL;
	info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT |
		VK_IMAGE_USAGE_SAMPLED_BIT;
	info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	VK_FAIL(vkCreateImage(st->device, &info, NULL, image), "vkCreateImage");

	vkGetImageMemoryRequirements(st->device, *image, &req);
	memset(&alloc, 0, sizeof(alloc));
	alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	alloc.allocationSize = req.size;
	alloc.memoryTypeIndex = find_memory_type(st->physical,
		req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	VK_FAIL(vkAllocateMemory(st->device, &alloc, NULL, memory),
		"vkAllocateMemory");
	VK_FAIL(vkBindImageMemory(st->device, *image, *memory, 0),
		"vkBindImageMemory");

	memset(&view_info, 0, sizeof(view_info));
	view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	view_info.image = *image;
	view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
	view_info.format = format;
	view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	view_info.subresourceRange.levelCount = 1;
	view_info.subresourceRange.layerCount = 1;
	VK_FAIL(vkCreateImageView(st->device, &view_info, NULL, view),
		"vkCreateImageView");

	return 0;
}

static int
create_swapchain(struct vk_state *st, int drawable_w, int drawable_h)
{
	VkSurfaceCapabilitiesKHR caps;
	VkSurfaceFormatKHR *formats = NULL;
	uint32_t format_count = 0, i;
	VkSwapchainCreateInfoKHR info;
	VkCompositeAlphaFlagBitsKHR alpha;
	VkExtent2D extent;
	uint32_t count;
	VkImageViewCreateInfo view_info;

	VK_FAIL(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(st->physical,
		st->surface, &caps), "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
	VK_FAIL(vkGetPhysicalDeviceSurfaceFormatsKHR(st->physical, st->surface,
		&format_count, NULL), "vkGetPhysicalDeviceSurfaceFormatsKHR");
	if (format_count == 0) {
		fputs("Vulkan surface reports no formats\n", stderr);
		return -1;
	}
	formats = malloc(sizeof(*formats) * format_count);
	if (formats == NULL)
		return -1;
	VK_FAIL(vkGetPhysicalDeviceSurfaceFormatsKHR(st->physical, st->surface,
		&format_count, formats), "vkGetPhysicalDeviceSurfaceFormatsKHR");

	{
		VkFormat chosen = VK_FORMAT_UNDEFINED;

		/* Keep the format across recreations so the pipeline stays
		 * compatible with the recreated render pass. */
		if (st->format != VK_FORMAT_UNDEFINED) {
			for (i = 0; i < format_count; i++) {
				if (formats[i].format == st->format) {
					chosen = st->format;
					break;
				}
			}
		}
		if (chosen == VK_FORMAT_UNDEFINED) {
			for (i = 0; i < format_count; i++) {
				if (formats[i].colorSpace !=
					VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
					continue;
				if (formats[i].format ==
					VK_FORMAT_B8G8R8A8_UNORM) {
					chosen = VK_FORMAT_B8G8R8A8_UNORM;
					break;
				}
				if (formats[i].format ==
					VK_FORMAT_R8G8B8A8_UNORM &&
					chosen == VK_FORMAT_UNDEFINED)
					chosen = VK_FORMAT_R8G8B8A8_UNORM;
			}
		}
		free(formats);
		if (chosen == VK_FORMAT_UNDEFINED) {
			fputs("Vulkan surface offers no UNORM swapchain format\n",
				stderr);
			return -1;
		}
		st->format = chosen;
	}

	if (caps.currentExtent.width != UINT32_MAX) {
		extent = caps.currentExtent;
	} else {
		uint32_t dw = drawable_w > 0 ? (uint32_t) drawable_w : 1;
		uint32_t dh = drawable_h > 0 ? (uint32_t) drawable_h : 1;

		extent.width = dw < caps.minImageExtent.width ?
			caps.minImageExtent.width : dw;
		extent.width = extent.width > caps.maxImageExtent.width ?
			caps.maxImageExtent.width : extent.width;
		extent.height = dh < caps.minImageExtent.height ?
			caps.minImageExtent.height : dh;
		extent.height = extent.height > caps.maxImageExtent.height ?
			caps.maxImageExtent.height : extent.height;
	}
	if (extent.width < 1)
		extent.width = 1;
	if (extent.height < 1)
		extent.height = 1;

	count = caps.minImageCount + 1;
	if (caps.maxImageCount > 0 && count > caps.maxImageCount)
		count = caps.maxImageCount;

	alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
	if ((caps.supportedCompositeAlpha & alpha) == 0)
		alpha = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;

	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
	info.surface = st->surface;
	info.minImageCount = count;
	info.imageFormat = st->format;
	info.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
	info.imageExtent = extent;
	info.imageArrayLayers = 1;
	info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
	info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
	info.preTransform = caps.currentTransform;
	info.compositeAlpha = alpha;
	info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
	info.clipped = VK_TRUE;
	VK_FAIL(vkCreateSwapchainKHR(st->device, &info, NULL, &st->swapchain),
		"vkCreateSwapchainKHR");

	st->extent = extent;
	VK_FAIL(vkGetSwapchainImagesKHR(st->device, st->swapchain,
		&st->image_count, NULL), "vkGetSwapchainImagesKHR");
	st->images = malloc(sizeof(*st->images) * st->image_count);
	st->views = malloc(sizeof(*st->views) * st->image_count);
	st->framebuffers = calloc(st->image_count, sizeof(*st->framebuffers));
	if (st->images == NULL || st->views == NULL ||
		st->framebuffers == NULL)
		return -1;
	VK_FAIL(vkGetSwapchainImagesKHR(st->device, st->swapchain,
		&st->image_count, st->images), "vkGetSwapchainImagesKHR");

	memset(&view_info, 0, sizeof(view_info));
	view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
	view_info.format = st->format;
	view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	view_info.subresourceRange.levelCount = 1;
	view_info.subresourceRange.layerCount = 1;
	for (i = 0; i < st->image_count; i++) {
		view_info.image = st->images[i];
		VK_FAIL(vkCreateImageView(st->device, &view_info, NULL,
			&st->views[i]), "vkCreateImageView");
	}

	return 0;
}

static int
create_render_pass(struct vk_state *st)
{
	VkAttachmentDescription color;
	VkAttachmentReference ref;
	VkSubpassDescription subpass;
	VkSubpassDependency dep;
	VkRenderPassCreateInfo info;

	memset(&color, 0, sizeof(color));
	color.format = st->format;
	color.samples = VK_SAMPLE_COUNT_1_BIT;
	color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	color.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

	ref.attachment = 0;
	ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

	memset(&subpass, 0, sizeof(subpass));
	subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	subpass.colorAttachmentCount = 1;
	subpass.pColorAttachments = &ref;

	memset(&dep, 0, sizeof(dep));
	dep.srcSubpass = VK_SUBPASS_EXTERNAL;
	dep.dstSubpass = 0;
	dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	dep.srcAccessMask = 0;
	dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
	info.attachmentCount = 1;
	info.pAttachments = &color;
	info.subpassCount = 1;
	info.pSubpasses = &subpass;
	info.dependencyCount = 1;
	info.pDependencies = &dep;
	VK_FAIL(vkCreateRenderPass(st->device, &info, NULL, &st->render_pass),
		"vkCreateRenderPass");

	return 0;
}

static int
create_framebuffers(struct vk_state *st)
{
	VkFramebufferCreateInfo info;
	uint32_t i;

	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
	info.renderPass = st->render_pass;
	info.attachmentCount = 1;
	info.width = st->extent.width;
	info.height = st->extent.height;
	info.layers = 1;
	for (i = 0; i < st->image_count; i++) {
		info.pAttachments = &st->views[i];
		VK_FAIL(vkCreateFramebuffer(st->device, &info, NULL,
			&st->framebuffers[i]), "vkCreateFramebuffer");
	}
	return 0;
}

static VkShaderModule
create_shader_module(struct vk_state *st, const uint32_t *code,
	unsigned long size)
{
	VkShaderModuleCreateInfo info;
	VkShaderModule module = VK_NULL_HANDLE;

	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	info.codeSize = (size_t) size;
	info.pCode = code;
	if (vkCreateShaderModule(st->device, &info, NULL, &module) !=
		VK_SUCCESS)
		return VK_NULL_HANDLE;
	return module;
}

static int
create_pipeline(struct vk_state *st)
{
	VkShaderModule vert, frag;
	VkPipelineShaderStageCreateInfo stages[2];
	VkPipelineVertexInputStateCreateInfo vertex_input;
	VkPipelineInputAssemblyStateCreateInfo assembly;
	VkPipelineViewportStateCreateInfo viewport;
	VkPipelineRasterizationStateCreateInfo raster;
	VkPipelineMultisampleStateCreateInfo multisample;
	VkPipelineColorBlendAttachmentState blend_attachment;
	VkPipelineColorBlendStateCreateInfo blend;
	VkDynamicState dynamic[2];
	VkPipelineDynamicStateCreateInfo dynamic_info;
	VkPipelineLayoutCreateInfo layout_info;
	VkGraphicsPipelineCreateInfo info;
	VkDescriptorSetLayoutBinding bindings[2];
	VkDescriptorSetLayoutCreateInfo set_info;
	VkDescriptorPoolSize pool_size;
	VkDescriptorPoolCreateInfo pool_info;
	VkDescriptorSetAllocateInfo alloc_info;
	VkDescriptorImageInfo image_info[2];
	VkWriteDescriptorSet writes[2];
	VkSamplerCreateInfo sampler_info;
	uint32_t i;

	vert = create_shader_module(st, smk_vk_vert_spv,
		smk_vk_vert_spv_len);
	frag = create_shader_module(st, smk_vk_frag_spv,
		smk_vk_frag_spv_len);
	if (vert == VK_NULL_HANDLE || frag == VK_NULL_HANDLE) {
		fputs("Vulkan shader module creation failed\n", stderr);
		return -1;
	}

	memset(stages, 0, sizeof(stages));
	stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	stages[0].module = vert;
	stages[0].pName = "main";
	stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	stages[1].module = frag;
	stages[1].pName = "main";

	memset(&vertex_input, 0, sizeof(vertex_input));
	vertex_input.sType =
		VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

	memset(&assembly, 0, sizeof(assembly));
	assembly.sType =
		VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
	assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;

	memset(&viewport, 0, sizeof(viewport));
	viewport.sType =
		VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
	viewport.viewportCount = 1;
	viewport.scissorCount = 1;

	memset(&raster, 0, sizeof(raster));
	raster.sType =
		VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
	raster.polygonMode = VK_POLYGON_MODE_FILL;
	raster.cullMode = VK_CULL_MODE_NONE;
	raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
	raster.lineWidth = 1.0f;

	memset(&multisample, 0, sizeof(multisample));
	multisample.sType =
		VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
	multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

	memset(&blend_attachment, 0, sizeof(blend_attachment));
	blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT |
		VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
		VK_COLOR_COMPONENT_A_BIT;
	blend_attachment.blendEnable = VK_FALSE;

	memset(&blend, 0, sizeof(blend));
	blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
	blend.attachmentCount = 1;
	blend.pAttachments = &blend_attachment;

	dynamic[0] = VK_DYNAMIC_STATE_VIEWPORT;
	dynamic[1] = VK_DYNAMIC_STATE_SCISSOR;
	memset(&dynamic_info, 0, sizeof(dynamic_info));
	dynamic_info.sType =
		VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
	dynamic_info.dynamicStateCount = 2;
	dynamic_info.pDynamicStates = dynamic;

	for (i = 0; i < 2; i++) {
		memset(&bindings[i], 0, sizeof(bindings[i]));
		bindings[i].binding = i;
		bindings[i].descriptorType =
			VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		bindings[i].descriptorCount = 1;
		bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	}
	memset(&set_info, 0, sizeof(set_info));
	set_info.sType =
		VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	set_info.bindingCount = 2;
	set_info.pBindings = bindings;
	VK_FAIL(vkCreateDescriptorSetLayout(st->device, &set_info, NULL,
		&st->set_layout), "vkCreateDescriptorSetLayout");

	memset(&layout_info, 0, sizeof(layout_info));
	layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	layout_info.setLayoutCount = 1;
	layout_info.pSetLayouts = &st->set_layout;
	VK_FAIL(vkCreatePipelineLayout(st->device, &layout_info, NULL,
		&st->pipeline_layout), "vkCreatePipelineLayout");

	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
	info.stageCount = 2;
	info.pStages = stages;
	info.pVertexInputState = &vertex_input;
	info.pInputAssemblyState = &assembly;
	info.pViewportState = &viewport;
	info.pRasterizationState = &raster;
	info.pMultisampleState = &multisample;
	info.pColorBlendState = &blend;
	info.pDynamicState = &dynamic_info;
	info.layout = st->pipeline_layout;
	info.renderPass = st->render_pass;
	info.subpass = 0;
	VK_FAIL(vkCreateGraphicsPipelines(st->device, VK_NULL_HANDLE, 1, &info,
		NULL, &st->pipeline), "vkCreateGraphicsPipelines");

	vkDestroyShaderModule(st->device, vert, NULL);
	vkDestroyShaderModule(st->device, frag, NULL);

	memset(&sampler_info, 0, sizeof(sampler_info));
	sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
	sampler_info.magFilter = VK_FILTER_NEAREST;
	sampler_info.minFilter = VK_FILTER_NEAREST;
	sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
	sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sampler_info.maxLod = 0.0f;
	VK_FAIL(vkCreateSampler(st->device, &sampler_info, NULL, &st->sampler),
		"vkCreateSampler");

	pool_size.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	pool_size.descriptorCount = 2;
	memset(&pool_info, 0, sizeof(pool_info));
	pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	pool_info.maxSets = 1;
	pool_info.poolSizeCount = 1;
	pool_info.pPoolSizes = &pool_size;
	VK_FAIL(vkCreateDescriptorPool(st->device, &pool_info, NULL,
		&st->descriptor_pool), "vkCreateDescriptorPool");

	memset(&alloc_info, 0, sizeof(alloc_info));
	alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	alloc_info.descriptorPool = st->descriptor_pool;
	alloc_info.descriptorSetCount = 1;
	alloc_info.pSetLayouts = &st->set_layout;
	VK_FAIL(vkAllocateDescriptorSets(st->device, &alloc_info,
		&st->descriptor_set), "vkAllocateDescriptorSets");

	memset(image_info, 0, sizeof(image_info));
	image_info[0].sampler = st->sampler;
	image_info[0].imageView = st->frame_view;
	image_info[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	image_info[1].sampler = st->sampler;
	image_info[1].imageView = st->palette_view;
	image_info[1].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	memset(writes, 0, sizeof(writes));
	for (i = 0; i < 2; i++) {
		writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[i].dstSet = st->descriptor_set;
		writes[i].dstBinding = i;
		writes[i].descriptorCount = 1;
		writes[i].descriptorType =
			VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		writes[i].pImageInfo = &image_info[i];
	}
	vkUpdateDescriptorSets(st->device, 2, writes, 0, NULL);

	return 0;
}

/* 4-byte aligned palette slot in the staging buffer. */
static VkDeviceSize
palette_offset(const struct vk_state *st)
{
	return (((VkDeviceSize) st->w * st->h) + 3) & ~(VkDeviceSize) 3;
}

static int
create_staging(struct vk_state *st)
{
	VkBufferCreateInfo buffer_info;
	VkMemoryRequirements req;
	VkMemoryAllocateInfo alloc;
	VkDeviceSize size = palette_offset(st) + 256 * 4;

	memset(&buffer_info, 0, sizeof(buffer_info));
	buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	buffer_info.size = size;
	buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
	buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	VK_FAIL(vkCreateBuffer(st->device, &buffer_info, NULL, &st->staging),
		"vkCreateBuffer");

	vkGetBufferMemoryRequirements(st->device, st->staging, &req);
	memset(&alloc, 0, sizeof(alloc));
	alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	alloc.allocationSize = req.size;
	alloc.memoryTypeIndex = find_memory_type(st->physical,
		req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
		VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	VK_FAIL(vkAllocateMemory(st->device, &alloc, NULL,
		&st->staging_memory), "vkAllocateMemory");
	VK_FAIL(vkBindBufferMemory(st->device, st->staging, st->staging_memory,
		0), "vkBindBufferMemory");
	VK_FAIL(vkMapMemory(st->device, st->staging_memory, 0, VK_WHOLE_SIZE,
		0, &st->staging_ptr), "vkMapMemory");
	st->staging_size = size;
	return 0;
}

static int
init_instance(struct vk_state *st, SDL_Window *window)
{
	uint32_t extension_count = 0;
	const char **extensions = NULL;
	VkApplicationInfo app;
	VkInstanceCreateInfo info;

#ifdef SMK_USE_SDL3
	(void) window;
	{
		uint32_t i;
		const char *const *names =
			SDL_Vulkan_GetInstanceExtensions(&extension_count);

		if (names == NULL)
			return -1;
		extensions = malloc(sizeof(*extensions) *
			(extension_count + 1));
		if (extensions == NULL)
			return -1;
		for (i = 0; i < extension_count; i++)
			extensions[i] = names[i];
	}
#else
	if (!SDL_Vulkan_GetInstanceExtensions(window, &extension_count, NULL))
		return -1;
	extensions = malloc(sizeof(*extensions) * (extension_count + 1));
	if (extensions == NULL)
		return -1;
	if (!SDL_Vulkan_GetInstanceExtensions(window, &extension_count,
		extensions)) {
		free(extensions);
		return -1;
	}
#endif

	memset(&app, 0, sizeof(app));
	app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	app.pApplicationName = "smk_vk_player";
	app.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
	app.pEngineName = "libsmacker";
	app.engineVersion = VK_MAKE_VERSION(1, 2, 0);
	app.apiVersion = VK_API_VERSION_1_1;

	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	info.pApplicationInfo = &app;
	info.enabledExtensionCount = extension_count;
	info.ppEnabledExtensionNames = extensions;
#if defined(__APPLE__)
	extensions[extension_count++] =
		VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME;
	info.enabledExtensionCount = extension_count;
	info.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
#endif

	if (vkCreateInstance(&info, NULL, &st->instance) != VK_SUCCESS) {
		free(extensions);
		return -1;
	}
	free(extensions);
	return 0;
}

static int
init_device(struct vk_state *st)
{
	uint32_t count = 0, i, j;
	VkPhysicalDevice *devices = NULL;
	VkPhysicalDeviceProperties props;
	float priority = 1.0f;
	VkDeviceQueueCreateInfo queue_info;
	VkDeviceCreateInfo info;
	const char *extensions[2];
	uint32_t extension_count = 0;
	VkExtensionProperties *available = NULL;
	uint32_t available_count = 0;
	VkPhysicalDeviceFeatures features;

	VK_FAIL(vkEnumeratePhysicalDevices(st->instance, &count, NULL),
		"vkEnumeratePhysicalDevices");
	if (count == 0) {
		fputs("No Vulkan physical devices\n", stderr);
		return -1;
	}
	devices = malloc(sizeof(*devices) * count);
	if (devices == NULL)
		return -1;
	VK_FAIL(vkEnumeratePhysicalDevices(st->instance, &count, devices),
		"vkEnumeratePhysicalDevices");

	for (i = 0; i < count && st->physical == VK_NULL_HANDLE; i++) {
		uint32_t family_count = 0;
		VkQueueFamilyProperties *families;
		VkBool32 present = VK_FALSE;

		vkGetPhysicalDeviceQueueFamilyProperties(devices[i],
			&family_count, NULL);
		families = malloc(sizeof(*families) * family_count);
		if (families == NULL)
			continue;
		vkGetPhysicalDeviceQueueFamilyProperties(devices[i],
			&family_count, families);
		for (j = 0; j < family_count; j++) {
			if ((families[j].queueFlags &
				VK_QUEUE_GRAPHICS_BIT) == 0)
				continue;
			if (vkGetPhysicalDeviceSurfaceSupportKHR(devices[i], j,
				st->surface, &present) != VK_SUCCESS)
				continue;
			if (present) {
				st->physical = devices[i];
				st->queue_family = j;
				break;
			}
		}
		free(families);
	}
	free(devices);
	if (st->physical == VK_NULL_HANDLE) {
		fputs("No Vulkan device with a graphics/present queue\n",
			stderr);
		return -1;
	}

	vkGetPhysicalDeviceProperties(st->physical, &props);
	if ((unsigned long) props.limits.maxImageDimension2D < st->w ||
		(unsigned long) props.limits.maxImageDimension2D < st->h) {
		fprintf(stderr,
			"%lu x %lu exceeds the Vulkan image limit of %u\n",
			st->w, st->h, props.limits.maxImageDimension2D);
		return -1;
	}

	extensions[extension_count++] = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
	vkEnumerateDeviceExtensionProperties(st->physical, NULL,
		&available_count, NULL);
	available = malloc(sizeof(*available) * available_count);
	if (available != NULL) {
		vkEnumerateDeviceExtensionProperties(st->physical, NULL,
			&available_count, available);
		for (i = 0; i < available_count; i++) {
			if (strcmp(available[i].extensionName,
				VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME) == 0) {
				extensions[extension_count++] =
					VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME;
				break;
			}
		}
		free(available);
	}

	memset(&queue_info, 0, sizeof(queue_info));
	queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
	queue_info.queueFamilyIndex = st->queue_family;
	queue_info.queueCount = 1;
	queue_info.pQueuePriorities = &priority;
	memset(&features, 0, sizeof(features));
	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
	info.queueCreateInfoCount = 1;
	info.pQueueCreateInfos = &queue_info;
	info.pEnabledFeatures = &features;
	info.enabledExtensionCount = extension_count;
	info.ppEnabledExtensionNames = extensions;
	VK_FAIL(vkCreateDevice(st->physical, &info, NULL, &st->device),
		"vkCreateDevice");
	vkGetDeviceQueue(st->device, st->queue_family, 0, &st->queue);
	return 0;
}

static int
init_commands(struct vk_state *st)
{
	VkCommandPoolCreateInfo pool_info;
	VkCommandBufferAllocateInfo alloc_info;
	VkFenceCreateInfo fence_info;
	VkSemaphoreCreateInfo semaphore_info;

	memset(&pool_info, 0, sizeof(pool_info));
	pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	pool_info.queueFamilyIndex = st->queue_family;
	VK_FAIL(vkCreateCommandPool(st->device, &pool_info, NULL,
		&st->command_pool), "vkCreateCommandPool");

	memset(&alloc_info, 0, sizeof(alloc_info));
	alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	alloc_info.commandPool = st->command_pool;
	alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	alloc_info.commandBufferCount = 1;
	VK_FAIL(vkAllocateCommandBuffers(st->device, &alloc_info, &st->command),
		"vkAllocateCommandBuffers");

	memset(&fence_info, 0, sizeof(fence_info));
	fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;
	VK_FAIL(vkCreateFence(st->device, &fence_info, NULL, &st->fence),
		"vkCreateFence");

	memset(&semaphore_info, 0, sizeof(semaphore_info));
	semaphore_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
	VK_FAIL(vkCreateSemaphore(st->device, &semaphore_info, NULL,
		&st->image_available), "vkCreateSemaphore");
	VK_FAIL(vkCreateSemaphore(st->device, &semaphore_info, NULL,
		&st->render_finished), "vkCreateSemaphore");
	return 0;
}

static int
vk_init(struct vk_state *st, SDL_Window *window, unsigned long w,
	unsigned long h)
{
	int drawable_w = 0, drawable_h = 0;

	memset(st, 0, sizeof(*st));
	st->w = w;
	st->h = h;
	st->frame_layout = VK_IMAGE_LAYOUT_UNDEFINED;
	st->palette_layout = VK_IMAGE_LAYOUT_UNDEFINED;
	st->physical = VK_NULL_HANDLE;

	if (init_instance(st, window) != 0) {
		fputs("Vulkan instance creation failed\n", stderr);
		return -1;
	}

#ifdef SMK_USE_SDL3
	if (!SDL_Vulkan_CreateSurface(window, st->instance, NULL,
		&st->surface))
#else
	if (!SDL_Vulkan_CreateSurface(window, st->instance, &st->surface))
#endif
	{
		fprintf(stderr, "SDL_Vulkan_CreateSurface failed: %s\n",
			SDL_GetError());
		return -1;
	}

	if (init_device(st) != 0)
		return -1;

	vk_drawable_size(window, &drawable_w, &drawable_h);
	if (create_swapchain(st, drawable_w, drawable_h) != 0 ||
		create_render_pass(st) != 0 ||
		create_framebuffers(st) != 0)
		return -1;

	if (create_image(st, VK_FORMAT_R8_UNORM, (uint32_t) w, (uint32_t) h,
		&st->frame_image, &st->frame_memory, &st->frame_view) != 0 ||
		create_image(st, VK_FORMAT_R8G8B8A8_UNORM, 256, 1,
		&st->palette_image, &st->palette_memory,
		&st->palette_view) != 0)
		return -1;

	if (create_pipeline(st) != 0)
		return -1;
	if (init_commands(st) != 0)
		return -1;
	if (create_staging(st) != 0)
		return -1;

	return 0;
}

static void
destroy_swapchain(struct vk_state *st)
{
	uint32_t i;

	if (st->device == VK_NULL_HANDLE)
		return;
	for (i = 0; i < st->image_count; i++) {
		if (st->framebuffers != NULL &&
			st->framebuffers[i] != VK_NULL_HANDLE)
			vkDestroyFramebuffer(st->device, st->framebuffers[i],
				NULL);
		if (st->views != NULL && st->views[i] != VK_NULL_HANDLE)
			vkDestroyImageView(st->device, st->views[i], NULL);
	}
	free(st->framebuffers);
	free(st->views);
	free(st->images);
	st->framebuffers = NULL;
	st->views = NULL;
	st->images = NULL;
	st->image_count = 0;
	if (st->render_pass != VK_NULL_HANDLE) {
		vkDestroyRenderPass(st->device, st->render_pass, NULL);
		st->render_pass = VK_NULL_HANDLE;
	}
	if (st->swapchain != VK_NULL_HANDLE) {
		vkDestroySwapchainKHR(st->device, st->swapchain, NULL);
		st->swapchain = VK_NULL_HANDLE;
	}
}

static void
vk_shutdown(void *ctx)
{
	struct vk_state *st = ctx;

	if (st == NULL)
		return;
	if (st->device != VK_NULL_HANDLE)
		vkDeviceWaitIdle(st->device);

	destroy_swapchain(st);

	if (st->device != VK_NULL_HANDLE) {
		if (st->staging_ptr != NULL)
			vkUnmapMemory(st->device, st->staging_memory);
		if (st->staging != VK_NULL_HANDLE)
			vkDestroyBuffer(st->device, st->staging, NULL);
		if (st->staging_memory != VK_NULL_HANDLE)
			vkFreeMemory(st->device, st->staging_memory, NULL);
		if (st->sampler != VK_NULL_HANDLE)
			vkDestroySampler(st->device, st->sampler, NULL);
		if (st->frame_view != VK_NULL_HANDLE)
			vkDestroyImageView(st->device, st->frame_view, NULL);
		if (st->frame_image != VK_NULL_HANDLE)
			vkDestroyImage(st->device, st->frame_image, NULL);
		if (st->frame_memory != VK_NULL_HANDLE)
			vkFreeMemory(st->device, st->frame_memory, NULL);
		if (st->palette_view != VK_NULL_HANDLE)
			vkDestroyImageView(st->device, st->palette_view, NULL);
		if (st->palette_image != VK_NULL_HANDLE)
			vkDestroyImage(st->device, st->palette_image, NULL);
		if (st->palette_memory != VK_NULL_HANDLE)
			vkFreeMemory(st->device, st->palette_memory, NULL);
		if (st->descriptor_pool != VK_NULL_HANDLE)
			vkDestroyDescriptorPool(st->device,
				st->descriptor_pool, NULL);
		if (st->set_layout != VK_NULL_HANDLE)
			vkDestroyDescriptorSetLayout(st->device,
				st->set_layout, NULL);
		if (st->pipeline != VK_NULL_HANDLE)
			vkDestroyPipeline(st->device, st->pipeline, NULL);
		if (st->pipeline_layout != VK_NULL_HANDLE)
			vkDestroyPipelineLayout(st->device,
				st->pipeline_layout, NULL);
		if (st->image_available != VK_NULL_HANDLE)
			vkDestroySemaphore(st->device, st->image_available,
				NULL);
		if (st->render_finished != VK_NULL_HANDLE)
			vkDestroySemaphore(st->device, st->render_finished,
				NULL);
		if (st->fence != VK_NULL_HANDLE)
			vkDestroyFence(st->device, st->fence, NULL);
		if (st->command_pool != VK_NULL_HANDLE)
			vkDestroyCommandPool(st->device, st->command_pool,
				NULL);
		vkDestroyDevice(st->device, NULL);
		st->device = VK_NULL_HANDLE;
	}

	if (st->surface != VK_NULL_HANDLE && st->instance != VK_NULL_HANDLE) {
		vkDestroySurfaceKHR(st->instance, st->surface, NULL);
		st->surface = VK_NULL_HANDLE;
	}
	if (st->instance != VK_NULL_HANDLE) {
		vkDestroyInstance(st->instance, NULL);
		st->instance = VK_NULL_HANDLE;
	}
}

static int
recreate_swapchain(struct vk_state *st, int drawable_w, int drawable_h)
{
	vkDeviceWaitIdle(st->device);
	destroy_swapchain(st);
	if (create_swapchain(st, drawable_w, drawable_h) != 0 ||
		create_render_pass(st) != 0 ||
		create_framebuffers(st) != 0)
		return -1;
	return 0;
}

static void
image_barrier(VkCommandBuffer command, VkImage image,
	VkImageLayout old_layout, VkImageLayout new_layout)
{
	VkImageMemoryBarrier barrier;
	VkPipelineStageFlags src_stage, dst_stage;

	memset(&barrier, 0, sizeof(barrier));
	barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	barrier.oldLayout = old_layout;
	barrier.newLayout = new_layout;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image = image;
	barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	barrier.subresourceRange.levelCount = 1;
	barrier.subresourceRange.layerCount = 1;

	if (new_layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
		src_stage = old_layout == VK_IMAGE_LAYOUT_UNDEFINED ?
			VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT :
			VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
		dst_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
		barrier.srcAccessMask = old_layout ==
			VK_IMAGE_LAYOUT_UNDEFINED ? 0 :
			VK_ACCESS_SHADER_READ_BIT;
		barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	} else {
		src_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
		dst_stage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
		barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
	}

	vkCmdPipelineBarrier(command, src_stage, dst_stage, 0, 0, NULL, 0,
		NULL, 1, &barrier);
}

static int
vk_present(void *ctx, const unsigned char *frame,
	const unsigned char *palette, int drawable_w, int drawable_h,
	int y_double)
{
	struct vk_state *st = ctx;
	VkResult result;
	uint32_t index = 0;
	VkCommandBufferBeginInfo begin_info;
	VkBufferMemoryBarrier buffer_barrier;
	VkBufferImageCopy copy;
	VkRenderPassBeginInfo pass_info;
	VkClearValue clear;
	VkViewport viewport;
	VkRect2D scissor;
	VkSubmitInfo submit_info;
	VkPresentInfoKHR present_info;
	VkPipelineStageFlags wait_stage;
	int palette_dirty;
	int x, y, vw, vh;
	unsigned char *base = st->staging_ptr;
	unsigned char *palette_slot = base + palette_offset(st);

	if (drawable_w <= 0 || drawable_h <= 0)
		return 1;

	result = vkWaitForFences(st->device, 1, &st->fence, VK_TRUE,
		UINT64_MAX);
	if (result != VK_SUCCESS) {
		fprintf(stderr, "vkWaitForFences failed: %d\n", (int) result);
		return -1;
	}

	if ((uint32_t) drawable_w != st->extent.width ||
		(uint32_t) drawable_h != st->extent.height) {
		if (recreate_swapchain(st, drawable_w, drawable_h) != 0)
			return -1;
	}

	result = vkAcquireNextImageKHR(st->device, st->swapchain, UINT64_MAX,
		st->image_available, VK_NULL_HANDLE, &index);
	if (result == VK_ERROR_OUT_OF_DATE_KHR) {
		if (recreate_swapchain(st, drawable_w, drawable_h) != 0)
			return -1;
		return 1;
	}
	if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
		fprintf(stderr, "vkAcquireNextImageKHR failed: %d\n",
			(int) result);
		return -1;
	}

	memcpy(base, frame, (size_t) st->w * st->h);
	palette_dirty = smk_palette_changed(&st->palettes, palette);
	if (palette_dirty) {
		unsigned int i;

		for (i = 0; i < 256; i++) {
			palette_slot[i * 4 + 0] = palette[i * 3 + 0];
			palette_slot[i * 4 + 1] = palette[i * 3 + 1];
			palette_slot[i * 4 + 2] = palette[i * 3 + 2];
			palette_slot[i * 4 + 3] = 255;
		}
	}

	VK_FAIL(vkResetCommandBuffer(st->command, 0),
		"vkResetCommandBuffer");
	memset(&begin_info, 0, sizeof(begin_info));
	begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	VK_FAIL(vkBeginCommandBuffer(st->command, &begin_info),
		"vkBeginCommandBuffer");

	memset(&buffer_barrier, 0, sizeof(buffer_barrier));
	buffer_barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
	buffer_barrier.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
	buffer_barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	buffer_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	buffer_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	buffer_barrier.buffer = st->staging;
	buffer_barrier.offset = 0;
	buffer_barrier.size = VK_WHOLE_SIZE;
	vkCmdPipelineBarrier(st->command, VK_PIPELINE_STAGE_HOST_BIT,
		VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 1,
		&buffer_barrier, 0, NULL);

	image_barrier(st->command, st->frame_image, st->frame_layout,
		VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
	memset(&copy, 0, sizeof(copy));
	copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	copy.imageSubresource.layerCount = 1;
	copy.imageExtent.width = (uint32_t) st->w;
	copy.imageExtent.height = (uint32_t) st->h;
	copy.imageExtent.depth = 1;
	vkCmdCopyBufferToImage(st->command, st->staging, st->frame_image,
		VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
	image_barrier(st->command, st->frame_image,
		VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	st->frame_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

	if (palette_dirty) {
		image_barrier(st->command, st->palette_image,
			st->palette_layout,
			VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
		memset(&copy, 0, sizeof(copy));
		copy.bufferOffset = palette_offset(st);
		copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		copy.imageSubresource.layerCount = 1;
		copy.imageExtent.width = 256;
		copy.imageExtent.height = 1;
		copy.imageExtent.depth = 1;
		vkCmdCopyBufferToImage(st->command, st->staging,
			st->palette_image,
			VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
		image_barrier(st->command, st->palette_image,
			VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
		st->palette_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		smk_palette_store(&st->palettes, palette);
	}

	memset(&clear, 0, sizeof(clear));
	clear.color.float32[3] = 1.0f;

	memset(&pass_info, 0, sizeof(pass_info));
	pass_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
	pass_info.renderPass = st->render_pass;
	pass_info.framebuffer = st->framebuffers[index];
	pass_info.renderArea.extent = st->extent;
	pass_info.clearValueCount = 1;
	pass_info.pClearValues = &clear;
	vkCmdBeginRenderPass(st->command, &pass_info,
		VK_SUBPASS_CONTENTS_INLINE);

	smk_player_letterbox(drawable_w, drawable_h, st->w,
		y_double ? st->h * 2 : st->h, &x, &y, &vw, &vh);
	scissor.offset.x = 0;
	scissor.offset.y = 0;
	scissor.extent = st->extent;
	vkCmdSetScissor(st->command, 0, 1, &scissor);

	if (vw > 0 && vh > 0) {
		viewport.x = (float) x;
		viewport.y = (float) y;
		viewport.width = (float) vw;
		viewport.height = (float) vh;
		viewport.minDepth = 0.0f;
		viewport.maxDepth = 1.0f;
		vkCmdSetViewport(st->command, 0, 1, &viewport);

		vkCmdBindPipeline(st->command,
			VK_PIPELINE_BIND_POINT_GRAPHICS, st->pipeline);
		vkCmdBindDescriptorSets(st->command,
			VK_PIPELINE_BIND_POINT_GRAPHICS, st->pipeline_layout,
			0, 1, &st->descriptor_set, 0, NULL);
		vkCmdDraw(st->command, 4, 1, 0, 0);
	}

	vkCmdEndRenderPass(st->command);
	VK_FAIL(vkEndCommandBuffer(st->command), "vkEndCommandBuffer");

	wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	memset(&submit_info, 0, sizeof(submit_info));
	submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submit_info.waitSemaphoreCount = 1;
	submit_info.pWaitSemaphores = &st->image_available;
	submit_info.pWaitDstStageMask = &wait_stage;
	submit_info.commandBufferCount = 1;
	submit_info.pCommandBuffers = &st->command;
	submit_info.signalSemaphoreCount = 1;
	submit_info.pSignalSemaphores = &st->render_finished;
	VK_FAIL(vkResetFences(st->device, 1, &st->fence),
		"vkResetFences");
	VK_FAIL(vkQueueSubmit(st->queue, 1, &submit_info, st->fence),
		"vkQueueSubmit");

	memset(&present_info, 0, sizeof(present_info));
	present_info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
	present_info.waitSemaphoreCount = 1;
	present_info.pWaitSemaphores = &st->render_finished;
	present_info.swapchainCount = 1;
	present_info.pSwapchains = &st->swapchain;
	present_info.pImageIndices = &index;
	result = vkQueuePresentKHR(st->queue, &present_info);
	if (result == VK_ERROR_OUT_OF_DATE_KHR ||
		result == VK_SUBOPTIMAL_KHR) {
		if (recreate_swapchain(st, drawable_w, drawable_h) != 0)
			return -1;
		return 1;
	}
	if (result != VK_SUCCESS) {
		fprintf(stderr, "vkQueuePresentKHR failed: %d\n",
			(int) result);
		return -1;
	}

	return 0;
}

int
main(int argc, char *argv[])
{
	struct smk_player_options	opt;
	struct smk_sdl_player		player;
	struct vk_state			state;
	struct smk_sdl_render_ops	ops;
	smk				s;
	unsigned long			w = 0, h = 0;
	double				usf = 0.0;
	unsigned char			y_scale = SMK_FLAG_Y_NONE;
	int				y_double;
	int				status;
	int				rc;
	char				title[512];

	rc = smk_player_parse_options(argc, argv, &opt);
	if (rc != 0)
		return rc > 0 ? 0 : 1;

	s = smk_open_file(opt.path, SMK_MODE_DISK);
	if (s == NULL) {
		fprintf(stderr, "Errors encountered opening %s, exiting.\n",
			opt.path);
		return 1;
	}

	smk_info_all(s, NULL, NULL, &usf);
	smk_info_video(s, &w, &h, &y_scale);
	if (w == 0 || h == 0) {
		fprintf(stderr, "%s has no video track, exiting.\n", opt.path);
		smk_close(s);
		return 1;
	}
	y_double = y_scale != SMK_FLAG_Y_NONE;
	if (w > (unsigned long) INT_MAX ||
		h > (unsigned long) (INT_MAX / (y_double ? 2 : 1))) {
		fprintf(stderr, "%s has unsupported video dimensions: %lu x %lu\n",
			opt.path, w, h);
		smk_close(s);
		return 1;
	}

	snprintf(title, sizeof(title), "libsmacker Vulkan player - %s",
		opt.path);
	if (smk_sdl_open(&player, title, w, h, y_double, SDL_WINDOW_VULKAN)
		!= 0) {
		fprintf(stderr, "SDL window creation failed: %s\n",
			SDL_GetError());
		smk_close(s);
		return 1;
	}

	if (vk_init(&state, player.window, w, h) != 0) {
		vk_shutdown(&state);
		smk_sdl_close(&player);
		smk_close(s);
		return 1;
	}

	ops.present = vk_present;
	ops.shutdown = vk_shutdown;
	status = smk_sdl_run(&player, s, &opt, &ops, &state);

	smk_sdl_close(&player);
	smk_close(s);
	return status;
}
