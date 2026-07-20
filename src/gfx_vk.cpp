/*
	Phase 4 Vulkan backend: 2D/UI pipeline + 3D world rendering (terrain meshes,
	transient tesselator draws, block outline, damaged/collapsing voxels, nametags,
	fog). kv6 model paths remain log-once stubs. GL backend is untouched.

	Coordinate convention (clip-space Y / NDC depth):
	  OpenGL and Vulkan differ in clip-space Y (GL +Y up, VK +Y down in NDC)
	  and depth range (GL Z in -1..1, VK Z in 0..1). We handle these two axes
	  independently:

	  1. Y: negative-viewport-height trick (VK_KHR_maintenance1 semantics, core
	     since Vulkan 1.1):
	       viewport.y = (float)fb_height;
	       viewport.height = -(float)fb_height;
	     so the SAME matrices the game uploads produce the SAME on-screen
	     orientation as GL. Do NOT also flip Y in the shader.

	  2. Z: GL→VK depth remap via a clip matrix folded into the MVP in
	     rebuild_mvp() (column-major, remaps Z from -1..1 to 0..1):
	       clip = identity; clip[10] = 0.5f; clip[14] = 0.5f;
	       mvp = clip * proj * modelview
	     This maps GL's z_ndc (-1..1) into VK's (0..1). Do NOT touch Y here.

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
#include "camera.h"
#include "config.h"
#include "gfx.h"
#include "gfx_backend.h"
#include "log.h"
#include "map.h"
#include "shaders_embedded.h"
}

namespace {

constexpr uint32_t kFramesInFlight = 2;
constexpr size_t kInitialVbBytes = 4u * 1024u * 1024u;
constexpr size_t kInitialWorldVbBytes = 4u * 1024u * 1024u;
constexpr uint32_t kDescSetsPerPool = 64;
constexpr size_t kWorldUboBytes = 112; /* std140: mat4 + 3*vec4 */
constexpr size_t kMeshPoolCapBytes = 64u * 1024u * 1024u;

struct UIVertex {
	float x, y;
	float u, v;
	float r, g, b, a;
};

/* World vertex: float3 position + packed R8G8B8A8_UNORM color (GL ubyte4 layout). */
struct WorldVertex {
	float x, y, z;
	uint32_t rgba;
};

/* Deferred-destruction / recycle record.
 *
 * ORDERING PROOF (Hypothesis A) — why free_after = serial + kFramesInFlight is
 * safe when flush runs AFTER the slot fence wait:
 *
 *   ensure_frame_begun():
 *     1) vkWaitForFences(frames[frame_index].in_flight)  // prior CB on THIS slot done
 *     2) frame_serial++                                  // now recording serial S
 *     3) flush_pending_retire()                          // free if free_after <= S
 *
 *   With FIF=2, slots alternate. Submissions on this slot were serial
 *   ..., S-4, S-2; the wait in (1) completed serial S-2. The OTHER slot may
 *   still be executing serial S-1.
 *
 *   A buffer retired while recording serial R gets free_after = R+2.
 *   It becomes eligible when frame_serial reaches R+2, i.e. at the begin of
 *   the frame that records R+2. That begin waits the fence for serial R on
 *   the same slot (R and R+2 share a slot). Serial R+1 (other slot) may still
 *   be in flight, but after retire the MeshSlot no longer points at the old
 *   buffer, so R+1 cannot newly bind it; only CBs that already recorded binds
 *   to it are R (same slot, waited) and possibly earlier. Hence the buffer is
 *   GPU-idle when freed.
 *
 *   CRITICAL: flush MUST remain after the fence wait. If flush ran before the
 *   wait, free_after=R+2 could destroy a buffer still referenced by the prior
 *   submission on this slot → disappearing geometry without a use-after-free
 *   validation error if memory is recycled quietly.
 */
struct RetireResource {
	enum { BUF, IMG, VIEW, DSET, MESH_BUF } kind;
	VkBuffer buffer = VK_NULL_HANDLE;
	VkImage image = VK_NULL_HANDLE;
	VkImageView view = VK_NULL_HANDLE;
	VkDescriptorSet dset = VK_NULL_HANDLE;
	VmaAllocation alloc = VK_NULL_HANDLE;
	void* mapped = nullptr;
	size_t capacity = 0;
	bool host_coherent = true;
	uint64_t free_after_serial = 0;
};

/* Size-bucketed host-visible mesh buffer pool (Phase 4.1). */
struct PooledMeshBuffer {
	VkBuffer buffer = VK_NULL_HANDLE;
	VmaAllocation alloc = VK_NULL_HANDLE;
	void* mapped = nullptr;
	size_t capacity = 0;
	bool host_coherent = true;
};

/* Persistent world mesh (former display-list / VBO). 1-based handle in gfx_mesh_t::modern. */
struct MeshSlot {
	bool alive = false;
	VkBuffer buffer = VK_NULL_HANDLE;
	VmaAllocation alloc = VK_NULL_HANDLE;
	void* mapped = nullptr;
	size_t capacity = 0;
	size_t vertex_count = 0; /* final GPU vertex count after quad→tri expand */
	gfx_mesh_type_t type = GFX_MESH_FLOAT;
	int has_color = 0;
	uint64_t last_used_serial = 0;	 /* last frame_serial that drew this buffer */
	uint64_t updated_serial = 0;	 /* frame_serial of last mesh_update */
	bool host_coherent = true;
};

/* Per-second mesh path stats (BUTTERSPADES_VK_STATS=1). */
struct VkMeshStats {
	bool enabled = false;
	double window_start = 0.0;
	uint32_t frames = 0;
	uint32_t mesh_updates = 0;
	uint32_t vma_allocs = 0;
	uint64_t vma_bytes = 0;
	uint32_t same_frame_draws = 0; /* draw after update in same serial */
	uint32_t pool_hits = 0;
	uint32_t pool_misses = 0;
	uint32_t inplace_writes = 0;
	size_t retire_len_max = 0;
	size_t pool_bytes = 0;
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

	/* Transient world (3D) vertex buffer — kept separate from the UI VB so we
	   never mix UIVertex and WorldVertex layouts in one buffer. */
	VkBuffer world_vb = VK_NULL_HANDLE;
	VmaAllocation world_vb_alloc = VK_NULL_HANDLE;
	void* world_vb_mapped = nullptr;
	size_t world_vb_capacity = 0;
	size_t world_vb_used = 0;
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
	uint64_t frame_serial = 0;
	std::vector<RetireResource> pending_retire;

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
	bool logged_world_vb_grow = false;

	/* ---- Phase 4: depth attachment ---- */
	VkImage depth_image = VK_NULL_HANDLE;
	VmaAllocation depth_alloc = VK_NULL_HANDLE;
	VkImageView depth_view = VK_NULL_HANDLE;
	VkFormat depth_format = VK_FORMAT_UNDEFINED;

	/* ---- Phase 4: world resources ---- */
	VkShaderModule world_vert_mod = VK_NULL_HANDLE;
	VkShaderModule world_frag_mod = VK_NULL_HANDLE;
	VkShaderModule nametag_frag_mod = VK_NULL_HANDLE;
	VkDescriptorSetLayout world_dset_layout = VK_NULL_HANDLE;
	VkPipelineLayout world_pipe_layout = VK_NULL_HANDLE;
	VkBuffer world_ubo = VK_NULL_HANDLE;
	VmaAllocation world_ubo_alloc = VK_NULL_HANDLE;
	void* world_ubo_mapped = nullptr;
	VkDescriptorSet world_dset = VK_NULL_HANDLE;
	VkDescriptorPool world_desc_pool = VK_NULL_HANDLE;

	VkPipeline pipe_world_opaque = VK_NULL_HANDLE;
	VkPipeline pipe_world_outline = VK_NULL_HANDLE;
	VkPipeline pipe_world_damaged = VK_NULL_HANDLE;
	VkPipeline pipe_world_collapse = VK_NULL_HANDLE;
	VkPipeline pipe_world_collapse_depth = VK_NULL_HANDLE;
	VkPipeline pipe_ui_nametag = VK_NULL_HANDLE;

	/* ---- Phase 4: world state ---- */
	gfx_pass_t current_pass = GFX_PASS_UI_2D;
	bool color_mask_all = true;
	bool depth_range_weapon = false;
	float model[16]{};

	bool fog_spherical = false;
	bool fog_exp2 = false;
	float fog_color[4] = {0.f, 0.f, 0.f, 1.f};
	float fog_density = 0.f;
	bool logged_mesh_update_retire = false;

	std::vector<MeshSlot> meshes;
	std::vector<uint32_t> free_mesh_ids; /* 1-based handles into meshes */

	std::vector<PooledMeshBuffer> mesh_pool;
	size_t mesh_pool_bytes = 0;
	bool logged_mesh_coherent = false;

	VkMeshStats stats{};
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
	/* clip * proj * modelview: fold the GL(-1..1)→VK(0..1) depth remap into the
	   MVP. Column-major: clip[10]=0.5, clip[14]=0.5 → z_vk = 0.5*z_gl + 0.5*w. */
	float clip[16];
	mat4_identity(clip);
	clip[10] = 0.5f;
	clip[14] = 0.5f;
	float pv[16];
	mat4_mul(pv, g.proj, g.modelview);
	mat4_mul(g.mvp, clip, pv);
	g.mvp_dirty = false;
}

uint32_t pack_color4f(const float c[4]) {
	auto b = [](float v) -> uint32_t {
		int i = (int)(v * 255.0f + 0.5f);
		if(i < 0)
			i = 0;
		if(i > 255)
			i = 255;
		return (uint32_t)i;
	};
	return b(c[0]) | (b(c[1]) << 8) | (b(c[2]) << 16) | (b(c[3]) << 24);
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

bool want_vk_stats() {
	const char* env = std::getenv("BUTTERSPADES_VK_STATS");
	return env && env[0] == '1' && env[1] == '\0';
}

void stats_tick_frame_end() {
	if(!g.stats.enabled)
		return;
	g.stats.frames++;
	if(g.pending_retire.size() > g.stats.retire_len_max)
		g.stats.retire_len_max = g.pending_retire.size();
	double now = glfwGetTime();
	if(g.stats.window_start <= 0.0)
		g.stats.window_start = now;
	if(now - g.stats.window_start < 1.0)
		return;
	float inv_f = g.stats.frames ? (1.f / (float)g.stats.frames) : 0.f;
	uint32_t pool_lookups = g.stats.pool_hits + g.stats.pool_misses;
	float hit_pct = pool_lookups ? (100.f * (float)g.stats.pool_hits / (float)pool_lookups) : 0.f;
	log_info("vk_stats: frames=%u upd/f=%.1f alloc/f=%.1f bytes/f=%.0f retire_max=%zu same_frame_draw/f=%.1f "
			 "inplace/f=%.1f pool_hit=%.0f%% (%u/%u) pool_bytes=%zu",
			 g.stats.frames, g.stats.mesh_updates * inv_f, g.stats.vma_allocs * inv_f,
			 (double)g.stats.vma_bytes * inv_f, g.stats.retire_len_max, g.stats.same_frame_draws * inv_f,
			 g.stats.inplace_writes * inv_f, hit_pct, g.stats.pool_hits, pool_lookups, g.stats.pool_bytes);
	g.stats.window_start = now;
	g.stats.frames = 0;
	g.stats.mesh_updates = 0;
	g.stats.vma_allocs = 0;
	g.stats.vma_bytes = 0;
	g.stats.same_frame_draws = 0;
	g.stats.pool_hits = 0;
	g.stats.pool_misses = 0;
	g.stats.inplace_writes = 0;
	g.stats.retire_len_max = 0;
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

VkFormat find_depth_format() {
	const VkFormat candidates[] = {VK_FORMAT_D32_SFLOAT, VK_FORMAT_D24_UNORM_S8_UINT};
	for(VkFormat f : candidates) {
		VkFormatProperties props{};
		vkGetPhysicalDeviceFormatProperties(g.vkb_phys.physical_device, f, &props);
		if(props.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)
			return f;
	}
	return VK_FORMAT_D32_SFLOAT;
}

VkImageAspectFlags depth_aspect() {
	VkImageAspectFlags a = VK_IMAGE_ASPECT_DEPTH_BIT;
	if(g.depth_format == VK_FORMAT_D24_UNORM_S8_UINT || g.depth_format == VK_FORMAT_D32_SFLOAT_S8_UINT)
		a |= VK_IMAGE_ASPECT_STENCIL_BIT;
	return a;
}

void destroy_depth_resources() {
	if(g.depth_view) {
		vkDestroyImageView(g.device, g.depth_view, nullptr);
		g.depth_view = VK_NULL_HANDLE;
	}
	if(g.depth_image) {
		vmaDestroyImage(g.allocator, g.depth_image, g.depth_alloc);
		g.depth_image = VK_NULL_HANDLE;
		g.depth_alloc = VK_NULL_HANDLE;
	}
}

bool create_depth_resources() {
	destroy_depth_resources();
	g.depth_format = find_depth_format();

	VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
	ici.imageType = VK_IMAGE_TYPE_2D;
	ici.format = g.depth_format;
	ici.extent = {g.swapchain_extent.width, g.swapchain_extent.height, 1};
	ici.mipLevels = 1;
	ici.arrayLayers = 1;
	ici.samples = VK_SAMPLE_COUNT_1_BIT;
	ici.tiling = VK_IMAGE_TILING_OPTIMAL;
	ici.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
	ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

	VmaAllocationCreateInfo aci{};
	aci.usage = VMA_MEMORY_USAGE_AUTO;
	aci.preferredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
	if(vmaCreateImage(g.allocator, &ici, &aci, &g.depth_image, &g.depth_alloc, nullptr) != VK_SUCCESS)
		return false;

	VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
	vci.image = g.depth_image;
	vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
	vci.format = g.depth_format;
	vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
	vci.subresourceRange.levelCount = 1;
	vci.subresourceRange.layerCount = 1;
	if(vkCreateImageView(g.device, &vci, nullptr, &g.depth_view) != VK_SUCCESS)
		return false;
	return true;
}

bool create_render_pass_12() {
	VkAttachmentDescription atts[2]{};
	atts[0].format = g.swapchain_format;
	atts[0].samples = VK_SAMPLE_COUNT_1_BIT;
	atts[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	atts[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	atts[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	atts[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	atts[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	atts[0].finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

	atts[1].format = g.depth_format;
	atts[1].samples = VK_SAMPLE_COUNT_1_BIT;
	atts[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	atts[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	atts[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	atts[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	atts[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	atts[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

	VkAttachmentReference color_ref{};
	color_ref.attachment = 0;
	color_ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	VkAttachmentReference depth_ref{};
	depth_ref.attachment = 1;
	depth_ref.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

	VkSubpassDescription sub{};
	sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	sub.colorAttachmentCount = 1;
	sub.pColorAttachments = &color_ref;
	sub.pDepthStencilAttachment = &depth_ref;

	VkSubpassDependency dep{};
	dep.srcSubpass = VK_SUBPASS_EXTERNAL;
	dep.dstSubpass = 0;
	dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
	dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
	dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

	VkRenderPassCreateInfo rpci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
	rpci.attachmentCount = 2;
	rpci.pAttachments = atts;
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
		VkImageView attachments[2] = {g.swapchain_views[i], g.depth_view};
		VkFramebufferCreateInfo fci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
		fci.renderPass = g.render_pass;
		fci.attachmentCount = 2;
		fci.pAttachments = attachments;
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
	destroy_depth_resources();
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

	if(!create_depth_resources()) {
		log_error("Failed to create depth attachment");
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

void destroy_frame_world_vb(FrameSync& f) {
	if(f.world_vb) {
		vmaDestroyBuffer(g.allocator, f.world_vb, f.world_vb_alloc);
		f.world_vb = VK_NULL_HANDLE;
		f.world_vb_alloc = VK_NULL_HANDLE;
		f.world_vb_mapped = nullptr;
		f.world_vb_capacity = 0;
		f.world_vb_used = 0;
	}
}

bool create_frame_world_vb(FrameSync& f, size_t bytes) {
	destroy_frame_world_vb(f);
	VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
	bci.size = bytes;
	bci.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
	VmaAllocationCreateInfo aci{};
	aci.usage = VMA_MEMORY_USAGE_AUTO;
	aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
	VmaAllocationInfo ainfo{};
	if(vmaCreateBuffer(g.allocator, &bci, &aci, &f.world_vb, &f.world_vb_alloc, &ainfo) != VK_SUCCESS)
		return false;
	f.world_vb_mapped = ainfo.pMappedData;
	f.world_vb_capacity = bytes;
	f.world_vb_used = 0;
	return true;
}

void free_dset(VkDescriptorSet s);
void flush_batch();
void enqueue_retire(RetireResource r);

void destroy_retired(RetireResource& r) {
	switch(r.kind) {
		case RetireResource::BUF:
			if(r.buffer)
				vmaDestroyBuffer(g.allocator, r.buffer, r.alloc);
			break;
		case RetireResource::MESH_BUF:
			/* Handled in flush → pool_release; should not reach here. */
			if(r.buffer)
				vmaDestroyBuffer(g.allocator, r.buffer, r.alloc);
			break;
		case RetireResource::IMG:
			if(r.image)
				vmaDestroyImage(g.allocator, r.image, r.alloc);
			break;
		case RetireResource::VIEW:
			if(r.view)
				vkDestroyImageView(g.device, r.view, nullptr);
			break;
		case RetireResource::DSET:
			if(r.dset)
				free_dset(r.dset);
			break;
	}
}

size_t mesh_bucket_bytes(size_t need) {
	size_t b = 4096;
	while(b < need)
		b *= 2;
	return b;
}

void pool_evict_until(size_t max_bytes) {
	while(g.mesh_pool_bytes > max_bytes && !g.mesh_pool.empty()) {
		PooledMeshBuffer& p = g.mesh_pool.back();
		if(p.buffer)
			vmaDestroyBuffer(g.allocator, p.buffer, p.alloc);
		g.mesh_pool_bytes -= p.capacity;
		g.mesh_pool.pop_back();
	}
	g.stats.pool_bytes = g.mesh_pool_bytes;
}

void pool_put(VkBuffer buffer, VmaAllocation alloc, void* mapped, size_t capacity, bool host_coherent) {
	if(!buffer)
		return;
	if(capacity > kMeshPoolCapBytes / 4) {
		/* Oversized: never pool. */
		vmaDestroyBuffer(g.allocator, buffer, alloc);
		return;
	}
	pool_evict_until(kMeshPoolCapBytes > capacity ? kMeshPoolCapBytes - capacity : 0);
	PooledMeshBuffer p;
	p.buffer = buffer;
	p.alloc = alloc;
	p.mapped = mapped;
	p.capacity = capacity;
	p.host_coherent = host_coherent;
	g.mesh_pool.push_back(p);
	g.mesh_pool_bytes += capacity;
	g.stats.pool_bytes = g.mesh_pool_bytes;
}

bool pool_take(size_t need, VkBuffer* out_buf, VmaAllocation* out_alloc, void** out_mapped, size_t* out_cap,
			   bool* out_coherent) {
	size_t best_i = (size_t)-1;
	size_t best_cap = (size_t)-1;
	for(size_t i = 0; i < g.mesh_pool.size(); i++) {
		size_t c = g.mesh_pool[i].capacity;
		if(c >= need && c < best_cap) {
			best_cap = c;
			best_i = i;
		}
	}
	if(best_i == (size_t)-1)
		return false;
	PooledMeshBuffer p = g.mesh_pool[best_i];
	g.mesh_pool_bytes -= p.capacity;
	g.mesh_pool.erase(g.mesh_pool.begin() + (std::ptrdiff_t)best_i);
	g.stats.pool_bytes = g.mesh_pool_bytes;
	*out_buf = p.buffer;
	*out_alloc = p.alloc;
	*out_mapped = p.mapped;
	*out_cap = p.capacity;
	*out_coherent = p.host_coherent;
	return true;
}

bool alloc_mesh_buffer(size_t need, VkBuffer* out_buf, VmaAllocation* out_alloc, void** out_mapped, size_t* out_cap,
					   bool* out_coherent) {
	if(pool_take(need, out_buf, out_alloc, out_mapped, out_cap, out_coherent)) {
		if(g.stats.enabled)
			g.stats.pool_hits++;
		return true;
	}
	if(g.stats.enabled)
		g.stats.pool_misses++;

	size_t bytes = mesh_bucket_bytes(need);
	VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
	bci.size = bytes;
	bci.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
	VmaAllocationCreateInfo aci{};
	aci.usage = VMA_MEMORY_USAGE_AUTO;
	aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
	VmaAllocationInfo ainfo{};
	if(vmaCreateBuffer(g.allocator, &bci, &aci, out_buf, out_alloc, &ainfo) != VK_SUCCESS)
		return false;
	if(g.stats.enabled) {
		g.stats.vma_allocs++;
		g.stats.vma_bytes += bytes;
	}
	*out_mapped = ainfo.pMappedData;
	*out_cap = bytes;
	VkMemoryPropertyFlags mem_flags = 0;
	vmaGetAllocationMemoryProperties(g.allocator, *out_alloc, &mem_flags);
	*out_coherent = (mem_flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
	if(!g.logged_mesh_coherent) {
		log_info("gfx_vk: mesh host buffers HOST_COHERENT=%d (flush if 0)", (int)*out_coherent);
		g.logged_mesh_coherent = true;
	}
	return true;
}

void retire_mesh_buffer(VkBuffer buffer, VmaAllocation alloc, void* mapped, size_t capacity, bool host_coherent) {
	if(!buffer)
		return;
	RetireResource r;
	r.kind = RetireResource::MESH_BUF;
	r.buffer = buffer;
	r.alloc = alloc;
	r.mapped = mapped;
	r.capacity = capacity;
	r.host_coherent = host_coherent;
	enqueue_retire(r);
}

/* True if no in-flight CB can still reference this mesh's current buffer.
   See ORDERING PROOF: last_used <= S - FIF means both slots are past that use. */
bool mesh_buffer_cpu_exclusive(const MeshSlot& s) {
	if(!s.buffer)
		return true;
	if(s.last_used_serial == 0)
		return true;
	if(g.frame_serial < kFramesInFlight)
		return false;
	return s.last_used_serial <= g.frame_serial - kFramesInFlight;
}

/* Free anything whose free_after_serial has elapsed. Called AFTER the current
   frame slot's fence wait and frame_serial++ (see ORDERING PROOF on RetireResource).
   Descriptor sets freed before image views; MESH_BUF recycled into the size pool. */
void flush_pending_retire() {
	std::vector<RetireResource> keep;
	keep.reserve(g.pending_retire.size());
	for(auto& r : g.pending_retire) {
		if(r.free_after_serial <= g.frame_serial && r.kind == RetireResource::DSET)
			destroy_retired(r);
		else
			keep.push_back(r);
	}
	std::vector<RetireResource> keep2;
	keep2.reserve(keep.size());
	for(auto& r : keep) {
		if(r.free_after_serial <= g.frame_serial) {
			if(r.kind == RetireResource::MESH_BUF)
				pool_put(r.buffer, r.alloc, r.mapped, r.capacity, r.host_coherent);
			else
				destroy_retired(r);
		} else {
			keep2.push_back(r);
		}
	}
	g.pending_retire.swap(keep2);
}

void flush_all_retire_idle() {
	if(g.device)
		vkDeviceWaitIdle(g.device);
	for(auto& r : g.pending_retire) {
		if(r.kind == RetireResource::DSET)
			destroy_retired(r);
	}
	for(auto& r : g.pending_retire) {
		if(r.kind == RetireResource::MESH_BUF)
			pool_put(r.buffer, r.alloc, r.mapped, r.capacity, r.host_coherent);
		else if(r.kind != RetireResource::DSET)
			destroy_retired(r);
	}
	g.pending_retire.clear();
}

/* Deferred destruction: resources may be referenced by either in-flight frame.
   Schedule free/recycle at frame_serial + kFramesInFlight (see ORDERING PROOF).
   Outside a frame, wait idle then destroy or pool immediately. */
void enqueue_retire(RetireResource r) {
	if(!g.frame_begun) {
		vkDeviceWaitIdle(g.device);
		if(r.kind == RetireResource::MESH_BUF)
			pool_put(r.buffer, r.alloc, r.mapped, r.capacity, r.host_coherent);
		else
			destroy_retired(r);
		return;
	}
	r.free_after_serial = g.frame_serial + kFramesInFlight;
	g.pending_retire.push_back(r);
}

void retire_buffer(VkBuffer buffer, VmaAllocation alloc) {
	if(!buffer)
		return;
	RetireResource r;
	r.kind = RetireResource::BUF;
	r.buffer = buffer;
	r.alloc = alloc;
	enqueue_retire(r);
}

void retire_image(VkImage image, VmaAllocation alloc) {
	if(!image)
		return;
	RetireResource r;
	r.kind = RetireResource::IMG;
	r.image = image;
	r.alloc = alloc;
	enqueue_retire(r);
}

void retire_view(VkImageView view) {
	if(!view)
		return;
	RetireResource r;
	r.kind = RetireResource::VIEW;
	r.view = view;
	enqueue_retire(r);
}

void retire_dset(VkDescriptorSet dset) {
	if(!dset)
		return;
	RetireResource r;
	r.kind = RetireResource::DSET;
	r.dset = dset;
	enqueue_retire(r);
}

void destroy_frames() {
	flush_all_retire_idle();
	for(auto& f : g.frames) {
		destroy_frame_vb(f);
		destroy_frame_world_vb(f);
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
		if(!create_frame_world_vb(f, kInitialWorldVbBytes))
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

bool grow_frame_world_vb(FrameSync& f, size_t need) {
	size_t new_cap = f.world_vb_capacity ? f.world_vb_capacity * 2 : kInitialWorldVbBytes;
	while(new_cap < need)
		new_cap *= 2;
	if(!g.logged_world_vb_grow) {
		log_warn("gfx_vk: growing world vertex buffer to %zu bytes", new_cap);
		g.logged_world_vb_grow = true;
	}
	std::vector<uint8_t> saved(f.world_vb_used);
	if(f.world_vb_used && f.world_vb_mapped)
		std::memcpy(saved.data(), f.world_vb_mapped, f.world_vb_used);
	size_t used = f.world_vb_used;
	if(!create_frame_world_vb(f, new_cap))
		return false;
	if(used && f.world_vb_mapped)
		std::memcpy(f.world_vb_mapped, saved.data(), used);
	f.world_vb_used = used;
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
	if(!t.alive && !t.image && !t.view && !t.dset)
		return;
	/* Flush UI draws that may reference this dset before retiring it. */
	if(g.frame_begun)
		flush_batch();
	/* Retire GPU objects; do not recycle the dset until after FIF delay so an
	   in-flight CB / stale binding cannot outlive the image view. */
	retire_view(t.view);
	retire_image(t.image, t.alloc);
	retire_dset(t.dset);
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

VkPipeline create_ui_pipeline(VkPrimitiveTopology topo, bool blend_on, VkShaderModule frag_mod) {
	VkPipelineShaderStageCreateInfo stages[2]{};
	stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	stages[0].module = g.vert_mod;
	stages[0].pName = "main";
	stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	stages[1].module = frag_mod;
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
		ri.depthAttachmentFormat = g.depth_format;
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
	g.pipe_quads_blend = create_ui_pipeline(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, true, g.frag_mod);
	g.pipe_quads_opaque = create_ui_pipeline(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, false, g.frag_mod);
	g.pipe_lines_blend = create_ui_pipeline(VK_PRIMITIVE_TOPOLOGY_LINE_LIST, true, g.frag_mod);
	g.pipe_lines_opaque = create_ui_pipeline(VK_PRIMITIVE_TOPOLOGY_LINE_LIST, false, g.frag_mod);
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

/* ---------------------------------------------------------------------------
   Phase 4: world (3D) pipelines and resources
   --------------------------------------------------------------------------- */

struct WorldPipeCfg {
	VkPrimitiveTopology topo;
	bool depth_test;
	bool depth_write;
	VkCompareOp compare;
	bool blend;
	bool color_write;
};

VkPipeline create_world_pipeline(const WorldPipeCfg& cfg) {
	VkPipelineShaderStageCreateInfo stages[2]{};
	stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	stages[0].module = g.world_vert_mod;
	stages[0].pName = "main";
	stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	stages[1].module = g.world_frag_mod;
	stages[1].pName = "main";

	VkVertexInputBindingDescription bind{};
	bind.binding = 0;
	bind.stride = sizeof(WorldVertex);
	bind.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

	VkVertexInputAttributeDescription attrs[2]{};
	attrs[0] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(WorldVertex, x)};
	attrs[1] = {1, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(WorldVertex, rgba)};

	VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
	vi.vertexBindingDescriptionCount = 1;
	vi.pVertexBindingDescriptions = &bind;
	vi.vertexAttributeDescriptionCount = 2;
	vi.pVertexAttributeDescriptions = attrs;

	VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
	ia.topology = cfg.topo;

	VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
	vp.viewportCount = 1;
	vp.scissorCount = 1;

	VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
	rs.polygonMode = VK_POLYGON_MODE_FILL;
	/* Cull NONE: negative-viewport-height inverts winding vs GL, so rather than
	   flip front-face we simply draw both sides (matches GL visual output). */
	rs.cullMode = VK_CULL_MODE_NONE;
	rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
	rs.lineWidth = 1.0f;

	VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
	ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

	VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
	ds.depthTestEnable = cfg.depth_test ? VK_TRUE : VK_FALSE;
	ds.depthWriteEnable = cfg.depth_write ? VK_TRUE : VK_FALSE;
	ds.depthCompareOp = cfg.compare;

	VkPipelineColorBlendAttachmentState ba{};
	ba.colorWriteMask = cfg.color_write ? (VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
										   VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT)
									   : 0;
	if(cfg.blend) {
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
	pci.layout = g.world_pipe_layout;

	VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
	if(g.use_dynamic_rendering) {
		ri.colorAttachmentCount = 1;
		ri.pColorAttachmentFormats = &g.swapchain_format;
		ri.depthAttachmentFormat = g.depth_format;
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

void destroy_world_pipelines() {
	auto dpipe = [](VkPipeline& p) {
		if(p) {
			vkDestroyPipeline(g.device, p, nullptr);
			p = VK_NULL_HANDLE;
		}
	};
	dpipe(g.pipe_world_opaque);
	dpipe(g.pipe_world_outline);
	dpipe(g.pipe_world_damaged);
	dpipe(g.pipe_world_collapse);
	dpipe(g.pipe_world_collapse_depth);
	dpipe(g.pipe_ui_nametag);
}

bool create_world_pipelines() {
	destroy_world_pipelines();
	/* name | topo | depthTest | depthWrite | compare | blend | colorWrite */
	g.pipe_world_opaque = create_world_pipeline(
		{VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, true, true, VK_COMPARE_OP_LESS_OR_EQUAL, false, true});
	g.pipe_world_outline =
		create_world_pipeline({VK_PRIMITIVE_TOPOLOGY_LINE_LIST, false, false, VK_COMPARE_OP_LESS_OR_EQUAL, false, true});
	g.pipe_world_damaged =
		create_world_pipeline({VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, true, true, VK_COMPARE_OP_EQUAL, true, true});
	g.pipe_world_collapse = create_world_pipeline(
		{VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, true, true, VK_COMPARE_OP_LESS_OR_EQUAL, true, true});
	g.pipe_world_collapse_depth = create_world_pipeline(
		{VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, true, true, VK_COMPARE_OP_LESS_OR_EQUAL, false, false});
	/* Nametags: UI vertex layout + alpha-test frag, using the UI pipeline layout. */
	g.pipe_ui_nametag = create_ui_pipeline(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, true, g.nametag_frag_mod);
	return g.pipe_world_opaque && g.pipe_world_outline && g.pipe_world_damaged && g.pipe_world_collapse &&
		g.pipe_world_collapse_depth && g.pipe_ui_nametag;
}

bool create_world_resources() {
	mat4_identity(g.model);

	g.world_vert_mod = create_shader_module(world_vert_spv, world_vert_spv_len);
	g.world_frag_mod = create_shader_module(world_frag_spv, world_frag_spv_len);
	g.nametag_frag_mod = create_shader_module(ui_nametag_frag_spv, ui_nametag_frag_spv_len);
	if(!g.world_vert_mod || !g.world_frag_mod || !g.nametag_frag_mod) {
		log_error("Failed to create world/nametag shader modules");
		return false;
	}

	VkDescriptorSetLayoutBinding binding{};
	binding.binding = 0;
	binding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	binding.descriptorCount = 1;
	binding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
	VkDescriptorSetLayoutCreateInfo dlci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
	dlci.bindingCount = 1;
	dlci.pBindings = &binding;
	if(vkCreateDescriptorSetLayout(g.device, &dlci, nullptr, &g.world_dset_layout) != VK_SUCCESS)
		return false;

	VkPushConstantRange pcr{};
	pcr.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
	pcr.offset = 0;
	pcr.size = 64;
	VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
	plci.setLayoutCount = 1;
	plci.pSetLayouts = &g.world_dset_layout;
	plci.pushConstantRangeCount = 1;
	plci.pPushConstantRanges = &pcr;
	if(vkCreatePipelineLayout(g.device, &plci, nullptr, &g.world_pipe_layout) != VK_SUCCESS)
		return false;

	VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
	bci.size = kWorldUboBytes;
	bci.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
	VmaAllocationCreateInfo aci{};
	aci.usage = VMA_MEMORY_USAGE_AUTO;
	aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
	VmaAllocationInfo ainfo{};
	if(vmaCreateBuffer(g.allocator, &bci, &aci, &g.world_ubo, &g.world_ubo_alloc, &ainfo) != VK_SUCCESS)
		return false;
	g.world_ubo_mapped = ainfo.pMappedData;

	VkDescriptorPoolSize ps{};
	ps.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	ps.descriptorCount = 1;
	VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
	dpci.maxSets = 1;
	dpci.poolSizeCount = 1;
	dpci.pPoolSizes = &ps;
	if(vkCreateDescriptorPool(g.device, &dpci, nullptr, &g.world_desc_pool) != VK_SUCCESS)
		return false;

	VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
	dsai.descriptorPool = g.world_desc_pool;
	dsai.descriptorSetCount = 1;
	dsai.pSetLayouts = &g.world_dset_layout;
	if(vkAllocateDescriptorSets(g.device, &dsai, &g.world_dset) != VK_SUCCESS)
		return false;

	VkDescriptorBufferInfo dbi{};
	dbi.buffer = g.world_ubo;
	dbi.offset = 0;
	dbi.range = kWorldUboBytes;
	VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
	w.dstSet = g.world_dset;
	w.dstBinding = 0;
	w.descriptorCount = 1;
	w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	w.pBufferInfo = &dbi;
	vkUpdateDescriptorSets(g.device, 1, &w, 0, nullptr);

	if(!create_world_pipelines()) {
		log_error("Failed to create world pipelines");
		return false;
	}
	return true;
}

void destroy_world_resources() {
	destroy_world_pipelines();

	for(auto& s : g.meshes) {
		if(s.buffer)
			vmaDestroyBuffer(g.allocator, s.buffer, s.alloc);
		s = MeshSlot{};
	}
	g.meshes.clear();
	g.free_mesh_ids.clear();

	if(g.world_ubo) {
		vmaDestroyBuffer(g.allocator, g.world_ubo, g.world_ubo_alloc);
		g.world_ubo = VK_NULL_HANDLE;
		g.world_ubo_alloc = VK_NULL_HANDLE;
		g.world_ubo_mapped = nullptr;
	}
	if(g.world_desc_pool) {
		vkDestroyDescriptorPool(g.device, g.world_desc_pool, nullptr);
		g.world_desc_pool = VK_NULL_HANDLE;
		g.world_dset = VK_NULL_HANDLE;
	}
	if(g.world_pipe_layout) {
		vkDestroyPipelineLayout(g.device, g.world_pipe_layout, nullptr);
		g.world_pipe_layout = VK_NULL_HANDLE;
	}
	if(g.world_dset_layout) {
		vkDestroyDescriptorSetLayout(g.device, g.world_dset_layout, nullptr);
		g.world_dset_layout = VK_NULL_HANDLE;
	}
	if(g.nametag_frag_mod) {
		vkDestroyShaderModule(g.device, g.nametag_frag_mod, nullptr);
		g.nametag_frag_mod = VK_NULL_HANDLE;
	}
	if(g.world_frag_mod) {
		vkDestroyShaderModule(g.device, g.world_frag_mod, nullptr);
		g.world_frag_mod = VK_NULL_HANDLE;
	}
	if(g.world_vert_mod) {
		vkDestroyShaderModule(g.device, g.world_vert_mod, nullptr);
		g.world_vert_mod = VK_NULL_HANDLE;
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
		if(g.world_pipe_layout && !create_world_pipelines())
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
	vp.maxDepth = g.depth_range_weapon ? 0.05f : 1.f;
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
	if(g.current_pass == GFX_PASS_NAMETAG && !g.batch_lines && g.pipe_ui_nametag)
		pipe = g.pipe_ui_nametag; /* alpha-test font in 3D space */
	else if(g.batch_lines)
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

/* ---------------------------------------------------------------------------
   Phase 4: world draw helpers
   --------------------------------------------------------------------------- */

MeshSlot* mesh_from_handle(uint32_t h) {
	if(h == 0 || h > g.meshes.size())
		return nullptr;
	return &g.meshes[h - 1];
}

uint32_t alloc_mesh_id() {
	if(!g.free_mesh_ids.empty()) {
		uint32_t id = g.free_mesh_ids.back();
		g.free_mesh_ids.pop_back();
		return id;
	}
	g.meshes.emplace_back();
	return (uint32_t)g.meshes.size();
}

/* Pack the game's separate [pos][color] blocks into interleaved WorldVertex.
   Quads (count % 4 == 0) expand to two triangles (0,1,2, 0,2,3). POINTS copy
   1:1. Positions come as short3 or float3; colors as GL ubyte4 (uint32 RGBA). */
void build_world_vertices(std::vector<WorldVertex>& out, size_t count, gfx_mesh_type_t type, const void* vertex,
						  const void* color, int has_color) {
	out.clear();
	if(count == 0 || !vertex)
		return;

	const uint32_t fallback = pack_color4f(g.color);
	auto get_pos = [&](size_t i, float& x, float& y, float& z) {
		if(type == GFX_MESH_SHORT) {
			const int16_t* p = static_cast<const int16_t*>(vertex) + i * 3;
			x = (float)p[0];
			y = (float)p[1];
			z = (float)p[2];
		} else {
			const float* p = static_cast<const float*>(vertex) + i * 3;
			x = p[0];
			y = p[1];
			z = p[2];
		}
	};
	auto get_col = [&](size_t i) -> uint32_t {
		if(has_color && color)
			return static_cast<const uint32_t*>(color)[i];
		return fallback;
	};

	if(type == GFX_MESH_POINTS) {
		out.resize(count);
		for(size_t i = 0; i < count; i++) {
			float x, y, z;
			get_pos(i, x, y, z);
			out[i] = {x, y, z, get_col(i)};
		}
		return;
	}

	const size_t quads = count / 4;
	static const int idx[6] = {0, 1, 2, 0, 2, 3};
	out.reserve(quads * 6);
	for(size_t q = 0; q < quads; q++) {
		for(int k = 0; k < 6; k++) {
			size_t si = q * 4 + (size_t)idx[k];
			float x, y, z;
			get_pos(si, x, y, z);
			out.push_back({x, y, z, get_col(si)});
		}
	}
}

void update_world_ubo() {
	if(!g.world_ubo_mapped)
		return;
	uint8_t* p = static_cast<uint8_t*>(g.world_ubo_mapped);
	std::memcpy(p + 0, g.model, 64);

	float fc[4];
	if(g.fog_spherical) {
		fc[0] = fog_color[0];
		fc[1] = fog_color[1];
		fc[2] = fog_color[2];
		fc[3] = fog_color[3];
	} else {
		std::memcpy(fc, g.fog_color, sizeof(fc));
	}
	std::memcpy(p + 64, fc, 16);

	float cam[4] = {camera_x, camera_y, camera_z, settings.render_distance};
	std::memcpy(p + 80, cam, 16);

	uint32_t flags = 0;
	if(g.fog_spherical)
		flags |= 1u;
	if(g.fog_exp2)
		flags |= 2u;
	float params[4] = {g.fog_density, (float)flags, 0.f, 0.f};
	std::memcpy(p + 96, params, 16);
}

VkPipeline select_world_pipeline(bool lines) {
	if(lines)
		return g.pipe_world_outline;
	switch(g.current_pass) {
		case GFX_PASS_DAMAGED: return g.pipe_world_damaged;
		case GFX_PASS_COLLAPSING: return g.color_mask_all ? g.pipe_world_collapse : g.pipe_world_collapse_depth;
		case GFX_PASS_WORLD_3D:
		default: return g.pipe_world_opaque;
	}
}

void set_world_viewport_scissor(VkCommandBuffer cmd) {
	float vp_w = (float)(g.vp_w > 0 ? g.vp_w : (int)g.swapchain_extent.width);
	float vp_h = (float)(g.vp_h > 0 ? g.vp_h : (int)g.swapchain_extent.height);
	VkViewport vp{};
	vp.x = (float)g.vp_x;
	vp.y = (float)g.vp_y + vp_h;
	vp.width = vp_w;
	vp.height = -vp_h;
	vp.minDepth = 0.f;
	vp.maxDepth = g.depth_range_weapon ? 0.05f : 1.0f;
	vkCmdSetViewport(cmd, 0, 1, &vp);

	VkRect2D sci{};
	if(g.scissor_on) {
		int x, y, w, h;
		gl_scissor_to_vk(g.sci_x, g.sci_y, g.sci_w, g.sci_h, &x, &y, &w, &h);
		sci.offset = {x, y};
		sci.extent = {(uint32_t)w, (uint32_t)h};
	} else {
		sci.offset = {0, 0};
		sci.extent = g.swapchain_extent;
	}
	vkCmdSetScissor(cmd, 0, 1, &sci);
}

/* Common per-draw world setup: begin frame, flush any pending UI batch, refresh
   the world UBO, bind the world pipeline for the current pass + push MVP. */
bool ensure_world_rendering(bool lines) {
	if(!ensure_frame_begun())
		return false;
	flush_batch();
	update_world_ubo();

	FrameSync& frame = g.frames[g.frame_index];
	VkCommandBuffer cmd = frame.cmd;
	VkPipeline pipe = select_world_pipeline(lines);
	if(!pipe)
		return false;
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
	set_world_viewport_scissor(cmd);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g.world_pipe_layout, 0, 1, &g.world_dset, 0, nullptr);
	if(g.mvp_dirty)
		rebuild_mvp();
	vkCmdPushConstants(cmd, g.world_pipe_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, 64, g.mvp);
	return true;
}

void world_draw_transient(const WorldVertex* verts, size_t vcount, bool lines) {
	if(vcount == 0 || !verts)
		return;
	if(!ensure_world_rendering(lines))
		return;
	FrameSync& frame = g.frames[g.frame_index];
	size_t bytes = vcount * sizeof(WorldVertex);
	size_t need = frame.world_vb_used + bytes;
	if(need > frame.world_vb_capacity) {
		/* Never destroy a world VB already referenced by recorded draws. */
		if(frame.draws_recorded) {
			if(!g.logged_world_vb_grow) {
				log_warn("gfx_vk: world vertex buffer exhausted mid-frame (%zu need, %zu cap); dropping verts", need,
						 frame.world_vb_capacity);
				g.logged_world_vb_grow = true;
			}
			return;
		}
		if(!grow_frame_world_vb(frame, need))
			return;
	}
	uint32_t first = (uint32_t)(frame.world_vb_used / sizeof(WorldVertex));
	std::memcpy(static_cast<uint8_t*>(frame.world_vb_mapped) + frame.world_vb_used, verts, bytes);
	frame.world_vb_used += bytes;

	VkDeviceSize off = 0;
	vkCmdBindVertexBuffers(frame.cmd, 0, 1, &frame.world_vb, &off);
	vkCmdDraw(frame.cmd, (uint32_t)vcount, 1, first, 0);
	frame.draws_recorded = true;
}

void begin_rendering_pass(VkCommandBuffer cmd, uint32_t image_index, bool do_clear) {
	VkClearValue clear{};
	clear.color.float32[0] = g.clear_color[0];
	clear.color.float32[1] = g.clear_color[1];
	clear.color.float32[2] = g.clear_color[2];
	clear.color.float32[3] = g.clear_color[3];

	VkClearValue depth_clear{};
	depth_clear.depthStencil.depth = 1.0f;
	depth_clear.depthStencil.stencil = 0;

	if(g.use_dynamic_rendering) {
		VkImageMemoryBarrier barriers[2]{};
		barriers[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		barriers[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		barriers[0].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		barriers[0].newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		barriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barriers[0].image = g.swapchain_images[image_index];
		barriers[0].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		barriers[0].subresourceRange.levelCount = 1;
		barriers[0].subresourceRange.layerCount = 1;

		barriers[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		barriers[1].dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
		barriers[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		barriers[1].newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
		barriers[1].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barriers[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barriers[1].image = g.depth_image;
		barriers[1].subresourceRange.aspectMask = depth_aspect();
		barriers[1].subresourceRange.levelCount = 1;
		barriers[1].subresourceRange.layerCount = 1;
		vkCmdPipelineBarrier(cmd,
							 VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
							 VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
								 VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
							 0, 0, nullptr, 0, nullptr, 2, barriers);

		VkRenderingAttachmentInfo color_att{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
		color_att.imageView = g.swapchain_views[image_index];
		color_att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		color_att.loadOp = do_clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
		color_att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		color_att.clearValue = clear;

		VkRenderingAttachmentInfo depth_att{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
		depth_att.imageView = g.depth_view;
		depth_att.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
		depth_att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
		depth_att.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		depth_att.clearValue = depth_clear;

		VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
		ri.renderArea.extent = g.swapchain_extent;
		ri.layerCount = 1;
		ri.colorAttachmentCount = 1;
		ri.pColorAttachments = &color_att;
		ri.pDepthAttachment = &depth_att;
		vkCmdBeginRendering(cmd, &ri);
	} else {
		VkClearValue clears[2] = {clear, depth_clear};
		VkRenderPassBeginInfo rpbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
		rpbi.renderPass = g.render_pass;
		rpbi.framebuffer = g.framebuffers[image_index];
		rpbi.renderArea.extent = g.swapchain_extent;
		rpbi.clearValueCount = 2;
		rpbi.pClearValues = clears;
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
	g.frame_serial++;
	flush_pending_retire();
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
	frame.world_vb_used = 0;
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
	stats_tick_frame_end();
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
	if(!create_world_resources()) {
		log_fatal("World pipeline / UBO resources failed");
		exit(1);
	}

	int w = 0, h = 0;
	glfwGetFramebufferSize(g.window, &w, &h);
	g.pending_w = w;
	g.pending_h = h;
	g.stats.enabled = want_vk_stats();
	if(g.stats.enabled)
		log_info("Vulkan mesh stats logging ON (BUTTERSPADES_VK_STATS=1)");
	log_info("Vulkan Phase 4 world + UI pipelines ready");
}

extern "C" void gfx_vk_shutdown(void) {
	g.frame_begun = false;
	if(g.device)
		vkDeviceWaitIdle(g.device);
	flush_all_retire_idle();
	pool_evict_until(0);

	destroy_world_resources();
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
	/* Keep the raw model matrix for world-space fog in the world UBO. */
	if(model16)
		std::memcpy(g.model, model16, 16 * sizeof(float));
	else
		mat4_identity(g.model);
	g.mvp_dirty = true;
}
extern "C" void gfx_vk_matrix_texture(float sx, float sy) {
	flush_batch();
	g.tex_sx = sx;
	g.tex_sy = sy;
}
extern "C" void gfx_vk_pass_begin(gfx_pass_t pass) {
	flush_batch();
	g.current_pass = pass;
	switch(pass) {
		case GFX_PASS_WORLD_3D:
			ensure_frame_begun();
			g.depth_range_weapon = false;
			break;
		case GFX_PASS_UI_2D:
			ensure_frame_begun();
			break;
		case GFX_PASS_BLOCK_OUTLINE:
		case GFX_PASS_DAMAGED:
		case GFX_PASS_COLLAPSING:
		case GFX_PASS_NAMETAG:
		default:
			break;
	}
}
extern "C" void gfx_vk_pass_end(gfx_pass_t pass) {
	(void)pass;
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
	if(!m)
		return;
	m->legacy = 0;
	m->modern = alloc_mesh_id();
	m->size = 0;
	m->buffer_size = 0;
	m->has_color = has_color;
	m->has_normal = has_normal;
	MeshSlot* s = mesh_from_handle(m->modern);
	if(s) {
		*s = MeshSlot{};
		s->has_color = has_color;
		s->alive = true;
	}
}
extern "C" void gfx_vk_mesh_destroy(gfx_mesh_t* m) {
	if(!m)
		return;
	MeshSlot* s = mesh_from_handle(m->modern);
	if(s) {
		if(s->buffer)
			retire_mesh_buffer(s->buffer, s->alloc, s->mapped, s->capacity, s->host_coherent);
		*s = MeshSlot{};
		if(m->modern)
			g.free_mesh_ids.push_back(m->modern);
	}
	m->modern = 0;
	m->size = 0;
	m->buffer_size = 0;
}
extern "C" void gfx_vk_mesh_update(gfx_mesh_t* m, size_t count, gfx_mesh_type_t type, const void* color,
								  const void* vertex, const void* normal) {
	(void)normal; /* normals ignored in the Vulkan world path */
	if(!m)
		return;
	MeshSlot* s = mesh_from_handle(m->modern);
	if(!s)
		return;

	if(g.stats.enabled)
		g.stats.mesh_updates++;

	std::vector<WorldVertex> verts;
	build_world_vertices(verts, count, type, vertex, color, m->has_color);

	m->size = count;
	if(count > m->buffer_size)
		m->buffer_size = count;

	s->type = type;
	s->has_color = m->has_color;
	s->alive = true;
	s->updated_serial = g.frame_serial;

	const size_t bytes = verts.size() * sizeof(WorldVertex);
	if(verts.empty()) {
		/* Keep existing GPU buffer so draws still work until a non-empty update;
		   only clear the draw count. */
		s->vertex_count = 0;
		return;
	}

	/* Hypothesis B: vertex_count and buffer swap are applied together after the
	   new contents are written — never leave count>0 with a null/stale buffer. */

	/* In-place write when data fits and no in-flight CB references this buffer. */
	if(s->buffer && bytes <= s->capacity && mesh_buffer_cpu_exclusive(*s)) {
		std::memcpy(s->mapped, verts.data(), bytes);
		if(!s->host_coherent)
			vmaFlushAllocation(g.allocator, s->alloc, 0, bytes);
		s->vertex_count = verts.size();
		if(g.stats.enabled)
			g.stats.inplace_writes++;
		return;
	}

	VkBuffer new_buf = VK_NULL_HANDLE;
	VmaAllocation new_alloc = VK_NULL_HANDLE;
	void* new_mapped = nullptr;
	size_t new_cap = 0;
	bool new_coherent = true;
	if(!alloc_mesh_buffer(bytes, &new_buf, &new_alloc, &new_mapped, &new_cap, &new_coherent)) {
		s->vertex_count = 0;
		return;
	}
	std::memcpy(new_mapped, verts.data(), bytes);
	if(!new_coherent)
		vmaFlushAllocation(g.allocator, new_alloc, 0, bytes);

	if(s->buffer)
		retire_mesh_buffer(s->buffer, s->alloc, s->mapped, s->capacity, s->host_coherent);

	s->buffer = new_buf;
	s->alloc = new_alloc;
	s->mapped = new_mapped;
	s->capacity = new_cap;
	s->host_coherent = new_coherent;
	s->vertex_count = verts.size();

	if(!g.logged_mesh_update_retire) {
		log_info("gfx_vk: mesh_update uses pool/in-place path (see BUTTERSPADES_VK_STATS=1)");
		g.logged_mesh_update_retire = true;
	}
}
extern "C" void gfx_vk_mesh_draw(gfx_mesh_t* m, gfx_mesh_type_t type) {
	(void)type;
	if(!m)
		return;
	MeshSlot* s = mesh_from_handle(m->modern);
	if(!s || !s->alive || !s->buffer || s->vertex_count == 0)
		return;
	if(s->type == GFX_MESH_POINTS)
		return; /* no point pipeline (kv6 models stubbed) */
	if(!ensure_world_rendering(false))
		return;
	if(g.stats.enabled && s->updated_serial == g.frame_serial)
		g.stats.same_frame_draws++;
	s->last_used_serial = g.frame_serial;
	FrameSync& frame = g.frames[g.frame_index];
	VkDeviceSize off = 0;
	vkCmdBindVertexBuffers(frame.cmd, 0, 1, &s->buffer, &off);
	vkCmdDraw(frame.cmd, (uint32_t)s->vertex_count, 1, 0, 0);
	frame.draws_recorded = true;
}
extern "C" void gfx_vk_draw_arrays(gfx_mesh_type_t type, size_t count, const void* vertex, const void* color,
								  const void* normal) {
	(void)normal;
	if(count == 0 || !vertex)
		return;
	if(type == GFX_MESH_POINTS)
		return; /* no point pipeline (kv6 models stubbed) */
	std::vector<WorldVertex> verts;
	build_world_vertices(verts, count, type, vertex, color, color ? 1 : 0);
	world_draw_transient(verts.data(), verts.size(), false);
}
extern "C" void gfx_vk_color_mask(int r, int g_, int b, int a) {
	bool all_on = r && g_ && b && a;
	if(g.color_mask_all != all_on)
		flush_batch();
	g.color_mask_all = all_on;
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
	if(!xyz || vertex_count <= 0)
		return;
	std::vector<WorldVertex> verts((size_t)vertex_count);
	uint32_t col = pack_color4f(g.color);
	for(int i = 0; i < vertex_count; i++) {
		const short* p = xyz + i * 3;
		verts[(size_t)i] = {(float)p[0], (float)p[1], (float)p[2], col};
	}
	world_draw_transient(verts.data(), verts.size(), true);
}
extern "C" void gfx_vk_depth_range_weapon(void) {
	g.depth_range_weapon = true;
}
extern "C" void gfx_vk_depth_range_reset(void) {
	g.depth_range_weapon = false;
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
	/* Already cleared at begin; for the world path this is a second clear of
	   both color and depth (GL clears both here before drawing the world). */
	if(g.rendering_active) {
		flush_batch();
		VkClearAttachment ca[2]{};
		ca[0].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		ca[0].colorAttachment = 0;
		ca[0].clearValue.color.float32[0] = g.clear_color[0];
		ca[0].clearValue.color.float32[1] = g.clear_color[1];
		ca[0].clearValue.color.float32[2] = g.clear_color[2];
		ca[0].clearValue.color.float32[3] = g.clear_color[3];
		ca[1].aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
		ca[1].clearValue.depthStencil.depth = 1.0f;
		VkClearRect rect{};
		rect.rect.extent = g.swapchain_extent;
		rect.layerCount = 1;
		vkCmdClearAttachments(g.frames[g.frame_index].cmd, 2, ca, 1, &rect);
	}
}
extern "C" void gfx_vk_clear_color_only(void) {
	ensure_frame_begun();
	if(g.rendering_active) {
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
	if(color4)
		std::memcpy(g.fog_color, color4, sizeof(g.fog_color));
	g.fog_density = density;
	g.fog_exp2 = true;
}
extern "C" void gfx_vk_fog_disable(void) {
	g.fog_exp2 = false;
}
extern "C" void gfx_vk_fog_enable_spherical(void) {
	g.fog_spherical = true;
}
extern "C" void gfx_vk_fog_disable_spherical(void) {
	g.fog_spherical = false;
}
extern "C" int gfx_vk_fog_active(void) {
	return g.fog_spherical ? 1 : 0;
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
