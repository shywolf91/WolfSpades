/*
	Phase 3 Vulkan backend: 2D/UI pipeline (menu, settings, server list).
	World/3D paths remain log-once stubs. GL backend is untouched.

	Coordinate convention (clip-space Y / NDC depth):
	  OpenGL and Vulkan differ in clip-space Y (GL +Y up, VK +Y down in NDC)
	  and depth range. For this 2D phase we use ONLY the negative-viewport-height
	  trick (VK_KHR_maintenance1 semantics, core since Vulkan 1.1):
	    viewport.y = (float)fb_height;
	    viewport.height = -(float)fb_height;
	  so the SAME ortho matrices the game uploads produce the SAME on-screen
	  orientation as GL. Do not also flip in the shader.
	  Scissor is still in framebuffer coordinates (origin top-left). Callers pass
	  GL bottom-left scissors; we convert: y_vk = fb_h - y_gl - h.
*/

#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <functional>
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
#include "shaders_embedded.h"
}

namespace {

constexpr uint32_t kFramesInFlight = 2;
constexpr size_t kInitialVbBytes = 4u * 1024u * 1024u;
constexpr uint32_t kDescSetsPerPool = 64;

struct UIVertex {
	float x, y;
	float u, v;
	float r, g, b, a;
};

struct FrameSync {
	VkCommandPool pool = VK_NULL_HANDLE;
	VkCommandBuffer cmd = VK_NULL_HANDLE;
	VkSemaphore image_available = VK_NULL_HANDLE;
	VkFence in_flight = VK_NULL_HANDLE;

	VkBuffer vb = VK_NULL_HANDLE;
	VmaAllocation vb_alloc = VK_NULL_HANDLE;
	void* vb_mapped = nullptr;
	size_t vb_capacity = 0;
	size_t vb_used = 0;
	bool draws_recorded = false;
};

struct TextureSlot {
	bool alive = false;
	VkImage image = VK_NULL_HANDLE;
	VmaAllocation alloc = VK_NULL_HANDLE;
	VkImageView view = VK_NULL_HANDLE;
	VkDescriptorSet dset = VK_NULL_HANDLE;
	int w = 0;
	int h = 0;
	gfx_filter_t filter = GFX_FILTER_NEAREST;
	gfx_wrap_t wrap = GFX_WRAP_REPEAT;
	gfx_filter_t dset_filter = GFX_FILTER_NEAREST;
	gfx_wrap_t dset_wrap = GFX_WRAP_REPEAT;
	bool dset_dirty = false;
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
	VkPhysicalDeviceProperties phys_props{};

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
	std::vector<VkSemaphore> render_finished_for_image;
	uint32_t frame_index = 0;

	int pending_w = 0;
	int pending_h = 0;
	bool swapchain_dirty = false;
	bool vsync = true;
	uint32_t api_version = VK_API_VERSION_1_2;

	/* UI resources */
	VkShaderModule vert_mod = VK_NULL_HANDLE;
	VkShaderModule frag_mod = VK_NULL_HANDLE;
	VkDescriptorSetLayout dset_layout = VK_NULL_HANDLE;
	VkPipelineLayout pipe_layout = VK_NULL_HANDLE;
	VkPipeline pipe_quads_blend = VK_NULL_HANDLE;
	VkPipeline pipe_quads_opaque = VK_NULL_HANDLE;
	VkPipeline pipe_lines_blend = VK_NULL_HANDLE;
	VkPipeline pipe_lines_opaque = VK_NULL_HANDLE;
	VkSampler samplers[4]{}; /* nearest/linear × repeat/clamp */

	VkDescriptorPool desc_pool = VK_NULL_HANDLE;
	std::vector<VkDescriptorPool> desc_pools;
	std::vector<VkDescriptorSet> free_dsets;

	TextureSlot white{};
	std::vector<TextureSlot> textures;
	std::vector<uint32_t> free_tex_ids; /* 1-based handles */

	VkCommandPool upload_pool = VK_NULL_HANDLE;
	VkFence upload_fence = VK_NULL_HANDLE;

	/* Frame recording state */
	bool frame_begun = false;
	uint32_t acquired_image = 0;
	bool rendering_active = false;

	float clear_color[4] = {0.f, 0.f, 0.f, 1.f};
	float proj[16]{};
	float modelview[16]{};
	float mvp[16]{};
	bool mvp_dirty = true;
	float tex_sx = 1.f;
	float tex_sy = 1.f;
	float color[4] = {1.f, 1.f, 1.f, 1.f};

	gfx_texture_t bound_tex = 0;
	bool texture_2d = true;
	bool blend = true;

	int vp_x = 0, vp_y = 0, vp_w = 0, vp_h = 0;
	bool scissor_on = false;
	int sci_x = 0, sci_y = 0, sci_w = 0, sci_h = 0; /* GL bottom-left */

	/* Current open batch (verts already in VB, not yet drawn) */
	bool batch_open = false;
	bool batch_lines = false;
	bool batch_blend = true;
	gfx_texture_t batch_tex = 0;
	bool batch_scissor_on = false;
	int batch_sci_x = 0, batch_sci_y = 0, batch_sci_w = 0, batch_sci_h = 0;
	float batch_mvp[16]{};
	uint32_t batch_first = 0;
	uint32_t batch_count = 0;

	bool logged_vb_grow = false;
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

void mat4_identity(float* m) {
	std::memset(m, 0, 16 * sizeof(float));
	m[0] = m[5] = m[10] = m[15] = 1.f;
}

void mat4_mul(float* out, const float* a, const float* b) {
	float r[16];
	for(int c = 0; c < 4; c++) {
		for(int row = 0; row < 4; row++) {
			r[c * 4 + row] = a[0 * 4 + row] * b[c * 4 + 0] + a[1 * 4 + row] * b[c * 4 + 1] +
							 a[2 * 4 + row] * b[c * 4 + 2] + a[3 * 4 + row] * b[c * 4 + 3];
		}
	}
	std::memcpy(out, r, sizeof(r));
}

void rebuild_mvp() {
	mat4_mul(g.mvp, g.proj, g.modelview);
	g.mvp_dirty = false;
}

int sampler_index(gfx_filter_t filter, gfx_wrap_t wrap) {
	int f = (filter == GFX_FILTER_LINEAR) ? 1 : 0;
	int w = (wrap == GFX_WRAP_CLAMP) ? 1 : 0;
	return f * 2 + w;
}

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

	VkSubpassDescription sub{};
	sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	sub.colorAttachmentCount = 1;
	sub.pColorAttachments = &color_ref;

	VkSubpassDependency dep{};
	dep.srcSubpass = VK_SUBPASS_EXTERNAL;
	dep.dstSubpass = 0;
	dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

	VkRenderPassCreateInfo rpci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
	rpci.attachmentCount = 1;
	rpci.pAttachments = &color;
	rpci.subpassCount = 1;
	rpci.pSubpasses = &sub;
	rpci.dependencyCount = 1;
	rpci.pDependencies = &dep;
	return vkCreateRenderPass(g.device, &rpci, nullptr, &g.render_pass) == VK_SUCCESS;
}

bool create_framebuffers_12() {
	destroy_framebuffers();
	g.framebuffers.resize(g.swapchain_views.size());
	for(size_t i = 0; i < g.swapchain_views.size(); i++) {
		VkFramebufferCreateInfo fci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
		fci.renderPass = g.render_pass;
		fci.attachmentCount = 1;
		fci.pAttachments = &g.swapchain_views[i];
		fci.width = g.swapchain_extent.width;
		fci.height = g.swapchain_extent.height;
		fci.layers = 1;
		if(vkCreateFramebuffer(g.device, &fci, nullptr, &g.framebuffers[i]) != VK_SUCCESS)
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

bool create_swapchain() {
	vkb::SwapchainBuilder builder{g.vkb_device, g.surface};
	builder.set_desired_present_mode(VK_PRESENT_MODE_FIFO_KHR);
	/* Phase 3: UNORM to match GL non-sRGB default framebuffer + UNORM textures. */
	builder.set_desired_format({VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR});
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
		if(g.render_pass) {
			vkDestroyRenderPass(g.device, g.render_pass, nullptr);
			g.render_pass = VK_NULL_HANDLE;
		}
		if(!create_render_pass_12())
			return false;
		if(!create_framebuffers_12())
			return false;
	}

	const char* fmt_name = "unknown";
	switch(g.swapchain_format) {
		case VK_FORMAT_B8G8R8A8_UNORM: fmt_name = "B8G8R8A8_UNORM"; break;
		case VK_FORMAT_R8G8B8A8_UNORM: fmt_name = "R8G8B8A8_UNORM"; break;
		case VK_FORMAT_B8G8R8A8_SRGB: fmt_name = "B8G8R8A8_SRGB"; break;
		case VK_FORMAT_R8G8B8A8_SRGB: fmt_name = "R8G8B8A8_SRGB"; break;
		default: break;
	}
	log_info("Vulkan swapchain: %ux%u format=%s present=FIFO", g.swapchain_extent.width, g.swapchain_extent.height,
			 fmt_name);
	g.swapchain_dirty = false;
	g.vp_w = (int)g.swapchain_extent.width;
	g.vp_h = (int)g.swapchain_extent.height;
	return true;
}

bool recreate_swapchain();

void destroy_frame_vb(FrameSync& f) {
	if(f.vb) {
		vmaDestroyBuffer(g.allocator, f.vb, f.vb_alloc);
		f.vb = VK_NULL_HANDLE;
		f.vb_alloc = VK_NULL_HANDLE;
		f.vb_mapped = nullptr;
		f.vb_capacity = 0;
		f.vb_used = 0;
	}
}

bool create_frame_vb(FrameSync& f, size_t bytes) {
	destroy_frame_vb(f);
	VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
	bci.size = bytes;
	bci.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
	VmaAllocationCreateInfo aci{};
	aci.usage = VMA_MEMORY_USAGE_AUTO;
	aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
	VmaAllocationInfo ainfo{};
	if(vmaCreateBuffer(g.allocator, &bci, &aci, &f.vb, &f.vb_alloc, &ainfo) != VK_SUCCESS)
		return false;
	f.vb_mapped = ainfo.pMappedData;
	f.vb_capacity = bytes;
	f.vb_used = 0;
	return true;
}

void destroy_frames() {
	for(auto& f : g.frames) {
		destroy_frame_vb(f);
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
		if(!create_frame_vb(f, kInitialVbBytes))
			return false;
	}
	return true;
}

bool grow_frame_vb(FrameSync& f, size_t need) {
	size_t new_cap = f.vb_capacity ? f.vb_capacity * 2 : kInitialVbBytes;
	while(new_cap < need)
		new_cap *= 2;
	if(!g.logged_vb_grow) {
		log_warn("gfx_vk: growing UI vertex buffer to %zu bytes", new_cap);
		g.logged_vb_grow = true;
	}
	/* Must not be in-flight; grow only when fence waited (frame begin). */
	std::vector<uint8_t> saved(f.vb_used);
	if(f.vb_used && f.vb_mapped)
		std::memcpy(saved.data(), f.vb_mapped, f.vb_used);
	size_t used = f.vb_used;
	if(!create_frame_vb(f, new_cap))
		return false;
	if(used && f.vb_mapped)
		std::memcpy(f.vb_mapped, saved.data(), used);
	f.vb_used = used;
	return true;
}

VkShaderModule create_shader_module(const unsigned char* code, size_t len) {
	VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
	ci.codeSize = len;
	ci.pCode = reinterpret_cast<const uint32_t*>(code);
	VkShaderModule mod = VK_NULL_HANDLE;
	if(vkCreateShaderModule(g.device, &ci, nullptr, &mod) != VK_SUCCESS)
		return VK_NULL_HANDLE;
	return mod;
}

bool ensure_desc_pool() {
	if(!g.free_dsets.empty())
		return true;
	VkDescriptorPoolSize ps{};
	ps.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	ps.descriptorCount = kDescSetsPerPool;
	VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
	pci.maxSets = kDescSetsPerPool;
	pci.poolSizeCount = 1;
	pci.pPoolSizes = &ps;
	pci.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
	VkDescriptorPool pool = VK_NULL_HANDLE;
	if(vkCreateDescriptorPool(g.device, &pci, nullptr, &pool) != VK_SUCCESS)
		return false;
	g.desc_pools.push_back(pool);

	std::vector<VkDescriptorSetLayout> layouts(kDescSetsPerPool, g.dset_layout);
	VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
	ai.descriptorPool = pool;
	ai.descriptorSetCount = kDescSetsPerPool;
	ai.pSetLayouts = layouts.data();
	std::vector<VkDescriptorSet> sets(kDescSetsPerPool);
	if(vkAllocateDescriptorSets(g.device, &ai, sets.data()) != VK_SUCCESS)
		return false;
	for(VkDescriptorSet s : sets)
		g.free_dsets.push_back(s);
	return true;
}

VkDescriptorSet alloc_dset() {
	if(!ensure_desc_pool())
		return VK_NULL_HANDLE;
	VkDescriptorSet s = g.free_dsets.back();
	g.free_dsets.pop_back();
	return s;
}

void free_dset(VkDescriptorSet s) {
	if(s)
		g.free_dsets.push_back(s);
}

void write_texture_dset(TextureSlot& t) {
	VkDescriptorImageInfo ii{};
	ii.sampler = g.samplers[sampler_index(t.filter, t.wrap)];
	ii.imageView = t.view;
	ii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
	w.dstSet = t.dset;
	w.dstBinding = 0;
	w.descriptorCount = 1;
	w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	w.pImageInfo = &ii;
	vkUpdateDescriptorSets(g.device, 1, &w, 0, nullptr);
	t.dset_filter = t.filter;
	t.dset_wrap = t.wrap;
}

/* Descriptor updates are illegal while the set is referenced by an in-flight /
 * recording command buffer (without UPDATE_AFTER_BIND). Defer until frame begin
 * after the frame fence has been waited. */
void request_dset_rewrite(TextureSlot& t) {
	if(t.filter == t.dset_filter && t.wrap == t.dset_wrap)
		return;
	t.dset_dirty = true;
}

void flush_pending_dset_rewrites() {
	if(g.white.alive && g.white.dset_dirty) {
		write_texture_dset(g.white);
		g.white.dset_dirty = false;
	}
	for(auto& t : g.textures) {
		if(t.alive && t.dset_dirty) {
			write_texture_dset(t);
			t.dset_dirty = false;
		}
	}
}

bool immediate_submit(const std::function<void(VkCommandBuffer)>& record) {
	VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
	cai.commandPool = g.upload_pool;
	cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	cai.commandBufferCount = 1;
	VkCommandBuffer cmd = VK_NULL_HANDLE;
	if(vkAllocateCommandBuffers(g.device, &cai, &cmd) != VK_SUCCESS)
		return false;

	VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
	bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkBeginCommandBuffer(cmd, &bi);
	record(cmd);
	vkEndCommandBuffer(cmd);

	vkResetFences(g.device, 1, &g.upload_fence);
	VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
	si.commandBufferCount = 1;
	si.pCommandBuffers = &cmd;
	if(vkQueueSubmit(g.graphics_queue, 1, &si, g.upload_fence) != VK_SUCCESS) {
		vkFreeCommandBuffers(g.device, g.upload_pool, 1, &cmd);
		return false;
	}
	vkWaitForFences(g.device, 1, &g.upload_fence, VK_TRUE, UINT64_MAX);
	vkFreeCommandBuffers(g.device, g.upload_pool, 1, &cmd);
	return true;
}

void destroy_texture_slot(TextureSlot& t) {
	if(!t.alive && !t.image)
		return;
	if(t.view)
		vkDestroyImageView(g.device, t.view, nullptr);
	if(t.image)
		vmaDestroyImage(g.allocator, t.image, t.alloc);
	free_dset(t.dset);
	t = {};
}

bool create_rgba_image(TextureSlot& t, int w, int h, const void* pixels, gfx_filter_t filter, gfx_wrap_t wrap) {
	destroy_texture_slot(t);
	t.w = w;
	t.h = h;
	t.filter = filter;
	t.wrap = wrap;

	VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
	ici.imageType = VK_IMAGE_TYPE_2D;
	ici.format = VK_FORMAT_R8G8B8A8_UNORM;
	ici.extent = {(uint32_t)w, (uint32_t)h, 1};
	ici.mipLevels = 1;
	ici.arrayLayers = 1;
	ici.samples = VK_SAMPLE_COUNT_1_BIT;
	ici.tiling = VK_IMAGE_TILING_OPTIMAL;
	ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
	ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

	VmaAllocationCreateInfo aci{};
	aci.usage = VMA_MEMORY_USAGE_AUTO;
	aci.preferredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
	if(vmaCreateImage(g.allocator, &ici, &aci, &t.image, &t.alloc, nullptr) != VK_SUCCESS)
		return false;

	VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
	vci.image = t.image;
	vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
	vci.format = VK_FORMAT_R8G8B8A8_UNORM;
	vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	vci.subresourceRange.levelCount = 1;
	vci.subresourceRange.layerCount = 1;
	if(vkCreateImageView(g.device, &vci, nullptr, &t.view) != VK_SUCCESS)
		return false;

	t.dset = alloc_dset();
	if(!t.dset)
		return false;
	write_texture_dset(t);
	t.alive = true;

	const size_t nbytes = (size_t)w * (size_t)h * 4;
	VkBuffer staging = VK_NULL_HANDLE;
	VmaAllocation staging_alloc = VK_NULL_HANDLE;
	VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
	bci.size = nbytes ? nbytes : 4;
	bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
	VmaAllocationCreateInfo baci{};
	baci.usage = VMA_MEMORY_USAGE_AUTO;
	baci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
	VmaAllocationInfo bainfo{};
	if(vmaCreateBuffer(g.allocator, &bci, &baci, &staging, &staging_alloc, &bainfo) != VK_SUCCESS)
		return false;
	if(pixels && nbytes)
		std::memcpy(bainfo.pMappedData, pixels, nbytes);
	else if(bainfo.pMappedData)
		std::memset(bainfo.pMappedData, 0xff, bci.size);

	bool ok = immediate_submit([&](VkCommandBuffer cmd) {
		VkImageMemoryBarrier to_dst{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
		to_dst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		to_dst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		to_dst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		to_dst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		to_dst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		to_dst.image = t.image;
		to_dst.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		to_dst.subresourceRange.levelCount = 1;
		to_dst.subresourceRange.layerCount = 1;
		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
							 nullptr, 1, &to_dst);

		VkBufferImageCopy region{};
		region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		region.imageSubresource.layerCount = 1;
		region.imageExtent = {(uint32_t)w, (uint32_t)h, 1};
		vkCmdCopyBufferToImage(cmd, staging, t.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

		VkImageMemoryBarrier to_samp = to_dst;
		to_samp.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		to_samp.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
		to_samp.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		to_samp.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr,
							 0, nullptr, 1, &to_samp);
	});

	vmaDestroyBuffer(g.allocator, staging, staging_alloc);
	return ok;
}

bool update_texture_sub(TextureSlot& t, int x, int y, int w, int h, const void* pixels) {
	if(!t.alive || !pixels || w <= 0 || h <= 0)
		return false;
	const size_t nbytes = (size_t)w * (size_t)h * 4;
	VkBuffer staging = VK_NULL_HANDLE;
	VmaAllocation staging_alloc = VK_NULL_HANDLE;
	VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
	bci.size = nbytes;
	bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
	VmaAllocationCreateInfo baci{};
	baci.usage = VMA_MEMORY_USAGE_AUTO;
	baci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
	VmaAllocationInfo bainfo{};
	if(vmaCreateBuffer(g.allocator, &bci, &baci, &staging, &staging_alloc, &bainfo) != VK_SUCCESS)
		return false;
	std::memcpy(bainfo.pMappedData, pixels, nbytes);

	bool ok = immediate_submit([&](VkCommandBuffer cmd) {
		VkImageMemoryBarrier to_dst{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
		to_dst.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
		to_dst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		to_dst.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		to_dst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		to_dst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		to_dst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		to_dst.image = t.image;
		to_dst.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		to_dst.subresourceRange.levelCount = 1;
		to_dst.subresourceRange.layerCount = 1;
		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr,
							 0, nullptr, 1, &to_dst);

		VkBufferImageCopy region{};
		region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		region.imageSubresource.layerCount = 1;
		region.imageOffset = {x, y, 0};
		region.imageExtent = {(uint32_t)w, (uint32_t)h, 1};
		vkCmdCopyBufferToImage(cmd, staging, t.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

		VkImageMemoryBarrier to_samp = to_dst;
		to_samp.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		to_samp.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
		to_samp.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		to_samp.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr,
							 0, nullptr, 1, &to_samp);
	});
	vmaDestroyBuffer(g.allocator, staging, staging_alloc);
	return ok;
}

TextureSlot* tex_from_handle(gfx_texture_t h) {
	if(h == 0 || h > g.textures.size())
		return nullptr;
	TextureSlot& t = g.textures[h - 1];
	return t.alive ? &t : nullptr;
}

gfx_texture_t alloc_tex_handle() {
	if(!g.free_tex_ids.empty()) {
		uint32_t id = g.free_tex_ids.back();
		g.free_tex_ids.pop_back();
		return id;
	}
	g.textures.emplace_back();
	return (gfx_texture_t)g.textures.size();
}

void destroy_ui_pipelines() {
	auto dpipe = [](VkPipeline& p) {
		if(p) {
			vkDestroyPipeline(g.device, p, nullptr);
			p = VK_NULL_HANDLE;
		}
	};
	dpipe(g.pipe_quads_blend);
	dpipe(g.pipe_quads_opaque);
	dpipe(g.pipe_lines_blend);
	dpipe(g.pipe_lines_opaque);
}

VkPipeline create_ui_pipeline(VkPrimitiveTopology topo, bool blend_on) {
	VkPipelineShaderStageCreateInfo stages[2]{};
	stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	stages[0].module = g.vert_mod;
	stages[0].pName = "main";
	stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	stages[1].module = g.frag_mod;
	stages[1].pName = "main";

	VkVertexInputBindingDescription bind{};
	bind.binding = 0;
	bind.stride = sizeof(UIVertex);
	bind.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

	VkVertexInputAttributeDescription attrs[3]{};
	attrs[0] = {0, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(UIVertex, x)};
	attrs[1] = {1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(UIVertex, u)};
	attrs[2] = {2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(UIVertex, r)};

	VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
	vi.vertexBindingDescriptionCount = 1;
	vi.pVertexBindingDescriptions = &bind;
	vi.vertexAttributeDescriptionCount = 3;
	vi.pVertexAttributeDescriptions = attrs;

	VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
	ia.topology = topo;

	VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
	vp.viewportCount = 1;
	vp.scissorCount = 1;

	VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
	rs.polygonMode = VK_POLYGON_MODE_FILL;
	rs.cullMode = VK_CULL_MODE_NONE;
	rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
	rs.lineWidth = 1.0f;

	VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
	ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

	VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};

	VkPipelineColorBlendAttachmentState ba{};
	ba.colorWriteMask =
		VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
	if(blend_on) {
		ba.blendEnable = VK_TRUE;
		ba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
		ba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
		ba.colorBlendOp = VK_BLEND_OP_ADD;
		ba.srcAlphaBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
		ba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
		ba.alphaBlendOp = VK_BLEND_OP_ADD;
	}

	VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
	cb.attachmentCount = 1;
	cb.pAttachments = &ba;

	VkDynamicState dyn_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
	VkPipelineDynamicStateCreateInfo dyn{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
	dyn.dynamicStateCount = 2;
	dyn.pDynamicStates = dyn_states;

	VkGraphicsPipelineCreateInfo pci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
	pci.stageCount = 2;
	pci.pStages = stages;
	pci.pVertexInputState = &vi;
	pci.pInputAssemblyState = &ia;
	pci.pViewportState = &vp;
	pci.pRasterizationState = &rs;
	pci.pMultisampleState = &ms;
	pci.pDepthStencilState = &ds;
	pci.pColorBlendState = &cb;
	pci.pDynamicState = &dyn;
	pci.layout = g.pipe_layout;

	VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
	if(g.use_dynamic_rendering) {
		ri.colorAttachmentCount = 1;
		ri.pColorAttachmentFormats = &g.swapchain_format;
		pci.pNext = &ri;
	} else {
		pci.renderPass = g.render_pass;
		pci.subpass = 0;
	}

	VkPipeline pipe = VK_NULL_HANDLE;
	if(vkCreateGraphicsPipelines(g.device, VK_NULL_HANDLE, 1, &pci, nullptr, &pipe) != VK_SUCCESS)
		return VK_NULL_HANDLE;
	return pipe;
}

bool create_ui_pipelines() {
	destroy_ui_pipelines();
	g.pipe_quads_blend = create_ui_pipeline(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, true);
	g.pipe_quads_opaque = create_ui_pipeline(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, false);
	g.pipe_lines_blend = create_ui_pipeline(VK_PRIMITIVE_TOPOLOGY_LINE_LIST, true);
	g.pipe_lines_opaque = create_ui_pipeline(VK_PRIMITIVE_TOPOLOGY_LINE_LIST, false);
	return g.pipe_quads_blend && g.pipe_quads_opaque && g.pipe_lines_blend && g.pipe_lines_opaque;
}

bool create_ui_resources() {
	mat4_identity(g.proj);
	mat4_identity(g.modelview);
	mat4_identity(g.mvp);

	g.vert_mod = create_shader_module(ui_vert_spv, ui_vert_spv_len);
	g.frag_mod = create_shader_module(ui_frag_spv, ui_frag_spv_len);
	if(!g.vert_mod || !g.frag_mod) {
		log_error("Failed to create UI shader modules");
		return false;
	}

	VkDescriptorSetLayoutBinding binding{};
	binding.binding = 0;
	binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	binding.descriptorCount = 1;
	binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	VkDescriptorSetLayoutCreateInfo dlci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
	dlci.bindingCount = 1;
	dlci.pBindings = &binding;
	if(vkCreateDescriptorSetLayout(g.device, &dlci, nullptr, &g.dset_layout) != VK_SUCCESS)
		return false;

	VkPushConstantRange pcr{};
	pcr.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
	pcr.offset = 0;
	pcr.size = 64;

	VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
	plci.setLayoutCount = 1;
	plci.pSetLayouts = &g.dset_layout;
	plci.pushConstantRangeCount = 1;
	plci.pPushConstantRanges = &pcr;
	if(vkCreatePipelineLayout(g.device, &plci, nullptr, &g.pipe_layout) != VK_SUCCESS)
		return false;

	for(int i = 0; i < 4; i++) {
		bool linear = (i / 2) == 1;
		bool clamp = (i % 2) == 1;
		VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
		sci.magFilter = linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
		sci.minFilter = linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
		sci.addressModeU = clamp ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE : VK_SAMPLER_ADDRESS_MODE_REPEAT;
		sci.addressModeV = sci.addressModeU;
		sci.addressModeW = sci.addressModeU;
		sci.maxLod = 0.25f;
		if(vkCreateSampler(g.device, &sci, nullptr, &g.samplers[i]) != VK_SUCCESS)
			return false;
	}

	VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
	pci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
	pci.queueFamilyIndex = g.graphics_queue_family;
	if(vkCreateCommandPool(g.device, &pci, nullptr, &g.upload_pool) != VK_SUCCESS)
		return false;
	VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
	fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
	if(vkCreateFence(g.device, &fci, nullptr, &g.upload_fence) != VK_SUCCESS)
		return false;

	if(!create_ui_pipelines()) {
		log_error("Failed to create UI pipelines");
		return false;
	}

	uint8_t white_px[4] = {255, 255, 255, 255};
	if(!create_rgba_image(g.white, 1, 1, white_px, GFX_FILTER_NEAREST, GFX_WRAP_CLAMP)) {
		log_error("Failed to create white texture");
		return false;
	}
	return true;
}

void destroy_ui_resources() {
	destroy_ui_pipelines();
	destroy_texture_slot(g.white);
	for(auto& t : g.textures)
		destroy_texture_slot(t);
	g.textures.clear();
	g.free_tex_ids.clear();
	g.free_dsets.clear();
	for(VkDescriptorPool p : g.desc_pools) {
		if(p)
			vkDestroyDescriptorPool(g.device, p, nullptr);
	}
	g.desc_pools.clear();
	if(g.upload_fence) {
		vkDestroyFence(g.device, g.upload_fence, nullptr);
		g.upload_fence = VK_NULL_HANDLE;
	}
	if(g.upload_pool) {
		vkDestroyCommandPool(g.device, g.upload_pool, nullptr);
		g.upload_pool = VK_NULL_HANDLE;
	}
	for(int i = 0; i < 4; i++) {
		if(g.samplers[i]) {
			vkDestroySampler(g.device, g.samplers[i], nullptr);
			g.samplers[i] = VK_NULL_HANDLE;
		}
	}
	if(g.pipe_layout) {
		vkDestroyPipelineLayout(g.device, g.pipe_layout, nullptr);
		g.pipe_layout = VK_NULL_HANDLE;
	}
	if(g.dset_layout) {
		vkDestroyDescriptorSetLayout(g.device, g.dset_layout, nullptr);
		g.dset_layout = VK_NULL_HANDLE;
	}
	if(g.frag_mod) {
		vkDestroyShaderModule(g.device, g.frag_mod, nullptr);
		g.frag_mod = VK_NULL_HANDLE;
	}
	if(g.vert_mod) {
		vkDestroyShaderModule(g.device, g.vert_mod, nullptr);
		g.vert_mod = VK_NULL_HANDLE;
	}
}

bool recreate_swapchain() {
	if(!g.window)
		return false;
	int fb_w = 0, fb_h = 0;
	glfwGetFramebufferSize(g.window, &fb_w, &fb_h);
	if(fb_w == 0 || fb_h == 0) {
		g.swapchain_dirty = true;
		return true;
	}
	vkDeviceWaitIdle(g.device);
	VkFormat old_fmt = g.swapchain_format;
	if(!create_swapchain())
		return false;
	if(old_fmt != g.swapchain_format || (!g.use_dynamic_rendering && g.render_pass)) {
		if(!create_ui_pipelines())
			return false;
	}
	return true;
}

gfx_texture_t effective_tex() {
	if(!g.texture_2d || g.bound_tex == 0)
		return 0; /* sentinel: white */
	return g.bound_tex;
}

VkDescriptorSet dset_for_effective(gfx_texture_t tex) {
	if(tex == 0)
		return g.white.dset;
	TextureSlot* t = tex_from_handle(tex);
	return t ? t->dset : g.white.dset;
}

void gl_scissor_to_vk(int gl_x, int gl_y, int gl_w, int gl_h, int* ox, int* oy, int* ow, int* oh) {
	int fb_h = (int)g.swapchain_extent.height;
	*ox = gl_x;
	*oy = fb_h - gl_y - gl_h;
	*ow = gl_w;
	*oh = gl_h;
	if(*ox < 0) {
		*ow += *ox;
		*ox = 0;
	}
	if(*oy < 0) {
		*oh += *oy;
		*oy = 0;
	}
	if(*ox + *ow > (int)g.swapchain_extent.width)
		*ow = (int)g.swapchain_extent.width - *ox;
	if(*oy + *oh > fb_h)
		*oh = fb_h - *oy;
	if(*ow < 0)
		*ow = 0;
	if(*oh < 0)
		*oh = 0;
}

void set_dynamic_viewport_scissor(VkCommandBuffer cmd) {
	/* Negative viewport height: match GL clip Y with game ortho matrices. */
	float vp_x = (float)g.vp_x;
	float vp_w = (float)(g.vp_w > 0 ? g.vp_w : (int)g.swapchain_extent.width);
	float vp_h = (float)(g.vp_h > 0 ? g.vp_h : (int)g.swapchain_extent.height);
	float vp_y = (float)g.vp_y + vp_h;
	VkViewport vp{};
	vp.x = vp_x;
	vp.y = vp_y;
	vp.width = vp_w;
	vp.height = -vp_h;
	vp.minDepth = 0.f;
	vp.maxDepth = 1.f;
	vkCmdSetViewport(cmd, 0, 1, &vp);

	VkRect2D sci{};
	if(g.batch_scissor_on) {
		int x, y, w, h;
		gl_scissor_to_vk(g.batch_sci_x, g.batch_sci_y, g.batch_sci_w, g.batch_sci_h, &x, &y, &w, &h);
		sci.offset = {x, y};
		sci.extent = {(uint32_t)w, (uint32_t)h};
	} else {
		sci.offset = {0, 0};
		sci.extent = g.swapchain_extent;
	}
	vkCmdSetScissor(cmd, 0, 1, &sci);
}

void flush_batch() {
	if(!g.batch_open || g.batch_count == 0 || !g.rendering_active) {
		g.batch_open = false;
		g.batch_count = 0;
		return;
	}
	FrameSync& frame = g.frames[g.frame_index];
	VkCommandBuffer cmd = frame.cmd;

	VkPipeline pipe;
	if(g.batch_lines)
		pipe = g.batch_blend ? g.pipe_lines_blend : g.pipe_lines_opaque;
	else
		pipe = g.batch_blend ? g.pipe_quads_blend : g.pipe_quads_opaque;

	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
	set_dynamic_viewport_scissor(cmd);

	VkDescriptorSet ds = dset_for_effective(g.batch_tex);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g.pipe_layout, 0, 1, &ds, 0, nullptr);
	vkCmdPushConstants(cmd, g.pipe_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, 64, g.batch_mvp);

	VkDeviceSize off = 0;
	vkCmdBindVertexBuffers(cmd, 0, 1, &frame.vb, &off);
	vkCmdDraw(cmd, g.batch_count, 1, g.batch_first, 0);
	frame.draws_recorded = true;

	g.batch_open = false;
	g.batch_count = 0;
}

bool batch_state_matches(bool lines) {
	if(!g.batch_open)
		return false;
	if(g.batch_lines != lines)
		return false;
	if(g.batch_blend != g.blend)
		return false;
	if(g.batch_tex != effective_tex())
		return false;
	if(g.batch_scissor_on != g.scissor_on)
		return false;
	if(g.scissor_on &&
	   (g.batch_sci_x != g.sci_x || g.batch_sci_y != g.sci_y || g.batch_sci_w != g.sci_w || g.batch_sci_h != g.sci_h))
		return false;
	if(g.mvp_dirty)
		rebuild_mvp();
	if(std::memcmp(g.batch_mvp, g.mvp, sizeof(g.mvp)) != 0)
		return false;
	return true;
}

void open_batch(bool lines) {
	if(g.mvp_dirty)
		rebuild_mvp();
	FrameSync& frame = g.frames[g.frame_index];
	g.batch_open = true;
	g.batch_lines = lines;
	g.batch_blend = g.blend;
	g.batch_tex = effective_tex();
	g.batch_scissor_on = g.scissor_on;
	g.batch_sci_x = g.sci_x;
	g.batch_sci_y = g.sci_y;
	g.batch_sci_w = g.sci_w;
	g.batch_sci_h = g.sci_h;
	std::memcpy(g.batch_mvp, g.mvp, sizeof(g.mvp));
	g.batch_first = (uint32_t)(frame.vb_used / sizeof(UIVertex));
	g.batch_count = 0;
}

bool ensure_frame_begun();

UIVertex* reserve_verts(uint32_t count) {
	if(!ensure_frame_begun())
		return nullptr;
	FrameSync& frame = g.frames[g.frame_index];
	size_t need = frame.vb_used + (size_t)count * sizeof(UIVertex);
	if(need > frame.vb_capacity) {
		flush_batch();
		/* Never destroy a VB already referenced by recorded draws this frame. */
		if(frame.draws_recorded) {
			if(!g.logged_vb_grow) {
				log_warn("gfx_vk: UI vertex buffer exhausted mid-frame (%zu need, %zu cap); dropping verts", need,
						 frame.vb_capacity);
				g.logged_vb_grow = true;
			}
			return nullptr;
		}
		if(!grow_frame_vb(frame, need))
			return nullptr;
		g.batch_open = false;
	}
	UIVertex* ptr = reinterpret_cast<UIVertex*>(static_cast<uint8_t*>(frame.vb_mapped) + frame.vb_used);
	frame.vb_used += (size_t)count * sizeof(UIVertex);
	return ptr;
}

void emit_float_quads(const float* xy, const float* uv, int vertex_count) {
	if(vertex_count <= 0)
		return;
	if(!batch_state_matches(false)) {
		flush_batch();
		open_batch(false);
	}
	UIVertex* v = reserve_verts((uint32_t)vertex_count);
	if(!v)
		return;
	for(int i = 0; i < vertex_count; i++) {
		v[i].x = xy[i * 2 + 0];
		v[i].y = xy[i * 2 + 1];
		v[i].u = uv[i * 2 + 0] * g.tex_sx;
		v[i].v = uv[i * 2 + 1] * g.tex_sy;
		v[i].r = g.color[0];
		v[i].g = g.color[1];
		v[i].b = g.color[2];
		v[i].a = g.color[3];
	}
	g.batch_count += (uint32_t)vertex_count;
}

void emit_short_quads(const short* xy, const short* uv, int vertex_count) {
	if(vertex_count <= 0)
		return;
	if(!batch_state_matches(false)) {
		flush_batch();
		open_batch(false);
	}
	UIVertex* v = reserve_verts((uint32_t)vertex_count);
	if(!v)
		return;
	for(int i = 0; i < vertex_count; i++) {
		v[i].x = (float)xy[i * 2 + 0];
		v[i].y = (float)xy[i * 2 + 1];
		v[i].u = (float)uv[i * 2 + 0] * g.tex_sx;
		v[i].v = (float)uv[i * 2 + 1] * g.tex_sy;
		v[i].r = g.color[0];
		v[i].g = g.color[1];
		v[i].b = g.color[2];
		v[i].a = g.color[3];
	}
	g.batch_count += (uint32_t)vertex_count;
}

void emit_lines_2f(const float* xy_pairs, int vertex_count) {
	if(vertex_count <= 0)
		return;
	if(!batch_state_matches(true)) {
		flush_batch();
		open_batch(true);
	}
	UIVertex* v = reserve_verts((uint32_t)vertex_count);
	if(!v)
		return;
	for(int i = 0; i < vertex_count; i++) {
		v[i].x = xy_pairs[i * 2 + 0];
		v[i].y = xy_pairs[i * 2 + 1];
		v[i].u = 0.f;
		v[i].v = 0.f;
		v[i].r = g.color[0];
		v[i].g = g.color[1];
		v[i].b = g.color[2];
		v[i].a = g.color[3];
	}
	g.batch_count += (uint32_t)vertex_count;
}

void begin_rendering_pass(VkCommandBuffer cmd, uint32_t image_index, bool do_clear) {
	VkClearValue clear{};
	clear.color.float32[0] = g.clear_color[0];
	clear.color.float32[1] = g.clear_color[1];
	clear.color.float32[2] = g.clear_color[2];
	clear.color.float32[3] = g.clear_color[3];

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
		color_att.loadOp = do_clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
		color_att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		color_att.clearValue = clear;

		VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
		ri.renderArea.extent = g.swapchain_extent;
		ri.layerCount = 1;
		ri.colorAttachmentCount = 1;
		ri.pColorAttachments = &color_att;
		vkCmdBeginRendering(cmd, &ri);
	} else {
		VkRenderPassBeginInfo rpbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
		rpbi.renderPass = g.render_pass;
		rpbi.framebuffer = g.framebuffers[image_index];
		rpbi.renderArea.extent = g.swapchain_extent;
		rpbi.clearValueCount = 1;
		rpbi.pClearValues = &clear;
		vkCmdBeginRenderPass(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
	}
	g.rendering_active = true;
}

void end_rendering_pass(VkCommandBuffer cmd, uint32_t image_index) {
	if(!g.rendering_active)
		return;
	if(g.use_dynamic_rendering) {
		vkCmdEndRendering(cmd);
		VkImageMemoryBarrier to_present{};
		to_present.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		to_present.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		to_present.dstAccessMask = 0;
		to_present.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		to_present.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
		to_present.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		to_present.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		to_present.image = g.swapchain_images[image_index];
		to_present.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		to_present.subresourceRange.levelCount = 1;
		to_present.subresourceRange.layerCount = 1;
		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0,
							 0, nullptr, 0, nullptr, 1, &to_present);
	} else {
		vkCmdEndRenderPass(cmd);
	}
	g.rendering_active = false;
}

bool ensure_frame_begun() {
	if(g.frame_begun)
		return true;
	if(!g.device || !g.swapchain)
		return false;

	if(g.pending_w == 0 || g.pending_h == 0) {
		int fb_w = 0, fb_h = 0;
		glfwGetFramebufferSize(g.window, &fb_w, &fb_h);
		g.pending_w = fb_w;
		g.pending_h = fb_h;
		if(fb_w == 0 || fb_h == 0)
			return false;
	}
	if(g.swapchain_dirty) {
		if(!recreate_swapchain())
			return false;
		if(g.swapchain_extent.width == 0 || g.swapchain_extent.height == 0)
			return false;
	}

	FrameSync& frame = g.frames[g.frame_index];
	vkWaitForFences(g.device, 1, &frame.in_flight, VK_TRUE, UINT64_MAX);
	flush_pending_dset_rewrites();

	uint32_t image_index = 0;
	VkResult acq = vkAcquireNextImageKHR(g.device, g.swapchain, UINT64_MAX, frame.image_available, VK_NULL_HANDLE,
										 &image_index);
	if(acq == VK_ERROR_OUT_OF_DATE_KHR) {
		g.swapchain_dirty = true;
		recreate_swapchain();
		return false;
	}
	if(acq != VK_SUCCESS && acq != VK_SUBOPTIMAL_KHR) {
		log_error("vkAcquireNextImageKHR failed (%d)", (int)acq);
		return false;
	}

	if(g.images_in_flight[image_index] != VK_NULL_HANDLE)
		vkWaitForFences(g.device, 1, &g.images_in_flight[image_index], VK_TRUE, UINT64_MAX);
	g.images_in_flight[image_index] = frame.in_flight;

	vkResetFences(g.device, 1, &frame.in_flight);
	vkResetCommandBuffer(frame.cmd, 0);
	frame.vb_used = 0;
	frame.draws_recorded = false;
	g.batch_open = false;
	g.batch_count = 0;

	VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
	begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkBeginCommandBuffer(frame.cmd, &begin);

	g.acquired_image = image_index;
	g.frame_begun = true;
	begin_rendering_pass(frame.cmd, image_index, true);
	return true;
}

void end_frame_submit_present() {
	if(!g.frame_begun)
		return;

	FrameSync& frame = g.frames[g.frame_index];
	flush_batch();
	end_rendering_pass(frame.cmd, g.acquired_image);
	vkEndCommandBuffer(frame.cmd);

	VkSemaphore render_finished = g.render_finished_for_image[g.acquired_image];
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
		g.frame_begun = false;
		return;
	}

	VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
	pi.waitSemaphoreCount = 1;
	pi.pWaitSemaphores = &render_finished;
	pi.swapchainCount = 1;
	pi.pSwapchains = &g.swapchain;
	pi.pImageIndices = &g.acquired_image;
	VkResult pr = vkQueuePresentKHR(g.graphics_queue, &pi);
	if(pr == VK_ERROR_OUT_OF_DATE_KHR || pr == VK_SUBOPTIMAL_KHR)
		g.swapchain_dirty = true;
	else if(pr != VK_SUCCESS)
		log_error("vkQueuePresentKHR failed (%d)", (int)pr);

	g.frame_index = (g.frame_index + 1) % kFramesInFlight;
	g.frame_begun = false;
	g.rendering_active = false;
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
	vkGetPhysicalDeviceProperties(g.vkb_phys.physical_device, &g.phys_props);

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
	if(!create_ui_resources()) {
		log_fatal("UI pipeline / texture resources failed");
		exit(1);
	}

	int w = 0, h = 0;
	glfwGetFramebufferSize(g.window, &w, &h);
	g.pending_w = w;
	g.pending_h = h;
	log_info("Vulkan Phase 3 UI pipeline ready");
}

extern "C" void gfx_vk_shutdown(void) {
	if(g.device)
		vkDeviceWaitIdle(g.device);

	destroy_ui_resources();
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

	if(!g.frame_begun) {
		/* Nothing submitted this frame (e.g. minimized) — still clear once. */
		if(g.pending_w == 0 || g.pending_h == 0) {
			int fb_w = 0, fb_h = 0;
			glfwGetFramebufferSize(g.window, &fb_w, &fb_h);
			g.pending_w = fb_w;
			g.pending_h = fb_h;
			if(fb_w == 0 || fb_h == 0)
				return;
		}
		if(!ensure_frame_begun())
			return;
	}
	end_frame_submit_present();
}

extern "C" void gfx_vk_set_vsync(int interval) {
	g.vsync = interval != 0;
	(void)g.vsync;
}

extern "C" void gfx_vk_matrix_projection(const float* m16) {
	if(m16)
		std::memcpy(g.proj, m16, 16 * sizeof(float));
	g.mvp_dirty = true;
}
extern "C" void gfx_vk_matrix_modelview(const float* view16, const float* model16) {
	if(view16 && model16)
		mat4_mul(g.modelview, view16, model16);
	else if(view16)
		std::memcpy(g.modelview, view16, 16 * sizeof(float));
	g.mvp_dirty = true;
}
extern "C" void gfx_vk_matrix_texture(float sx, float sy) {
	flush_batch();
	g.tex_sx = sx;
	g.tex_sy = sy;
}
extern "C" void gfx_vk_pass_begin(gfx_pass_t pass) {
	if(pass == GFX_PASS_UI_2D) {
		ensure_frame_begun();
		flush_batch();
	}
}
extern "C" void gfx_vk_pass_end(gfx_pass_t pass) {
	if(pass == GFX_PASS_UI_2D)
		flush_batch();
}

extern "C" gfx_texture_t gfx_vk_texture_create_rgba(int w, int h, const void* pixels) {
	gfx_texture_t id = alloc_tex_handle();
	TextureSlot& t = g.textures[id - 1];
	if(!create_rgba_image(t, w, h, pixels, GFX_FILTER_NEAREST, GFX_WRAP_REPEAT)) {
		log_error("gfx_vk_texture_create_rgba failed");
		destroy_texture_slot(t);
		g.free_tex_ids.push_back(id);
		return 0;
	}
	return id;
}
extern "C" void gfx_vk_texture_upload_rgba(gfx_texture_t tex, int w, int h, const void* pixels) {
	TextureSlot* t = tex_from_handle(tex);
	if(!t)
		return;
	create_rgba_image(*t, w, h, pixels, GFX_FILTER_NEAREST, GFX_WRAP_REPEAT);
}
extern "C" gfx_texture_t gfx_vk_texture_create_alpha(int w, int h, const void* pixels) {
	std::vector<uint8_t> rgba((size_t)w * (size_t)h * 4);
	const uint8_t* src = static_cast<const uint8_t*>(pixels);
	for(int i = 0; i < w * h; i++) {
		rgba[i * 4 + 0] = 255;
		rgba[i * 4 + 1] = 255;
		rgba[i * 4 + 2] = 255;
		rgba[i * 4 + 3] = src ? src[i] : 255;
	}
	gfx_texture_t id = alloc_tex_handle();
	TextureSlot& t = g.textures[id - 1];
	if(!create_rgba_image(t, w, h, rgba.data(), GFX_FILTER_LINEAR, GFX_WRAP_CLAMP)) {
		log_error("gfx_vk_texture_create_alpha failed");
		destroy_texture_slot(t);
		g.free_tex_ids.push_back(id);
		return 0;
	}
	return id;
}
extern "C" void gfx_vk_texture_update_sub_rgba(gfx_texture_t tex, int x, int y, int w, int h, const void* pixels) {
	TextureSlot* t = tex_from_handle(tex);
	if(t)
		update_texture_sub(*t, x, y, w, h, pixels);
}
extern "C" void gfx_vk_texture_set_filter(gfx_texture_t tex, gfx_filter_t filter) {
	TextureSlot* t = tex_from_handle(tex);
	if(!t)
		return;
	t->filter = filter;
	request_dset_rewrite(*t);
}
extern "C" void gfx_vk_texture_set_filter_wrap_bound(gfx_filter_t filter, gfx_wrap_t wrap) {
	TextureSlot* t = tex_from_handle(g.bound_tex);
	if(!t)
		return;
	t->filter = filter;
	t->wrap = wrap;
	request_dset_rewrite(*t);
}
extern "C" void gfx_vk_texture_bind(gfx_texture_t tex) {
	if(g.bound_tex != tex)
		flush_batch();
	g.bound_tex = tex;
}
extern "C" void gfx_vk_texture_destroy(gfx_texture_t tex) {
	TextureSlot* t = tex_from_handle(tex);
	if(!t)
		return;
	if(g.bound_tex == tex) {
		flush_batch();
		g.bound_tex = 0;
	}
	destroy_texture_slot(*t);
	g.free_tex_ids.push_back(tex);
}
extern "C" int gfx_vk_max_texture_size(void) {
	return (int)g.phys_props.limits.maxImageDimension2D;
}
extern "C" int gfx_vk_supports_npot(void) {
	return 1;
}
extern "C" void gfx_vk_texture_2d(int enabled) {
	bool on = enabled != 0;
	if(g.texture_2d != on)
		flush_batch();
	g.texture_2d = on;
}
extern "C" void gfx_vk_blend(int enabled) {
	bool on = enabled != 0;
	if(g.blend != on)
		flush_batch();
	g.blend = on;
}
extern "C" void gfx_vk_draw_quads_2d(const float* xy, const float* uv, int vertex_count) {
	emit_float_quads(xy, uv, vertex_count);
}
extern "C" void gfx_vk_draw_quads_2d_short(const short* xy, const short* uv, int vertex_count) {
	emit_short_quads(xy, uv, vertex_count);
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
extern "C" void gfx_vk_color3f(float r, float g_f, float b) {
	g.color[0] = r;
	g.color[1] = g_f;
	g.color[2] = b;
	g.color[3] = 1.f;
}
extern "C" void gfx_vk_color3ub(unsigned char r, unsigned char g_u, unsigned char b) {
	g.color[0] = r / 255.f;
	g.color[1] = g_u / 255.f;
	g.color[2] = b / 255.f;
	g.color[3] = 1.f;
}
extern "C" void gfx_vk_color4f(float r, float g_f, float b, float a) {
	g.color[0] = r;
	g.color[1] = g_f;
	g.color[2] = b;
	g.color[3] = a;
}
extern "C" void gfx_vk_color4ub(unsigned char r, unsigned char g_u, unsigned char b, unsigned char a) {
	g.color[0] = r / 255.f;
	g.color[1] = g_u / 255.f;
	g.color[2] = b / 255.f;
	g.color[3] = a / 255.f;
}
extern "C" void gfx_vk_get_color4f(float out[4]) {
	if(out)
		std::memcpy(out, g.color, sizeof(g.color));
}
extern "C" void gfx_vk_multisample(int enabled) {
	GFX_VK_STUB_ONCE("gfx_multisample");
	(void)enabled;
}
extern "C" void gfx_vk_line_width(float w) {
	(void)w; /* UI lines fixed at 1.0 */
}
extern "C" void gfx_vk_draw_lines_2f(const float* xy_pairs, int vertex_count) {
	emit_lines_2f(xy_pairs, vertex_count);
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
	if(!g.scissor_on || g.sci_x != x || g.sci_y != y || g.sci_w != w || g.sci_h != h)
		flush_batch();
	g.scissor_on = true;
	g.sci_x = x;
	g.sci_y = y;
	g.sci_w = w;
	g.sci_h = h;
}
extern "C" void gfx_vk_scissor_off(void) {
	if(g.scissor_on)
		flush_batch();
	g.scissor_on = false;
}
extern "C" void gfx_vk_viewport(int x, int y, int w, int h) {
	flush_batch();
	g.vp_x = x;
	g.vp_y = y;
	g.vp_w = w;
	g.vp_h = h;
}
extern "C" void gfx_vk_clear_color(float r, float g_f, float b, float a) {
	g.clear_color[0] = r;
	g.clear_color[1] = g_f;
	g.clear_color[2] = b;
	g.clear_color[3] = a;
}
extern "C" void gfx_vk_clear(void) {
	ensure_frame_begun();
	/* Already cleared at begin; for world path this is a second clear.
	   Re-clear color attachment if rendering already started. */
	if(g.rendering_active && g.use_dynamic_rendering) {
		flush_batch();
		VkClearAttachment ca{};
		ca.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		ca.colorAttachment = 0;
		ca.clearValue.color.float32[0] = g.clear_color[0];
		ca.clearValue.color.float32[1] = g.clear_color[1];
		ca.clearValue.color.float32[2] = g.clear_color[2];
		ca.clearValue.color.float32[3] = g.clear_color[3];
		VkClearRect rect{};
		rect.rect.extent = g.swapchain_extent;
		rect.layerCount = 1;
		vkCmdClearAttachments(g.frames[g.frame_index].cmd, 1, &ca, 1, &rect);
	}
}
extern "C" void gfx_vk_clear_color_only(void) {
	gfx_vk_clear();
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
extern "C" void gfx_vk_model_texenv_color(float r, float g_f, float b) {
	GFX_VK_STUB_ONCE("gfx_model_texenv_color");
	(void)r;
	(void)g_f;
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
