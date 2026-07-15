/*
	Phase 2 Vulkan backend: window + swapchain clear only.
	All other gfx API entry points are deliberate log-once stubs.
*/

#include <array>
#include <atomic>
#include <cstdlib>
#include <vector>

#include <vulkan/vulkan.h>

#define VMA_IMPLEMENTATION
#include <vk_mem_alloc.h>

#include <VkBootstrap.h>

#ifdef USE_GLFW
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#endif

extern "C" {
#include "gfx.h"
#include "gfx_backend.h"
#include "log.h"
}

namespace {

constexpr uint32_t kFramesInFlight = 2;
/* Distinctive dark teal clear (phase 2 visual marker). */
constexpr float kClearR = 0.05f;
constexpr float kClearG = 0.28f;
constexpr float kClearB = 0.30f;
constexpr float kClearA = 1.0f;

struct FrameSync {
	VkCommandPool pool = VK_NULL_HANDLE;
	VkCommandBuffer cmd = VK_NULL_HANDLE;
	VkSemaphore image_available = VK_NULL_HANDLE;
	VkFence in_flight = VK_NULL_HANDLE;
};

struct GfxVk {
	GLFWwindow* window = nullptr;

	vkb::Instance vkb_instance{};
	VkInstance instance = VK_NULL_HANDLE;
	VkSurfaceKHR surface = VK_NULL_HANDLE;
	vkb::PhysicalDevice vkb_phys{};
	vkb::Device vkb_device{};
	VkDevice device = VK_NULL_HANDLE;
	VkQueue graphics_queue = VK_NULL_HANDLE;
	uint32_t graphics_queue_family = 0;

	VmaAllocator allocator = VK_NULL_HANDLE;

	vkb::Swapchain vkb_swapchain{};
	VkSwapchainKHR swapchain = VK_NULL_HANDLE;
	VkFormat swapchain_format = VK_FORMAT_UNDEFINED;
	VkExtent2D swapchain_extent{};
	std::vector<VkImage> swapchain_images;
	std::vector<VkImageView> swapchain_views;

	bool use_dynamic_rendering = false;
	VkRenderPass render_pass = VK_NULL_HANDLE;
	std::vector<VkFramebuffer> framebuffers;

	std::array<FrameSync, kFramesInFlight> frames{};
	std::vector<VkFence> images_in_flight;
	std::vector<VkSemaphore> render_finished_for_image; /* one per swapchain image (semaphore reuse) */
	uint32_t frame_index = 0;

	int pending_w = 0;
	int pending_h = 0;
	bool swapchain_dirty = false;
	bool vsync = true; /* FIFO always for phase 2 */

	uint32_t api_version = VK_API_VERSION_1_2;
};

GfxVk g;

#define GFX_VK_STUB_ONCE(fn)                                                                                           \
	do {                                                                                                               \
		static std::atomic<bool> logged{false};                                                                        \
		bool expected = false;                                                                                         \
		if(logged.compare_exchange_strong(expected, true)) {                                                           \
			log_warn("gfx_vk stub (once): %s", fn);                                                                    \
		}                                                                                                              \
	} while(0)

bool want_validation_layers() {
	const char* env = std::getenv("BUTTERSPADES_VK_VALIDATION");
	if(env) {
		if(env[0] == '0' && env[1] == '\0')
			return false;
		if(env[0] == '1' && env[1] == '\0')
			return true;
	}
#ifdef NDEBUG
	return false;
#else
	return true;
#endif
}

VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
											  VkDebugUtilsMessageTypeFlagsEXT types,
											  const VkDebugUtilsMessengerCallbackDataEXT* data, void* user_data) {
	(void)types;
	(void)user_data;
	if(severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
		log_error("[vk] %s", data->pMessage);
	else if(severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
		log_warn("[vk] %s", data->pMessage);
	else
		log_info("[vk] %s", data->pMessage);
	return VK_FALSE;
}

void destroy_framebuffers() {
	for(VkFramebuffer fb : g.framebuffers) {
		if(fb)
			vkDestroyFramebuffer(g.device, fb, nullptr);
	}
	g.framebuffers.clear();
}

void destroy_swapchain_views() {
	for(VkImageView v : g.swapchain_views) {
		if(v)
			vkDestroyImageView(g.device, v, nullptr);
	}
	g.swapchain_views.clear();
	g.swapchain_images.clear();
}

void destroy_render_finished_semaphores() {
	for(VkSemaphore s : g.render_finished_for_image) {
		if(s)
			vkDestroySemaphore(g.device, s, nullptr);
	}
	g.render_finished_for_image.clear();
}

bool create_render_finished_semaphores() {
	destroy_render_finished_semaphores();
	g.render_finished_for_image.resize(g.swapchain_images.size());
	VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
	for(size_t i = 0; i < g.render_finished_for_image.size(); i++) {
		if(vkCreateSemaphore(g.device, &sci, nullptr, &g.render_finished_for_image[i]) != VK_SUCCESS)
			return false;
	}
	return true;
}

void destroy_swapchain_objects() {
	destroy_framebuffers();
	destroy_swapchain_views();
	destroy_render_finished_semaphores();
	if(g.vkb_swapchain.swapchain != VK_NULL_HANDLE) {
		vkb::destroy_swapchain(g.vkb_swapchain);
		g.vkb_swapchain = {};
		g.swapchain = VK_NULL_HANDLE;
	}
}

bool create_render_pass_12() {
	VkAttachmentDescription color{};
	color.format = g.swapchain_format;
	color.samples = VK_SAMPLE_COUNT_1_BIT;
	color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	color.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

	VkAttachmentReference color_ref{};
	color_ref.attachment = 0;
	color_ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

	VkSubpassDescription subpass{};
	subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	subpass.colorAttachmentCount = 1;
	subpass.pColorAttachments = &color_ref;

	VkSubpassDependency dep{};
	dep.srcSubpass = VK_SUBPASS_EXTERNAL;
	dep.dstSubpass = 0;
	dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	dep.srcAccessMask = 0;
	dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

	VkRenderPassCreateInfo rpci{};
	rpci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
	rpci.attachmentCount = 1;
	rpci.pAttachments = &color;
	rpci.subpassCount = 1;
	rpci.pSubpasses = &subpass;
	rpci.dependencyCount = 1;
	rpci.pDependencies = &dep;

	if(vkCreateRenderPass(g.device, &rpci, nullptr, &g.render_pass) != VK_SUCCESS) {
		log_error("vkCreateRenderPass failed");
		return false;
	}
	return true;
}

bool create_framebuffers_12() {
	g.framebuffers.resize(g.swapchain_views.size());
	for(size_t i = 0; i < g.swapchain_views.size(); i++) {
		VkFramebufferCreateInfo fci{};
		fci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
		fci.renderPass = g.render_pass;
		fci.attachmentCount = 1;
		fci.pAttachments = &g.swapchain_views[i];
		fci.width = g.swapchain_extent.width;
		fci.height = g.swapchain_extent.height;
		fci.layers = 1;
		if(vkCreateFramebuffer(g.device, &fci, nullptr, &g.framebuffers[i]) != VK_SUCCESS) {
			log_error("vkCreateFramebuffer failed");
			return false;
		}
	}
	return true;
}

bool create_swapchain() {
	vkb::SwapchainBuilder builder{g.vkb_device, g.surface};
	builder.set_desired_present_mode(VK_PRESENT_MODE_FIFO_KHR);
	builder.set_desired_format({VK_FORMAT_B8G8R8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR});
	builder.add_fallback_format({VK_FORMAT_R8G8B8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR});
	builder.add_fallback_format({VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR});
	builder.add_fallback_format({VK_FORMAT_R8G8B8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR});

	vkb::Swapchain old = g.vkb_swapchain;
	if(old.swapchain != VK_NULL_HANDLE)
		builder.set_old_swapchain(old);

	auto swap_ret = builder.build();
	if(!swap_ret) {
		log_error("Swapchain creation failed: %s", swap_ret.error().message().c_str());
		return false;
	}

	destroy_framebuffers();
	destroy_swapchain_views();
	if(old.swapchain != VK_NULL_HANDLE)
		vkb::destroy_swapchain(old);

	g.vkb_swapchain = swap_ret.value();
	g.swapchain = g.vkb_swapchain.swapchain;
	g.swapchain_format = g.vkb_swapchain.image_format;
	g.swapchain_extent = g.vkb_swapchain.extent;

	auto images = g.vkb_swapchain.get_images();
	auto views = g.vkb_swapchain.get_image_views();
	if(!images || !views) {
		log_error("Failed to get swapchain images/views");
		return false;
	}
	g.swapchain_images = images.value();
	g.swapchain_views = views.value();
	g.images_in_flight.assign(g.swapchain_images.size(), VK_NULL_HANDLE);
	if(!create_render_finished_semaphores()) {
		log_error("Failed to create per-image render-finished semaphores");
		return false;
	}

	if(!g.use_dynamic_rendering) {
		if(!g.render_pass && !create_render_pass_12())
			return false;
		if(!create_framebuffers_12())
			return false;
	}

	const char* fmt_name = "unknown";
	switch(g.swapchain_format) {
		case VK_FORMAT_B8G8R8A8_SRGB: fmt_name = "B8G8R8A8_SRGB"; break;
		case VK_FORMAT_R8G8B8A8_SRGB: fmt_name = "R8G8B8A8_SRGB"; break;
		case VK_FORMAT_B8G8R8A8_UNORM: fmt_name = "B8G8R8A8_UNORM"; break;
		case VK_FORMAT_R8G8B8A8_UNORM: fmt_name = "R8G8B8A8_UNORM"; break;
		default: break;
	}
	log_info("Vulkan swapchain: %ux%u format=%s present=FIFO", g.swapchain_extent.width, g.swapchain_extent.height,
			 fmt_name);
	g.swapchain_dirty = false;
	return true;
}

bool recreate_swapchain() {
	if(!g.window)
		return false;
	int fb_w = 0, fb_h = 0;
	glfwGetFramebufferSize(g.window, &fb_w, &fb_h);
	if(fb_w == 0 || fb_h == 0) {
		g.swapchain_dirty = true;
		return true; /* skip until restored */
	}
	vkDeviceWaitIdle(g.device);
	return create_swapchain();
}

void destroy_frames() {
	for(auto& f : g.frames) {
		if(f.in_flight)
			vkDestroyFence(g.device, f.in_flight, nullptr);
		if(f.image_available)
			vkDestroySemaphore(g.device, f.image_available, nullptr);
		if(f.pool)
			vkDestroyCommandPool(g.device, f.pool, nullptr);
		f = {};
	}
}

bool create_frames() {
	for(uint32_t i = 0; i < kFramesInFlight; i++) {
		FrameSync& f = g.frames[i];
		VkCommandPoolCreateInfo pci{};
		pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
		pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
		pci.queueFamilyIndex = g.graphics_queue_family;
		if(vkCreateCommandPool(g.device, &pci, nullptr, &f.pool) != VK_SUCCESS)
			return false;

		VkCommandBufferAllocateInfo cai{};
		cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
		cai.commandPool = f.pool;
		cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		cai.commandBufferCount = 1;
		if(vkAllocateCommandBuffers(g.device, &cai, &f.cmd) != VK_SUCCESS)
			return false;

		VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
		VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
		fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
		if(vkCreateSemaphore(g.device, &sci, nullptr, &f.image_available) != VK_SUCCESS)
			return false;
		if(vkCreateFence(g.device, &fci, nullptr, &f.in_flight) != VK_SUCCESS)
			return false;
	}
	return true;
}

void record_clear(VkCommandBuffer cmd, uint32_t image_index) {
	VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
	begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkBeginCommandBuffer(cmd, &begin);

	VkClearValue clear{};
	clear.color.float32[0] = kClearR;
	clear.color.float32[1] = kClearG;
	clear.color.float32[2] = kClearB;
	clear.color.float32[3] = kClearA;

	if(g.use_dynamic_rendering) {
		VkImageMemoryBarrier to_color{};
		to_color.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		to_color.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		to_color.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		to_color.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		to_color.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		to_color.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		to_color.image = g.swapchain_images[image_index];
		to_color.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		to_color.subresourceRange.levelCount = 1;
		to_color.subresourceRange.layerCount = 1;
		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0,
							 nullptr, 0, nullptr, 1, &to_color);

		VkRenderingAttachmentInfo color_att{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
		color_att.imageView = g.swapchain_views[image_index];
		color_att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		color_att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
		color_att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		color_att.clearValue = clear;

		VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
		ri.renderArea.extent = g.swapchain_extent;
		ri.layerCount = 1;
		ri.colorAttachmentCount = 1;
		ri.pColorAttachments = &color_att;
		vkCmdBeginRendering(cmd, &ri);
		vkCmdEndRendering(cmd);

		VkImageMemoryBarrier to_present = to_color;
		to_present.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		to_present.dstAccessMask = 0;
		to_present.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		to_present.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0,
							 0, nullptr, 0, nullptr, 1, &to_present);
	} else {
		VkRenderPassBeginInfo rpbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
		rpbi.renderPass = g.render_pass;
		rpbi.framebuffer = g.framebuffers[image_index];
		rpbi.renderArea.extent = g.swapchain_extent;
		rpbi.clearValueCount = 1;
		rpbi.pClearValues = &clear;
		vkCmdBeginRenderPass(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
		vkCmdEndRenderPass(cmd);
	}

	vkEndCommandBuffer(cmd);
}

} // namespace

extern "C" void gfx_vk_apply_context_hints(void) {
#ifdef USE_GLFW
	glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
	glfwWindowHint(GLFW_COCOA_RETINA_FRAMEBUFFER, GLFW_FALSE);
#else
	log_error("Vulkan backend requires GLFW in this phase");
#endif
}

extern "C" void gfx_vk_init(void* window) {
	g.window = static_cast<GLFWwindow*>(window);

	const bool validation = want_validation_layers();
	bool layers_enabled = false;
	log_info("Vulkan validation layers: %s (override with BUTTERSPADES_VK_VALIDATION=0|1)",
			 validation ? "requested" : "off");

	auto sysinfo_ret = vkb::SystemInfo::get_system_info();
	if(validation && sysinfo_ret) {
		layers_enabled = sysinfo_ret->is_layer_available("VK_LAYER_KHRONOS_validation");
		if(!layers_enabled)
			log_warn("VK_LAYER_KHRONOS_validation not available (set VK_LAYER_PATH to the MinGW bin dir)");
	}

	vkb::InstanceBuilder ib;
	ib.set_app_name("butterspades")
		.set_engine_name("butterspades")
		.require_api_version(1, 3, 0)
		.set_debug_callback(debug_callback);
	if(layers_enabled)
		ib.request_validation_layers();

	uint32_t glfw_ext_count = 0;
	const char** glfw_exts = glfwGetRequiredInstanceExtensions(&glfw_ext_count);
	if(!glfw_exts || glfw_ext_count == 0) {
		log_fatal("glfwGetRequiredInstanceExtensions failed (Vulkan support in GLFW?)");
		exit(1);
	}
	ib.enable_extensions(glfw_ext_count, glfw_exts);

	auto inst13 = ib.build();
	if(inst13) {
		g.vkb_instance = inst13.value();
		g.use_dynamic_rendering = true;
		g.api_version = VK_API_VERSION_1_3;
		log_info("Vulkan instance API: 1.3 (dynamic rendering)%s", layers_enabled ? " + validation" : "");
	} else {
		log_warn("Vulkan 1.3 instance failed (%s); falling back to 1.2", inst13.error().message().c_str());
		vkb::InstanceBuilder ib12;
		ib12.set_app_name("butterspades")
			.set_engine_name("butterspades")
			.require_api_version(1, 2, 0)
			.set_debug_callback(debug_callback);
		if(layers_enabled)
			ib12.request_validation_layers();
		ib12.enable_extensions(glfw_ext_count, glfw_exts);
		auto inst12 = ib12.build();
		if(!inst12) {
			log_fatal("Vulkan instance creation failed: %s", inst12.error().message().c_str());
			exit(1);
		}
		g.vkb_instance = inst12.value();
		g.use_dynamic_rendering = false;
		g.api_version = VK_API_VERSION_1_2;
		log_info("Vulkan instance API: 1.2 (render pass path)");
	}
	g.instance = g.vkb_instance.instance;

	VkResult surf_err = glfwCreateWindowSurface(g.instance, g.window, nullptr, &g.surface);
	if(surf_err != VK_SUCCESS) {
		log_fatal("glfwCreateWindowSurface failed (%d)", (int)surf_err);
		exit(1);
	}

	vkb::PhysicalDeviceSelector selector{g.vkb_instance};
	selector.set_surface(g.surface)
		.prefer_gpu_device_type(vkb::PreferredDeviceType::discrete)
		.require_present(true);

	auto phys_ret = selector.select();
	if(!phys_ret) {
		log_fatal("Physical device selection failed: %s", phys_ret.error().message().c_str());
		exit(1);
	}
	g.vkb_phys = phys_ret.value();
	log_info("Vulkan GPU: %s", g.vkb_phys.name.c_str());

	vkb::DeviceBuilder db{g.vkb_phys};
	VkPhysicalDeviceDynamicRenderingFeatures dyn_feat{
		VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES};
	if(g.use_dynamic_rendering) {
		dyn_feat.dynamicRendering = VK_TRUE;
		db.add_pNext(&dyn_feat);
	}

	auto dev_ret = db.build();
	if(!dev_ret) {
		log_fatal("Logical device creation failed: %s", dev_ret.error().message().c_str());
		exit(1);
	}
	g.vkb_device = dev_ret.value();
	g.device = g.vkb_device.device;

	auto q_ret = g.vkb_device.get_queue(vkb::QueueType::graphics);
	if(!q_ret) {
		log_fatal("Missing graphics queue");
		exit(1);
	}
	g.graphics_queue = q_ret.value();
	g.graphics_queue_family = g.vkb_device.get_queue_index(vkb::QueueType::graphics).value();

	VmaAllocatorCreateInfo aci{};
	aci.physicalDevice = g.vkb_phys.physical_device;
	aci.device = g.device;
	aci.instance = g.instance;
	aci.vulkanApiVersion = g.api_version;
	if(vmaCreateAllocator(&aci, &g.allocator) != VK_SUCCESS) {
		log_fatal("VMA allocator creation failed");
		exit(1);
	}

	if(!create_swapchain()) {
		log_fatal("Initial swapchain creation failed");
		exit(1);
	}
	if(!create_frames()) {
		log_fatal("Frame sync / command buffer creation failed");
		exit(1);
	}

	int w = 0, h = 0;
	glfwGetFramebufferSize(g.window, &w, &h);
	g.pending_w = w;
	g.pending_h = h;
}

extern "C" void gfx_vk_shutdown(void) {
	if(g.device)
		vkDeviceWaitIdle(g.device);

	destroy_frames();
	destroy_swapchain_objects();
	if(g.render_pass) {
		vkDestroyRenderPass(g.device, g.render_pass, nullptr);
		g.render_pass = VK_NULL_HANDLE;
	}
	if(g.allocator) {
		vmaDestroyAllocator(g.allocator);
		g.allocator = VK_NULL_HANDLE;
	}
	if(g.vkb_device.device)
		vkb::destroy_device(g.vkb_device);
	g.device = VK_NULL_HANDLE;
	if(g.surface) {
		vkDestroySurfaceKHR(g.instance, g.surface, nullptr);
		g.surface = VK_NULL_HANDLE;
	}
	if(g.vkb_instance.instance)
		vkb::destroy_instance(g.vkb_instance);
	g.instance = VK_NULL_HANDLE;
	g.window = nullptr;
}

extern "C" void gfx_vk_resize(int w, int h) {
	g.pending_w = w;
	g.pending_h = h;
	g.swapchain_dirty = true;
}

extern "C" void gfx_vk_swap_buffers(void) {
	if(!g.device || !g.swapchain)
		return;

	if(g.pending_w == 0 || g.pending_h == 0) {
		int fb_w = 0, fb_h = 0;
		glfwGetFramebufferSize(g.window, &fb_w, &fb_h);
		g.pending_w = fb_w;
		g.pending_h = fb_h;
		if(fb_w == 0 || fb_h == 0)
			return;
	}

	if(g.swapchain_dirty) {
		if(!recreate_swapchain())
			return;
		if(g.swapchain_extent.width == 0 || g.swapchain_extent.height == 0)
			return;
	}

	FrameSync& frame = g.frames[g.frame_index];
	vkWaitForFences(g.device, 1, &frame.in_flight, VK_TRUE, UINT64_MAX);

	uint32_t image_index = 0;
	VkResult acq = vkAcquireNextImageKHR(g.device, g.swapchain, UINT64_MAX, frame.image_available, VK_NULL_HANDLE,
										 &image_index);
	if(acq == VK_ERROR_OUT_OF_DATE_KHR) {
		g.swapchain_dirty = true;
		recreate_swapchain();
		return;
	}
	if(acq != VK_SUCCESS && acq != VK_SUBOPTIMAL_KHR) {
		log_error("vkAcquireNextImageKHR failed (%d)", (int)acq);
		return;
	}

	if(g.images_in_flight[image_index] != VK_NULL_HANDLE)
		vkWaitForFences(g.device, 1, &g.images_in_flight[image_index], VK_TRUE, UINT64_MAX);
	g.images_in_flight[image_index] = frame.in_flight;

	vkResetFences(g.device, 1, &frame.in_flight);
	vkResetCommandBuffer(frame.cmd, 0);
	record_clear(frame.cmd, image_index);

	VkSemaphore render_finished = g.render_finished_for_image[image_index];

	VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
	si.waitSemaphoreCount = 1;
	si.pWaitSemaphores = &frame.image_available;
	si.pWaitDstStageMask = &wait_stage;
	si.commandBufferCount = 1;
	si.pCommandBuffers = &frame.cmd;
	si.signalSemaphoreCount = 1;
	si.pSignalSemaphores = &render_finished;
	if(vkQueueSubmit(g.graphics_queue, 1, &si, frame.in_flight) != VK_SUCCESS) {
		log_error("vkQueueSubmit failed");
		return;
	}

	VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
	pi.waitSemaphoreCount = 1;
	pi.pWaitSemaphores = &render_finished;
	pi.swapchainCount = 1;
	pi.pSwapchains = &g.swapchain;
	pi.pImageIndices = &image_index;
	VkResult pr = vkQueuePresentKHR(g.graphics_queue, &pi);
	if(pr == VK_ERROR_OUT_OF_DATE_KHR || pr == VK_SUBOPTIMAL_KHR)
		g.swapchain_dirty = true;
	else if(pr != VK_SUCCESS)
		log_error("vkQueuePresentKHR failed (%d)", (int)pr);

	g.frame_index = (g.frame_index + 1) % kFramesInFlight;
}

extern "C" void gfx_vk_set_vsync(int interval) {
	/* Phase 2: FIFO only (vsync on). interval==0 would want mailbox/immediate later. */
	g.vsync = interval != 0;
	(void)g.vsync;
}

/* --- deliberate stubs (log once) --- */

extern "C" void gfx_vk_matrix_projection(const float* m16) {
	GFX_VK_STUB_ONCE("gfx_matrix_projection");
	(void)m16;
}
extern "C" void gfx_vk_matrix_modelview(const float* view16, const float* model16) {
	GFX_VK_STUB_ONCE("gfx_matrix_modelview");
	(void)view16;
	(void)model16;
}
extern "C" void gfx_vk_matrix_texture(float sx, float sy) {
	GFX_VK_STUB_ONCE("gfx_matrix_texture");
	(void)sx;
	(void)sy;
}
extern "C" void gfx_vk_pass_begin(gfx_pass_t pass) {
	GFX_VK_STUB_ONCE("gfx_pass_begin");
	(void)pass;
}
extern "C" void gfx_vk_pass_end(gfx_pass_t pass) {
	GFX_VK_STUB_ONCE("gfx_pass_end");
	(void)pass;
}
extern "C" gfx_texture_t gfx_vk_texture_create_rgba(int w, int h, const void* pixels) {
	GFX_VK_STUB_ONCE("gfx_texture_create_rgba");
	(void)w;
	(void)h;
	(void)pixels;
	return 0;
}
extern "C" void gfx_vk_texture_upload_rgba(gfx_texture_t tex, int w, int h, const void* pixels) {
	GFX_VK_STUB_ONCE("gfx_texture_upload_rgba");
	(void)tex;
	(void)w;
	(void)h;
	(void)pixels;
}
extern "C" gfx_texture_t gfx_vk_texture_create_alpha(int w, int h, const void* pixels) {
	GFX_VK_STUB_ONCE("gfx_texture_create_alpha");
	(void)w;
	(void)h;
	(void)pixels;
	return 0;
}
extern "C" void gfx_vk_texture_update_sub_rgba(gfx_texture_t tex, int x, int y, int w, int h, const void* pixels) {
	GFX_VK_STUB_ONCE("gfx_texture_update_sub_rgba");
	(void)tex;
	(void)x;
	(void)y;
	(void)w;
	(void)h;
	(void)pixels;
}
extern "C" void gfx_vk_texture_set_filter(gfx_texture_t tex, gfx_filter_t filter) {
	GFX_VK_STUB_ONCE("gfx_texture_set_filter");
	(void)tex;
	(void)filter;
}
extern "C" void gfx_vk_texture_set_filter_wrap_bound(gfx_filter_t filter, gfx_wrap_t wrap) {
	GFX_VK_STUB_ONCE("gfx_texture_set_filter_wrap_bound");
	(void)filter;
	(void)wrap;
}
extern "C" void gfx_vk_texture_bind(gfx_texture_t tex) {
	GFX_VK_STUB_ONCE("gfx_texture_bind");
	(void)tex;
}
extern "C" void gfx_vk_texture_destroy(gfx_texture_t tex) {
	GFX_VK_STUB_ONCE("gfx_texture_destroy");
	(void)tex;
}
extern "C" int gfx_vk_max_texture_size(void) {
	GFX_VK_STUB_ONCE("gfx_max_texture_size");
	return 0;
}
extern "C" int gfx_vk_supports_npot(void) {
	GFX_VK_STUB_ONCE("gfx_supports_npot");
	return 0;
}
extern "C" void gfx_vk_texture_2d(int enabled) {
	GFX_VK_STUB_ONCE("gfx_texture_2d");
	(void)enabled;
}
extern "C" void gfx_vk_blend(int enabled) {
	GFX_VK_STUB_ONCE("gfx_blend");
	(void)enabled;
}
extern "C" void gfx_vk_draw_quads_2d(const float* xy, const float* uv, int vertex_count) {
	GFX_VK_STUB_ONCE("gfx_draw_quads_2d");
	(void)xy;
	(void)uv;
	(void)vertex_count;
}
extern "C" void gfx_vk_draw_quads_2d_short(const short* xy, const short* uv, int vertex_count) {
	GFX_VK_STUB_ONCE("gfx_draw_quads_2d_short");
	(void)xy;
	(void)uv;
	(void)vertex_count;
}
extern "C" void gfx_vk_mesh_create(gfx_mesh_t* m, int has_color, int has_normal) {
	GFX_VK_STUB_ONCE("gfx_mesh_create");
	if(m) {
		m->legacy = 0;
		m->modern = 0;
		m->size = 0;
		m->buffer_size = 0;
		m->has_color = has_color;
		m->has_normal = has_normal;
	}
}
extern "C" void gfx_vk_mesh_destroy(gfx_mesh_t* m) {
	GFX_VK_STUB_ONCE("gfx_mesh_destroy");
	(void)m;
}
extern "C" void gfx_vk_mesh_update(gfx_mesh_t* m, size_t count, gfx_mesh_type_t type, const void* color,
								  const void* vertex, const void* normal) {
	GFX_VK_STUB_ONCE("gfx_mesh_update");
	(void)m;
	(void)count;
	(void)type;
	(void)color;
	(void)vertex;
	(void)normal;
}
extern "C" void gfx_vk_mesh_draw(gfx_mesh_t* m, gfx_mesh_type_t type) {
	GFX_VK_STUB_ONCE("gfx_mesh_draw");
	(void)m;
	(void)type;
}
extern "C" void gfx_vk_draw_arrays(gfx_mesh_type_t type, size_t count, const void* vertex, const void* color,
								  const void* normal) {
	GFX_VK_STUB_ONCE("gfx_draw_arrays");
	(void)type;
	(void)count;
	(void)vertex;
	(void)color;
	(void)normal;
}
extern "C" void gfx_vk_color_mask(int r, int g, int b, int a) {
	GFX_VK_STUB_ONCE("gfx_color_mask");
	(void)r;
	(void)g;
	(void)b;
	(void)a;
}
extern "C" void gfx_vk_color3f(float r, float g, float b) {
	GFX_VK_STUB_ONCE("gfx_color3f");
	(void)r;
	(void)g;
	(void)b;
}
extern "C" void gfx_vk_color3ub(unsigned char r, unsigned char g, unsigned char b) {
	GFX_VK_STUB_ONCE("gfx_color3ub");
	(void)r;
	(void)g;
	(void)b;
}
extern "C" void gfx_vk_color4f(float r, float g, float b, float a) {
	GFX_VK_STUB_ONCE("gfx_color4f");
	(void)r;
	(void)g;
	(void)b;
	(void)a;
}
extern "C" void gfx_vk_color4ub(unsigned char r, unsigned char g, unsigned char b, unsigned char a) {
	GFX_VK_STUB_ONCE("gfx_color4ub");
	(void)r;
	(void)g;
	(void)b;
	(void)a;
}
extern "C" void gfx_vk_get_color4f(float out[4]) {
	GFX_VK_STUB_ONCE("gfx_get_color4f");
	if(out) {
		out[0] = out[1] = out[2] = out[3] = 1.0f;
	}
}
extern "C" void gfx_vk_multisample(int enabled) {
	GFX_VK_STUB_ONCE("gfx_multisample");
	(void)enabled;
}
extern "C" void gfx_vk_line_width(float w) {
	GFX_VK_STUB_ONCE("gfx_line_width");
	(void)w;
}
extern "C" void gfx_vk_draw_lines_2f(const float* xy_pairs, int vertex_count) {
	GFX_VK_STUB_ONCE("gfx_draw_lines_2f");
	(void)xy_pairs;
	(void)vertex_count;
}
extern "C" void gfx_vk_draw_lines_3s(const short* xyz, int vertex_count) {
	GFX_VK_STUB_ONCE("gfx_draw_lines_3s");
	(void)xyz;
	(void)vertex_count;
}
extern "C" void gfx_vk_depth_range_weapon(void) {
	GFX_VK_STUB_ONCE("gfx_depth_range_weapon");
}
extern "C" void gfx_vk_depth_range_reset(void) {
	GFX_VK_STUB_ONCE("gfx_depth_range_reset");
}
extern "C" void gfx_vk_depth_test(int enabled) {
	GFX_VK_STUB_ONCE("gfx_depth_test");
	(void)enabled;
}
extern "C" void gfx_vk_depth_func_notequal(void) {
	GFX_VK_STUB_ONCE("gfx_depth_func_notequal");
}
extern "C" void gfx_vk_depth_func_lequal(void) {
	GFX_VK_STUB_ONCE("gfx_depth_func_lequal");
}
extern "C" void gfx_vk_scissor(int x, int y, int w, int h) {
	GFX_VK_STUB_ONCE("gfx_scissor");
	(void)x;
	(void)y;
	(void)w;
	(void)h;
}
extern "C" void gfx_vk_scissor_off(void) {
	GFX_VK_STUB_ONCE("gfx_scissor_off");
}
extern "C" void gfx_vk_viewport(int x, int y, int w, int h) {
	GFX_VK_STUB_ONCE("gfx_viewport");
	(void)x;
	(void)y;
	(void)w;
	(void)h;
}
extern "C" void gfx_vk_clear_color(float r, float g, float b, float a) {
	GFX_VK_STUB_ONCE("gfx_clear_color");
	(void)r;
	(void)g;
	(void)b;
	(void)a;
}
extern "C" void gfx_vk_clear(void) {
	GFX_VK_STUB_ONCE("gfx_clear");
}
extern "C" void gfx_vk_clear_color_only(void) {
	GFX_VK_STUB_ONCE("gfx_clear_color_only");
}
extern "C" void gfx_vk_shade_smooth(void) {
	GFX_VK_STUB_ONCE("gfx_shade_smooth");
}
extern "C" void gfx_vk_shade_flat(void) {
	GFX_VK_STUB_ONCE("gfx_shade_flat");
}
extern "C" void gfx_vk_light0_position(const float pos4[4]) {
	GFX_VK_STUB_ONCE("gfx_light0_position");
	(void)pos4;
}
extern "C" void gfx_vk_capture_framebuffer(int x, int y, int w, int h, void* out_rgba) {
	GFX_VK_STUB_ONCE("gfx_capture_framebuffer");
	(void)x;
	(void)y;
	(void)w;
	(void)h;
	(void)out_rgba;
}
extern "C" int gfx_vk_gl2(void) {
	GFX_VK_STUB_ONCE("gfx_gl2");
	return 0;
}
extern "C" void gfx_vk_model_light(const float ambient4[4], const float diffuse4[4]) {
	GFX_VK_STUB_ONCE("gfx_model_light");
	(void)ambient4;
	(void)diffuse4;
}
extern "C" void gfx_vk_model_mesh_begin(gfx_texture_t dummy) {
	GFX_VK_STUB_ONCE("gfx_model_mesh_begin");
	(void)dummy;
}
extern "C" void gfx_vk_model_texenv_color(float r, float g, float b) {
	GFX_VK_STUB_ONCE("gfx_model_texenv_color");
	(void)r;
	(void)g;
	(void)b;
}
extern "C" void gfx_vk_model_mesh_end(void) {
	GFX_VK_STUB_ONCE("gfx_model_mesh_end");
}
extern "C" void gfx_vk_model_points_begin_fixed(float point_size) {
	GFX_VK_STUB_ONCE("gfx_model_points_begin_fixed");
	(void)point_size;
}
extern "C" void gfx_vk_model_points_end_fixed(void) {
	GFX_VK_STUB_ONCE("gfx_model_points_end_fixed");
}
extern "C" void gfx_vk_model_points_begin_shader(float point_size, float dist_factor, const float fog_rgb[3],
												const float camera[3], const float model16[16]) {
	GFX_VK_STUB_ONCE("gfx_model_points_begin_shader");
	(void)point_size;
	(void)dist_factor;
	(void)fog_rgb;
	(void)camera;
	(void)model16;
}
extern "C" void gfx_vk_model_points_end_shader(void) {
	GFX_VK_STUB_ONCE("gfx_model_points_end_shader");
}
extern "C" void gfx_vk_fog_enable_exp2(const float color4[4], float density) {
	GFX_VK_STUB_ONCE("gfx_fog_enable_exp2");
	(void)color4;
	(void)density;
}
extern "C" void gfx_vk_fog_disable(void) {
	GFX_VK_STUB_ONCE("gfx_fog_disable");
}
extern "C" void gfx_vk_fog_enable_spherical(void) {
	GFX_VK_STUB_ONCE("gfx_fog_enable_spherical");
}
extern "C" void gfx_vk_fog_disable_spherical(void) {
	GFX_VK_STUB_ONCE("gfx_fog_disable_spherical");
}
extern "C" int gfx_vk_fog_active(void) {
	GFX_VK_STUB_ONCE("gfx_fog_active");
	return 0;
}

extern "C" const gfx_ops_t gfx_vk_ops = {
	.apply_context_hints = gfx_vk_apply_context_hints,
	.init = gfx_vk_init,
	.shutdown = gfx_vk_shutdown,
	.resize = gfx_vk_resize,
	.swap_buffers = gfx_vk_swap_buffers,
	.set_vsync = gfx_vk_set_vsync,
	.matrix_projection = gfx_vk_matrix_projection,
	.matrix_modelview = gfx_vk_matrix_modelview,
	.matrix_texture = gfx_vk_matrix_texture,
	.pass_begin = gfx_vk_pass_begin,
	.pass_end = gfx_vk_pass_end,
	.texture_create_rgba = gfx_vk_texture_create_rgba,
	.texture_upload_rgba = gfx_vk_texture_upload_rgba,
	.texture_create_alpha = gfx_vk_texture_create_alpha,
	.texture_update_sub_rgba = gfx_vk_texture_update_sub_rgba,
	.texture_set_filter = gfx_vk_texture_set_filter,
	.texture_set_filter_wrap_bound = gfx_vk_texture_set_filter_wrap_bound,
	.texture_bind = gfx_vk_texture_bind,
	.texture_destroy = gfx_vk_texture_destroy,
	.max_texture_size = gfx_vk_max_texture_size,
	.supports_npot = gfx_vk_supports_npot,
	.texture_2d = gfx_vk_texture_2d,
	.blend = gfx_vk_blend,
	.draw_quads_2d = gfx_vk_draw_quads_2d,
	.draw_quads_2d_short = gfx_vk_draw_quads_2d_short,
	.mesh_create = gfx_vk_mesh_create,
	.mesh_destroy = gfx_vk_mesh_destroy,
	.mesh_update = gfx_vk_mesh_update,
	.mesh_draw = gfx_vk_mesh_draw,
	.draw_arrays = gfx_vk_draw_arrays,
	.color_mask = gfx_vk_color_mask,
	.color3f = gfx_vk_color3f,
	.color3ub = gfx_vk_color3ub,
	.color4f = gfx_vk_color4f,
	.color4ub = gfx_vk_color4ub,
	.get_color4f = gfx_vk_get_color4f,
	.multisample = gfx_vk_multisample,
	.line_width = gfx_vk_line_width,
	.draw_lines_2f = gfx_vk_draw_lines_2f,
	.draw_lines_3s = gfx_vk_draw_lines_3s,
	.depth_range_weapon = gfx_vk_depth_range_weapon,
	.depth_range_reset = gfx_vk_depth_range_reset,
	.depth_test = gfx_vk_depth_test,
	.depth_func_notequal = gfx_vk_depth_func_notequal,
	.depth_func_lequal = gfx_vk_depth_func_lequal,
	.scissor = gfx_vk_scissor,
	.scissor_off = gfx_vk_scissor_off,
	.viewport = gfx_vk_viewport,
	.clear_color = gfx_vk_clear_color,
	.clear = gfx_vk_clear,
	.clear_color_only = gfx_vk_clear_color_only,
	.shade_smooth = gfx_vk_shade_smooth,
	.shade_flat = gfx_vk_shade_flat,
	.light0_position = gfx_vk_light0_position,
	.capture_framebuffer = gfx_vk_capture_framebuffer,
	.gl2 = gfx_vk_gl2,
	.model_light = gfx_vk_model_light,
	.model_mesh_begin = gfx_vk_model_mesh_begin,
	.model_texenv_color = gfx_vk_model_texenv_color,
	.model_mesh_end = gfx_vk_model_mesh_end,
	.model_points_begin_fixed = gfx_vk_model_points_begin_fixed,
	.model_points_end_fixed = gfx_vk_model_points_end_fixed,
	.model_points_begin_shader = gfx_vk_model_points_begin_shader,
	.model_points_end_shader = gfx_vk_model_points_end_shader,
	.fog_enable_exp2 = gfx_vk_fog_enable_exp2,
	.fog_disable = gfx_vk_fog_disable,
	.fog_enable_spherical = gfx_vk_fog_enable_spherical,
	.fog_disable_spherical = gfx_vk_fog_disable_spherical,
	.fog_active = gfx_vk_fog_active,
};
