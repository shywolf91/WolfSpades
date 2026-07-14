# Phase 1a — OpenGL Call-Site Audit

**Branch:** `phase-1-gfx-abstraction`  
**Scope:** `src/` reconnaissance only. No source changes.  
**Method:** Count live `gl*` / `glu*` call expressions in `.c` files (line comments excluded). Vendor sample code in `stb_truetype.h` and ES-compat macros in `common.h` are noted but not counted as game call sites.  
**Date:** 2026-07-11

---

## 1. Per-file inventory

| File | Approx. GL calls | Category | Notes |
|------|-----------------:|----------|-------|
| `hud.c` | 154 | HUD/2D/font | Dominated by `glColor3ub` / `glColor3f` (~107). Three `glBegin(GL_LINES)`…`glEnd` blobs (crosshair / chat underlines). Depth/blend/viewport toggles for overlays. |
| `glx.c` | 110 | framebuffer/init + world buffer path | Existing thin layer: display lists **or** VBOs, fog (texgen / lighting / ES fog), one shader compiler helper. Comment already says “future opengl-es abstraction layer”. |
| `texture.c` | 70 | HUD/2D/font | Create/upload (`GL_RGBA`), filter, delete; per-quad client-array draws with enable/disable of blend+texture. `glGetFloatv(GL_CURRENT_COLOR)` for shadows; `glGetIntegerv` / `glGetString(GL_EXTENSIONS)` for NPOT resize. |
| `model.c` | 63 | models/kv6 | Two render paths: classic mesh + lighting/`GL_COMBINE` team tint; point-sprite path with optional GLSL + `glPointSize` / atten. Uses `glx_displaylist_*`. |
| `main.c` | 63 | init/context + microui + map editor/misc + screenshot | Frame clear/fog/depth; block-outline client arrays; microui command replay (`glScissor`, colors, blend); `init()` GL defaults; `glewInit`; `glReadPixels` screenshot; reshape viewport. |
| `font.c` | 35 | HUD/2D/font | Bake atlas to `GL_ALPHA` texture; textured triangle batch via client arrays; texture-matrix scale trick (`glMatrixMode(GL_TEXTURE)` + `glScalef`); `glGetFloatv(GL_CURRENT_COLOR)` for shadows. |
| `tesselator.c` | 12 | world/chunks (+ particles) | Immediate client-array draw (`GL_QUADS` or `GL_TRIANGLES`); also feeds `glx_displaylist_update`. No particles file has direct GL — `particle.c` draws only via tesselator. |
| `map.c` | 10 | world/chunks | Damaged-block overlay: depth-func/blend around tesselator draw. Collapsing structures: `glColorMask` double-draw + blend. |
| `player.c` | 7 | models/kv6 (name tags) | Team color + `GL_ALPHA_TEST` around `font_centered` nameplates. |
| `matrix.c` | 5 | init/context (matrix upload) | CPU matrices (cglm) uploaded with `glMatrixMode` + `glLoadMatrixf` / `glMultMatrixf`. |
| `chunk.c` | 3 | world/chunks | Draw via `glx_displaylist_draw`; minimap `glTexSubImage2D` into `texture_minimap`. (Two `glPolygonMode` wireframe calls are commented out.) |
| `window.c` | 0 GL / **GL-specific GLFW** | init/context | See §1.1. No direct `gl*` calls. |
| `particle.c` | 0 | particles/tracers | Builds quads into `particle_tesselator` → `tesselator_draw`. |
| `tracer.c` | 0 | particles/tracers | Minimap/HUD via `texture_draw_rotated`. |
| `microui.c` | 0 | microui | Immediate-mode UI library; **no GL**. Rendering is in `main.c` command loop. |
| `common.h` | 0 (macros) | init/context | Includes GLEW / GLES; ES shims remap `glColor3*` / `glDepthRange` / `glClearDepth`. |
| `stb_truetype.h` | ~18 (sample only) | — | Example OpenGL snippet in the header; **not used** by game code (`font.c` owns GL). |

**Live game call-site total: ~532** `gl*` invocations across 11 `.c` files.

### 1.1 GLFW: GL-specific vs GL-agnostic

**GL-specific (must move into gfx / backend init):**

| Call / hint | Where | Role |
|-------------|-------|------|
| `glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR/MINOR, 1)` | `window.c` | Requests GL 1.1 context |
| `glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_ES_API)` (+ EGL) | `window.c` | ES builds |
| `glfwWindowHint(GLFW_SAMPLES, …)` | `window.c` | MSAA |
| `glfwWindowHint(GLFW_COCOA_RETINA_FRAMEBUFFER, …)` | `window.c` | Framebuffer sizing |
| `glfwCreateWindow` (as GL context owner) | `window.c` | Creates window + GL context |
| `glfwMakeContextCurrent` | `window.c` | Bind GL context |
| `glfwSwapBuffers` | `window.c` | Present |
| `glfwSwapInterval` | `window.c` | VSync |
| `glewInit` | `main.c` | Load GL entry points (desktop) |

**GL-agnostic (can stay in `window_*` for Vulkan):** input callbacks, clipboard, cursor mode, joystick/gamepad, title, `glfwPollEvents`, monitor/fullscreen geometry, time, close flag. Framebuffer-size callback stays useful for both APIs but should notify `gfx` for viewport/swapchain resize.

---

## 2. Distinct GL feature set in use

### Pipeline model

| Feature | Used? | Where |
|---------|-------|-------|
| Fixed-function pipeline | **Primary** | Almost everything: matrix stack upload, lighting, fog, texenv, client vertex arrays, colors |
| GLSL shaders | **Minimal** | One program in `model.c` (kv6 point sprites) via `glx_shader`; uses legacy `gl_Vertex` / `gl_ModelViewProjectionMatrix` / `gl_FrontColor` |
| Immediate mode (`glBegin`/`glEnd`) | **Rare** | 3 sites in `hud.c` (`GL_LINES` only) |
| Client vertex arrays | **Heavy** | `texture.c`, `font.c`, `tesselator.c`, `main.c` (block outline), `glx.c` (list compile path) |
| VBOs (`glGenBuffers` / `glBufferData` / `glBufferSubData`) | **Yes (modern path)** | `glx_displaylist_*` when `glx_version` and not `force_displaylist` |
| Display lists | **Yes (legacy path)** | Same `glx_displaylist_*` when GL &lt; 2 or `settings.force_displaylist` |
| FBOs / render-to-texture | **No** | — |
| `glReadPixels` | **Yes** | Screenshot in `main.c` (RGBA unsigned byte, full window) |

### Geometry / draw modes

- `GL_TRIANGLES` — 2D quads (texture/font), ES display-list path, tesselator-triangles build  
- `GL_QUADS` — desktop chunk/display-list and tesselator-quads build  
- `GL_POINTS` — kv6 “voxlap” point models  
- `GL_LINES` — HUD lines, block placement outline  

### Textures

| Format / usage | Detail |
|----------------|--------|
| `GL_RGBA` / `GL_UNSIGNED_BYTE` | PNG UI/world textures (`texture_create*`); minimap subimage; screenshots |
| `GL_ALPHA` / `GL_UNSIGNED_BYTE` | Font atlases (`font.c`) |
| Filters | `NEAREST` default; `LINEAR` for some UI / fog gradient / fonts |
| Wrap | `REPEAT` (most assets); `CLAMP_TO_EDGE` (fonts, fog gradient texgen) |
| Updates | `glTexSubImage2D` for per-chunk minimap tiles |
| Units | Mostly unit 0; fog path uses **unit 1** + texgen (`glx_enable_sphericalfog`) |

### Blend / depth / other state

- **Blend:** exclusively `glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA)` (11 sites).  
- **Depth:** `GL_LEQUAL` default; temporary `GL_EQUAL` for damaged blocks; `glDepthRange` for FP weapon (near slice); `glDepthMask` off for outlines.  
- **Alpha test:** player name tags (`GL_GREATER, 0.5`).  
- **Cull:** back faces, CCW.  
- **Fog:** `GL_EXP2` linear-ish density in `main.c`; spherical fog via multi-tex blend **or** fake spotlight lighting **or** ES `GL_FOG`.  
- **Lighting:** `GL_LIGHT0` / `GL_LIGHT1`, `GL_COLOR_MATERIAL`, `GL_NORMALIZE` for kv6 meshes / fog.  
- **TexEnv:** `GL_MODULATE` default; `GL_COMBINE` constant×previous for team colorize; fog uses `GL_BLEND` env on unit 1.  
- **Multisample:** enable/disable around point models and 2D pass.  
- **Scissor:** microui clip rects only.

### Matrix handling

- **CPU-owned** `matrix_view` / `matrix_model` / `matrix_projection` (cglm) with an 8-deep software stack (`matrix_push`/`pop`).  
- Upload: `matrix_upload()` → `GL_MODELVIEW` = view × model; `matrix_upload_p()` → `GL_PROJECTION`.  
- Extra: font temporarily hijacks **texture matrix** for fixed-point UV scaling; fog path may identity-upload model matrix mid-frame.

---

## 3. State management patterns

### Who owns what

| Concern | Owner today | Leak / coupling risk |
|---------|-------------|----------------------|
| Projection / view / model | `matrix.c` globals; callers push/pop | Upload is fire-and-forget into GL matrix stacks; GL and CPU can desync if something else touches `glMatrixMode` |
| Fog / spherical fog | `glx_fog` flag + enable/disable pair | Texgen + LIGHT1 + LIGHT_MODEL_AMBIENT mutated; disable path restores ambient to `{0.2,0.2,0.2,1}` |
| Current color | Implicit GL state; set via `glColor*` everywhere | `font_render_shadow` / `texture_draw_shadow` **query** `GL_CURRENT_COLOR` then restore |
| Texture bind | Each draw path bind → draw → bind 0 | Generally careful; font also sets wrap/filter every draw |
| Client array enables | Local enable/disable around draws | Nested draws (mu text → font → texture) re-enable `GL_BLEND` after font disables it (`main.c` explicitly re-enables) |
| Lighting / combine | `model.c` enable around mesh draw | Restored on exit of that function; still global while active |
| Program | `glUseProgram(kv6_program)` then `0` | Uniform locations looked up **every draw** via `glGetUniformLocation` |
| Viewport / scissor | `reshape` + mu clip | Scissor left disabled after 2D pass |

### Cross-system leakage observed

1. **2D pass assumes 3D left depth/fog in a known state** — `display()` disables depth/MSAA, switches ortho, then microui/HUD draw; re-enables MSAA after.  
2. **Font/texture disable blend** while microui expects blend on — mitigated by `glEnable(GL_BLEND)` after each text/icon command.  
3. **Fog enable mutates texture unit 1 and lighting** — must be paired; any early return would leave unit 1 dirty.  
4. **No central state tracker** — systems poke `glEnable`/`glDisable` directly; Phase 1b should funnel toggles through `gfx_*` with known defaults per pass (3D world / 3D models / 2D UI).

---

## 4. Proposed `gfx_*` API surface (signatures only)

Minimal mirror of **actual** usage — not a general RHI. Opaque handles replace `GLuint` / `glx_displaylist`.

```c
/* --- lifecycle / context (replaces GL-specific GLFW + glew + init state) --- */
bool gfx_init(void);                    /* after window+context exists, or creates them */
void gfx_shutdown(void);
void gfx_begin_frame(void);             /* clear color/depth as configured */
void gfx_end_frame(void);               /* swap */
void gfx_resize(int w, int h);          /* viewport */
void gfx_set_vsync(int mode);
void gfx_set_clear_color(float r, float g, float b, float a);

/* --- matrices (CPU already owns math; gfx only uploads / binds for backend) --- */
void gfx_set_matrices(const float* view /*mat4*/, const float* model /*mat4*/,
                      const float* projection /*mat4*/);
/* optional convenience matching today: */
void gfx_upload_modelview(const float* view, const float* model);
void gfx_upload_projection(const float* projection);

/* --- pass / state bundles (collapse dozens of glEnable pairs) --- */
typedef enum {
    GFX_PASS_WORLD,       /* depth on, cull on, fog as configured */
    GFX_PASS_MODELS,      /* lighting/point-size as needed by backend */
    GFX_PASS_OVERLAY_3D,  /* depth equal / lines / no depth write */
    GFX_PASS_UI_2D        /* ortho assumed already set; blend+scissor available */
} gfx_pass;

void gfx_begin_pass(gfx_pass pass);
void gfx_end_pass(gfx_pass pass);

void gfx_set_color4ub(unsigned char r, unsigned char g, unsigned char b, unsigned char a);
void gfx_set_color3f(float r, float g, float b);
void gfx_set_color4f(float r, float g, float b, float a);
void gfx_get_color4f(float out[4]);     /* replaces glGetFloatv(GL_CURRENT_COLOR) */

void gfx_set_line_width(float w);
void gfx_set_depth_range(float near, float far);
void gfx_set_depth_mask(bool write);
void gfx_set_depth_func_equal(bool equal);  /* EQUAL vs LEQUAL */
void gfx_set_color_mask(bool r, bool g, bool b, bool a);
void gfx_set_blend(bool enabled);           /* always SRC_ALPHA / ONE_MINUS when on */
void gfx_set_scissor(bool enabled, int x, int y, int w, int h);
void gfx_set_alpha_test(bool enabled);      /* GREATER 0.5 when on */
void gfx_set_multisample(bool enabled);

/* fog: wraps glx_enable/disable_sphericalfog + main EXP2 fog */
void gfx_fog_set_color(const float rgba[4]);
void gfx_fog_set_distance(float render_distance);
void gfx_fog_enable_spherical(bool smooth);
void gfx_fog_disable_spherical(void);
void gfx_fog_enable_exp2(bool on);

/* --- textures --- */
typedef uint32_t gfx_texture;

gfx_texture gfx_texture_create_rgba(int w, int h, const void* pixels);
gfx_texture gfx_texture_create_alpha(int w, int h, const void* pixels); /* fonts */
void gfx_texture_update_sub_rgba(gfx_texture t, int x, int y, int w, int h, const void* pixels);
void gfx_texture_set_filter(gfx_texture t, bool linear);
void gfx_texture_set_wrap(gfx_texture t, bool clamp_to_edge);
void gfx_texture_destroy(gfx_texture t);
int  gfx_max_texture_size(void);        /* replaces glGetIntegerv */
bool gfx_supports_npot(void);           /* replaces GL_EXTENSIONS string scan */

/* --- mesh buffers (replace glx_displaylist + tesselator draw path) --- */
typedef uint32_t gfx_mesh;

typedef enum {
    GFX_MESH_QUADS_SHORT,   /* chunk: short3 + ubyte4 color [+ byte3 normal] */
    GFX_MESH_QUADS_FLOAT,   /* enhanced / particles */
    GFX_MESH_POINTS_FLOAT   /* kv6 points + color + normal */
} gfx_mesh_format;

gfx_mesh gfx_mesh_create(gfx_mesh_format fmt, bool has_color, bool has_normal);
void gfx_mesh_upload(gfx_mesh m, size_t count,
                     const void* colors, const void* vertices, const void* normals);
void gfx_mesh_draw(gfx_mesh m);
void gfx_mesh_destroy(gfx_mesh m);

/* one-shot client draws still used by HUD / outlines / particles before meshing */
typedef enum { GFX_PRIM_TRIANGLES, GFX_PRIM_LINES, GFX_PRIM_POINTS } gfx_prim;

void gfx_draw_arrays_pos(gfx_prim prim, int n, int comps, int type /* SHORT/FLOAT */,
                         const void* positions);
void gfx_draw_arrays_pos_col(gfx_prim prim, int n,
                             int pos_comps, int pos_type, const void* positions,
                             const void* colors_rgba8);           /* tesselator */
void gfx_draw_arrays_pos_uv(gfx_prim prim, int n,
                            const float* positions2, const float* uvs2); /* texture quads */
void gfx_draw_arrays_pos_uv_short(gfx_prim prim, int n,
                                  const short* positions2, const short* uvs2); /* font */
void gfx_bind_texture(gfx_texture t);   /* 0 = unbind */
void gfx_draw_textured_quad(gfx_texture t, float x, float y, float w, float h,
                            float u, float v, float us, float vs);

/* lines still expressed as begin/end in HUD — collapse to: */
void gfx_draw_lines_2f(const float* xy_pairs, int vertex_count);

/* --- models (kv6 lighting / point program) --- */
void gfx_model_begin_mesh(bool colorize, float rgb[3], int team_rgb[3]);
void gfx_model_end_mesh(void);
void gfx_model_begin_points(float point_size, const float* model_mat4,
                            float fog_dist_factor, const float fog_rgb[3],
                            const float camera[3]);
void gfx_model_end_points(void);

/* --- readback --- */
void gfx_read_pixels_rgba(int x, int y, int w, int h, void* out);
```

**Intentionally omitted:** general shader API, arbitrary blend modes, UBOs, compute, multiple render targets — the game does not use them.

**Migration note:** Keep `glx_*` as a temporary implementation of `gfx_mesh_*` + fog during Phase 1b, then delete once all callers use `gfx_*`.

---

## 5. Risk list (hardest to abstract)

| Risk | Why hard | Mitigation |
|------|----------|------------|
| **Spherical fog** (`glx_enable_sphericalfog`) | Multi-texture texgen **or** spotlight lighting hack **or** ES fog — three backends already; Vulkan needs a real fog factor in the world shader | Treat as a named effect API; bake into world/model shaders in Phase 2; do not expose texgen |
| **kv6 point sprites + GLSL** | Mix of `GL_POINTS`, `gl_PointSize`, distance atten, and a legacy-compatible shader; MSAA toggled around draw | Single `gfx_model_begin_points` that owns program/pipeline; hide `glGetUniformLocation`-per-frame |
| **`GL_COMBINE` team tint** | Fixed-function texture environment graph with dummy 1×1 texture | Replace with a vertex color multiply or uniform tint in mesh draw |
| **Immediate `glBegin` lines in `hud.c`** | Color changes interleaved between vertices | Convert to `gfx_draw_lines_2f` + current color, or a tiny 2D line batcher |
| **`glReadPixels` screenshot** | Sync GPU readback; Vulkan needs staging + layout transitions | `gfx_read_pixels_rgba` with “may stall” contract; later async if needed |
| **`glGetFloatv(GL_CURRENT_COLOR)`** | Reads GL state mid-frame (`font`/`texture` shadows) | Track current color in `gfx` CPU-side; eliminate queries |
| **`glGetString(GL_EXTENSIONS)` / max texture size** | Init-time queries; extensions string is fragile | Caps structure filled once in `gfx_init` |
| **Texture matrix UV hack in `font.c`** | Relies on `GL_TEXTURE` matrix stack | Pass float UVs or a UV scale uniform; drop texture matrix |
| **Client arrays + `GL_QUADS`** | Removed in core GL/Vulkan; ES already uses triangles | Tesselator/mesh path always emits triangles to gfx |
| **Display-list dual path** | Two storage backends in one struct | One `gfx_mesh` upload API; backend chooses buffer strategy |
| **Microui draw coupling** | `main.c` interleaves GL color/scissor with `font_*` / `texture_*` which themselves toggle GL | Introduce `gfx_ui_*` helpers used by the mu command loop only |
| **Chunk worker → `glTexSubImage2D`** | Texture upload from chunk completion path (GL context affinity) | Queue uploads to render thread via `gfx_texture_update_sub_rgba` |

---

## 6. GLFW / GLEW summary

| Item | Count / location | Phase 1b action |
|------|------------------|-----------------|
| GLEW | `common.h` include + `glewInit` in `main.c` | Behind `gfx_init` (desktop GL backend only) |
| GL context hints + make current + swap | `window.c` | Split: `window` creates OS window; `gfx` owns surface/context/swapchain |
| Input / window chrome | `window.c` | Unchanged |

---

## 7. Indirect GL users (0 direct calls)

| File | How it hits GL |
|------|----------------|
| `particle.c` | `tesselator_draw` |
| `tracer.c` | `texture_draw_rotated` |
| `microui.c` | Command list consumed in `main.c` |
| `grenade.c` / `weapon.c` / etc. | Via `model` / HUD only |

These need **no** per-file GL edits if the callees are wrapped first.

---

## Appendix — Call-site totals by category

| Category | Files | Approx. calls |
|----------|-------|--------------:|
| HUD/2D/font | `hud.c`, `font.c`, `texture.c` | 259 |
| Buffer/fog/shader helper | `glx.c` | 110 |
| models/kv6 | `model.c`, `player.c` | 70 |
| init/context + microui replay + outline + screenshot | `main.c`, `matrix.c` | 68 |
| world/chunks | `map.c`, `chunk.c`, `tesselator.c` | 25 |
| particles/tracers (direct) | — | 0 |
| microui lib (direct) | — | 0 |
| **Total** | | **~532** |

---

## Phase 1b progress — Step 1: context/lifecycle

**Date:** 2026-07-14  
**Scope:** Window/context lifecycle only. Zero behavior change.

### Call sites moved into `gfx_gl.c`

| Former site | Call(s) | New API |
|-------------|---------|---------|
| `window.c` (GLFW) | `GLFW_CONTEXT_VERSION_*`, ES client/EGL hints, `GLFW_SAMPLES`, `GLFW_COCOA_RETINA_FRAMEBUFFER` | `gfx_apply_context_hints()` |
| `window.c` (SDL) | `SDL_GL_SetAttribute*` | `gfx_apply_context_hints()` |
| `window.c` | `glfwMakeContextCurrent` / `SDL_GL_CreateContext` | `gfx_init(window)` |
| `main.c` | `glewInit`, `glGetString` vendor logs, one-shot MSAA enable, `glGetError` drain | `gfx_init(window)` |
| `main.c` `init()` | depth/cull/hint/`glClearDepth`/`GL_LEQUAL`/smooth/`glDisable(GL_FOG)` | `gfx_init(window)` |
| `main.c` `reshape` | `glViewport` | `gfx_resize(w, h)` |
| `window.c` | `glfwSwapBuffers` / `SDL_GL_SwapWindow` | `gfx_swap_buffers()` |
| `window.c` | `glfwSwapInterval` / `SDL_GL_SetSwapInterval` | `gfx_set_vsync(interval)` via `window_swapping` |

**Left in place:** `glfwCreateWindow` / `SDL_CreateWindow`, input/chrome, per-frame `glClearColor`/`glClear` in `display()`, toggled `glEnable`/`glDisable`, `glx_init()` / rest of `glx.c`.

### `gfx.h` GL-type leak check

Confirmed: no `GLuint`/`GLenum`/`GLFWwindow`, no `GL`/`glew` includes. Case-insensitive search for `gl` outside the `gfx_` prefix is empty. Window is passed as `void*` (cast inside `gfx_gl.c`) so the public header stays API-agnostic.

### API deviations vs §4 proposal

- `gfx_init(void*)` after an already-created window (not `bool gfx_init(void)` that owns creation).
- Added `gfx_apply_context_hints()` because hints must precede window create.
- Present via `gfx_swap_buffers()`; no `gfx_begin_frame`/`gfx_end_frame`/`gfx_clear` this step — per-frame clear is interleaved with depth/`chunk_update_all` and stays in `main.c`.
- `gfx_set_vsync` matches the audit name; no backend vtable.

---

## Phase 1b progress — Step 2: matrices + state-pass bundles

**Date:** 2026-07-14  
**Scope:** Matrix upload + recurring enable/disable pass bundles only. Zero behavior change. No VBO promotion, no state rationalization.

### Passes defined (`gfx_pass_t`)

| Pass | `gfx_pass_begin` | `gfx_pass_end` |
|------|------------------|----------------|
| `GFX_PASS_WORLD_3D` | `glEnable(GL_DEPTH_TEST)`; `glDepthRange(0,1)` | (none — UI pass takes over) |
| `GFX_PASS_BLOCK_OUTLINE` | `glDisable(GL_DEPTH_TEST)`; `glDepthMask(GL_FALSE)` | `glEnable(GL_DEPTH_TEST)`; `glDepthMask(GL_TRUE)` |
| `GFX_PASS_DAMAGED` | `glDepthFunc(GL_EQUAL)`; blend on `SRC_ALPHA`/`ONE_MINUS_SRC_ALPHA` | `glDepthFunc(GL_LEQUAL)`; blend off |
| `GFX_PASS_COLLAPSING` | blend on (same func) | blend off |
| `GFX_PASS_NAMETAG` | `glEnable(GL_ALPHA_TEST)`; `glAlphaFunc(GL_GREATER, 0.5)`; depth off | depth on; alpha test off |
| `GFX_PASS_UI_2D` | depth off; `glDisable(GL_MULTISAMPLE)` | re-enable MSAA if `settings.multisamples > 0` |

`GFX_PASS_DAMAGED` and `GFX_PASS_COLLAPSING` intentionally kept separate (differ by depth-func).

### Matrix API

| API | GL sequence |
|-----|-------------|
| `gfx_matrix_projection(const float* m16)` | `glMatrixMode(GL_PROJECTION)`; `glLoadMatrixf` |
| `gfx_matrix_modelview(const float* view16, const float* model16)` | `glMatrixMode(GL_MODELVIEW)`; `glLoadMatrixf(view)`; `glMultMatrixf(model)` |

CPU matrix math stays in `matrix.c` / cglm. Upload sites only.

### Call sites converted

| File | Sites | Change |
|------|------:|--------|
| `matrix.c` | 2 | `matrix_upload` / `matrix_upload_p` → gfx matrix uploads |
| `main.c` | 3 passes | world begin; block-outline begin/end; UI-2D begin/end |
| `map.c` | 2 passes | damaged begin/end; collapsing begin/end |
| `player.c` | 1 pass | nametag begin/end |

### Sites left raw (and why)

| Site | Why |
|------|-----|
| `font.c` texture-matrix scale (`glMatrixMode(GL_TEXTURE)` + `glLoadIdentity`/`glScalef`) | Font/2D draw internals — step 3 |
| `texture.c` / `font.c` blend+`TEXTURE_2D` enable/disable around quads | Texture/font draw path — step 3 |
| `model.c` lighting / normalize / point-size / MSAA around mesh|points | Models — step 5 |
| `glx.c` spherical-fog enable/disable (texgen / light / fog) | Fog — step 5 |
| `main.c` `glEnable`/`glDisable(GL_FOG)` in `drawScene` / after spherical fog | Fog — step 5 |
| `main.c` microui replay (`BLEND`/`SCISSOR` + re-enable after font/icon) | Microui — step 6 |
| `hud.c` blend wrappers (scoreboard / chat shadow / FPS box) | HUD leftovers — step 6 |
| `hud.c` network-stats depth/`ColorMask`/`DepthFunc(NOTEQUAL)` outline | One-off HUD technique — step 6 |
| `hud.c:169` / `main.c` FP weapon `glDepthRange(0, 0.05)` | One-off parameter toggle, not an enable/disable bundle |
| `map.c` falling-block `glColorMask` double-draw | One-off draw technique interleaved with `glx_displaylist_draw` — step 4 |
| `main.c` `glShadeModel` / `glClear*` / `glLightfv` | Not matrix/pass-bundle scope |

### Naming deviations vs §4 / step brief

- `gfx_matrix_modelview(view, model)` takes **two** matrices so the backend can keep exact `Load`+`Mult` (brief example showed a single pre-multiplied `m16`).
- Enum/pass names are derived from actual bundles (`WORLD_3D`, `BLOCK_OUTLINE`, `DAMAGED`, `COLLAPSING`, `NAMETAG`, `UI_2D`) rather than the audit sketch (`WORLD` / `MODELS` / `OVERLAY_3D` / `UI_2D`).
- API is `gfx_pass_begin` / `gfx_pass_end` (brief) rather than audit `gfx_begin_pass` / `gfx_end_pass`.
- No `GFX_PASS_MODELS` — lighting stays in `model.c` until step 5.

### `gfx.h` GL-type leak check

Confirmed: no `GLuint`/`GLenum`/`GLFW` includes; public surface is `float*` + `gfx_pass_t` only.

---

## Phase 1b progress — Step 3: textures + 2D draw

**Date:** 2026-07-14  
**Scope:** Texture create/upload/bind/delete, NPOT/max-size queries, textured client-array 2D draws, font texture-matrix. Zero behavior change. No atlas restructure, no batching changes.

### API added

| API | Role |
|-----|------|
| `typedef uint32_t gfx_texture_t` | Opaque handle (stores backend id; 0 = none). Chosen over opaque struct for smaller diff vs existing `int`/`GLuint` storage. |
| `gfx_filter_t` / `gfx_wrap_t` | `NEAREST`/`LINEAR`, `REPEAT`/`CLAMP` — differences kept per call site. |
| `gfx_texture_create_rgba` | Gen + RGBA upload + **NEAREST min/mag + REPEAT** (matches `texture_create` / `create_buffer`). |
| `gfx_texture_upload_rgba` | Re-upload existing handle; same NEAREST+REPEAT params (`create_buffer` `new==0`). |
| `gfx_texture_create_alpha` | ALPHA atlas; **MIN_FILTER=LINEAR only** (matches font bake; MAG left at driver default). |
| `gfx_texture_update_sub_rgba` | Bind + `TexSubImage2D` + unbind (minimap). |
| `gfx_texture_set_filter` | Bind, set min+mag, unbind (matches `texture_filter`). |
| `gfx_texture_set_filter_wrap_bound` | Filter+wrap on currently bound tex (font draw-time LINEAR+CLAMP). |
| `gfx_texture_bind` / `gfx_texture_destroy` | Bind (0=unbind) / delete. |
| `gfx_max_texture_size` / `gfx_supports_npot` | Replace `glGetIntegerv` / `GL_EXTENSIONS` scan. |
| `gfx_texture_2d` / `gfx_blend` | Granular enables so texture vs font call order stays exact. |
| `gfx_draw_quads_2d` / `gfx_draw_quads_2d_short` | Float (texture) and short (font) client-array tris; no layout change. |
| `gfx_matrix_texture(sx, sy)` | `TEXTURE` mode: LoadIdentity + Scale; `(1,1)` resets (equiv. to prior LoadIdentity-only). |

### Per-file conversion counts

| File | Approx. GL sites before | Converted | Left raw |
|------|------------------------:|----------:|---------:|
| `texture.c` | ~70 | all create/filter/delete/draw/NPOT (~67) | 3 (`glGetFloatv` + 2× `glColor4f` in shadow) |
| `font.c` | ~35 | all atlas/matrix/draw (~32) | 3 (`glGetFloatv` + 2× `glColor4f` in shadow) |
| `chunk.c` | 3 (minimap subimage) | 3 | 0 (display-list calls remain — step 4) |

### Sites left raw (and why)

| Site | Why |
|------|-----|
| `texture_draw_shadow` / `font_render_shadow`: `glGetFloatv(GL_CURRENT_COLOR)` + matching `glColor4f` | Queries color set by **callers** (`hud.c` / `main.c` microui) via raw `glColor*`. Not last-write-wins through gfx; shadowing would be wrong until color sets move (step 6). |
| `glx.c` fog unit-1 bind of `texture_gradient` | Fog — step 5 |
| `model.c` bind of `texture_dummy` + `GL_COMBINE` | Models — step 5 |

### Filtering / wrap confirmation (unchanged per site)

| Site | Filter | Wrap |
|------|--------|------|
| `texture_create` / `texture_create_buffer` | NEAREST min+mag | REPEAT |
| `texture_filter(..., NEAREST/LINEAR)` | both min+mag to that mode | (unchanged) |
| Font bake (`gfx_texture_create_alpha`) | MIN=LINEAR only | (unset, as before) |
| `font_render` draw path | LINEAR min+mag | CLAMP_TO_EDGE |
| Minimap subimage | (no param change; upload only) | — |

### Naming deviations vs §4 / step brief

- Handle is `gfx_texture_t` (`uint32_t`), not opaque `struct gfx_texture`.
- Separate `gfx_draw_quads_2d` (float) and `gfx_draw_quads_2d_short` rather than one typed entry — matches the two layouts in use.
- `gfx_texture_2d` / `gfx_blend` instead of a combined begin/end, so font (texture → bind → params → blend) vs texture (texture → blend → bind) order is preserved.
- `gfx_matrix_texture(sx, sy)` rather than a full `m16` upload — matches actual LoadIdentity+Scale usage.
- No `gfx_get_color4f` this step (see raw sites).

### `gfx.h` GL-type leak check

Confirmed: no `GLuint`/`GLenum`/`GLFW`/GLEW includes; case-insensitive `gl` outside `gfx_` prefix is empty.