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

---

## Phase 1b progress — Step 4: meshes (displaylists + tesselator)

**Date:** 2026-07-14  
**Scope:** Move `glx_displaylist_*` submission and `tesselator_draw` client-array draws behind `gfx_mesh_*` / `gfx_draw_arrays`. Zero behavior change. No VBO promotion, no vertex-format / batching / draw-order changes. `model.c`/kv6 left on `glx_displaylist_*` wrappers (step 5).

### API added

| API | Role |
|-----|------|
| `typedef struct gfx_mesh { … } gfx_mesh_t` | Embeddable mesh handle (former `glx_displaylist` fields; no GL types). Completeness chosen over incomplete opaque type so chunks/collapsing can keep by-value storage. |
| `gfx_mesh_type_t` | `GFX_MESH_SHORT` / `FLOAT` / `POINTS` — mirrors `GLX_DISPLAYLIST_NORMAL` / `ENHANCED` / `POINTS`. |
| `gfx_mesh_create` / `destroy` / `update` / `draw` | Exact former display-list **or** VBO dual path (`glx_version` + `force_displaylist`). |
| `gfx_draw_arrays` | Transient client-array path for `tesselator_draw` (color/normal pointers nullable). |
| `gfx_color_mask` | Collapsing double-draw depth pre-pass (`glColorMask` around two `gfx_mesh_draw`s). |

### Vertex layouts found

| Type | Position | Optional attrs | Primitive |
|------|----------|----------------|-----------|
| `GFX_MESH_SHORT` | `short3` | `ubyte4` color, `byte3` normal | `GL_QUADS` / ES `GL_TRIANGLES` |
| `GFX_MESH_FLOAT` | `float3` | same | same |
| `GFX_MESH_POINTS` | `float3` | same | `GL_POINTS` (model / step 5) |

### Mechanism preservation

- Desktop + `!force_displaylist` + `glx_version`: VBO upload/draw unchanged (not converted to client arrays).
- Desktop legacy / `force_displaylist`: `glNewList` / `glCallList` unchanged (not converted to VBOs or client arrays).
- ES: VBO path unchanged.
- `tesselator_draw`: remains immediate client arrays via `gfx_draw_arrays` (never a persistent mesh).

### Per-file conversion counts

| File | Before | Converted | Left for later |
|------|-------:|----------:|----------------|
| `glx.c` | displaylist create/update/draw/destroy (~40 GL) | body → `gfx_gl.c`; thin wrappers remain | fog + shader (step 5) |
| `tesselator.c` | 1 draw + 4 update sites | all → `gfx_draw_arrays` / `gfx_mesh_update`; `tesselator_glx` → `tesselator_gfx` | — |
| `chunk.c` / `chunk.h` | 1 create, 1 draw, 1 tess upload | all → `gfx_mesh_*` | — |
| `map.c` | 1 create, 1 destroy, 2 draw, 1 tess draw, ColorMask pair | all → `gfx_*` | — |
| `particle.c` | 1 `tesselator_draw` | via tesselator (no direct edit) | — |
| `model.c` / `model.h` | 14 displaylist + 2 `tesselator_glx` | rename `tesselator_glx`→`tesselator_gfx` only; still `glx_displaylist_*` | full step 5 |

### Sites left raw (justified)

| Site | Why |
|------|-----|
| `model.c` `glx_displaylist_*` | Step 5 (kv6 mesh + point sprites + lighting/fog coupling). Wrappers call `gfx_mesh_*` already. |
| `main.c` block-outline client arrays (`GL_LINES`) | Step 6 |
| `hud.c` `glColorMask` (non-collapsing) | Step 6 |
| `glx.c` fog / `glx_shader` | Step 5 |
| Texture/font shadow color query leftovers | Step 6 (from step 3) |

### `glx.c` non-draw utilities retained

- `glx_init` / `glx_version`
- `glx_fog` flag
- `glx_shader`
- `glx_enable_sphericalfog` / `glx_disable_sphericalfog`
- Thin `glx_displaylist_*` → `gfx_mesh_*` (model.c until step 5)

### Naming deviations vs §4 / step brief

- `gfx_mesh_t` is an embeddable complete struct (same fields as old `glx_displaylist`), not an incomplete opaque type — needed for by-value chunk/collapsing storage without heap indirection.
- Type enum uses `SHORT`/`FLOAT`/`POINTS` rather than embedding format in the mesh at create time — matches the existing create-flags + per-draw type parameter shape.
- `gfx_draw_arrays` name (not `gfx_mesh_draw_immediate`) for the transient path.
- `tesselator_glx` renamed to `tesselator_gfx` (takes `gfx_mesh_t*`).
- `glx_displaylist` is a `typedef` alias of `gfx_mesh_t` for model.h until step 5.

### `gfx.h` GL-type leak check

Confirmed: no `GLuint`/`GLenum`/`GLFW`/GLEW includes; case-insensitive `gl` outside `gfx_` prefix is empty.

---

## Phase 1b progress — Step 5: models (kv6) + fog

**Date:** 2026-07-14  
**Scope:** kv6 lighting/combine/point-sprite shader + mesh submission; spherical + EXP2 fog. Zero behavior change. No shader rewrite, no lighting value changes, no point-size “fixes”.

### API added

| API | Role |
|-----|------|
| `gfx_model_light` | `LIGHT0` ambient+diffuse (`kv6_calclight`) |
| `gfx_model_mesh_begin` / `texenv_color` / `mesh_end` | Exact combine+lighting+dummy bind / tint / teardown sequence |
| `gfx_model_points_begin_fixed` / `end_fixed` | `PointParameter` + `PointSize` + lighting stack |
| `gfx_model_points_begin_shader` / `end_shader` | Lazy kv6 program compile; uniforms `dist_factor`/`size`/`fog`/`camera`/`model`; `PROGRAM_POINT_SIZE` |
| `gfx_color3f` / `gfx_color3ub` | Point-path team/colorize tint |
| `gfx_multisample` | MSAA off/on around point draws |
| `gfx_fog_enable_exp2` / `gfx_fog_disable` | Smooth EXP2 fog (mode+density+color+enable) |
| `gfx_fog_enable_spherical` / `disable_spherical` | Former `glx_*_sphericalfog` bodies moved verbatim |
| `gfx_fog_active` | Reads `glx_fog` flag used by shader `dist_factor` |

Reused: `gfx_texture_2d`, `gfx_mesh_*`.

### Per-category conversion (model.c)

| Category | Approx. former sites | Result |
|----------|---------------------:|--------|
| Lighting | 2 | → `gfx_model_light` |
| Mesh combine / lighting | ~27 | → `gfx_model_mesh_*` + `gfx_texture_2d` |
| Point fixed-function | ~8 | → `gfx_model_points_*_fixed` |
| Point shader + uniforms | ~10 | → `gfx_model_points_*_shader` (source relocated) |
| Color / MSAA | ~6 | → `gfx_color*` / `gfx_multisample` |
| Submission | 14 `glx_displaylist_*` | → `gfx_mesh_*` |
| **Direct `gl*` left in model.c** | | **0** |

### Shader relocation

- Vertex + fragment source strings moved byte-identical into `gfx_gl.c` (`gfx_kv6_ensure_program`).
- Compile helper preserves prior `glx_shader` attach quirk (`if(vertex)` attaches fragment).
- Uniform names/meanings/update points unchanged; lazy init on first shader begin (was first point-mesh build).

### Fog conversion

| Site | Change |
|------|--------|
| `main.c` smooth fog | → `gfx_fog_enable_exp2(fog_color, 0.015F)` |
| `main.c` post-scene | → `gfx_fog_disable` |
| `main.c` spherical | → `gfx_fog_enable/disable_spherical` (incl. ES FPS toggles) |
| `glx.c` spherical bodies | → thin wrappers calling `gfx_fog_*_spherical` |

### Sites / leftovers (justified)

| Item | Why kept |
|------|----------|
| `glx_shader` body in `glx.c` | Unused after model move; `glx.h` not in allowlist — left for ABI |
| `glx_fog` / `glx_version` globals | Flag still owned in `glx.c`; spherical gfx path updates `glx_fog`; model still reads `glx_version` |
| `glx_displaylist` typedef in `model.h` | `model.h` not in allowlist; still alias of `gfx_mesh_t` |
| `glClearColor`/`glClear` / microui / hud | Step 6 |

### Naming deviations vs §4 / step brief

- Dedicated `gfx_model_mesh_*` / `gfx_model_points_*` rather than a single `GFX_PASS_MODELS` (interleaved tint/draw order needs mid-pass setters).
- Split fixed vs shader point begins (mirrors `#ifndef OPENGL_ES` / `glx_version` branches).
- `gfx_fog_enable_exp2(color, density)` packs mode+params+enable (matches single call site).
- `gfx_fog_active` instead of exporting `glx_fog` through `gfx.h`.
- `gfx_color3f`/`ub` naming matches step-3 granular style (`gfx_blend`), not audit `gfx_set_color*`.

### `gfx.h` GL-type leak check

Confirmed: no `GLuint`/`GLenum`/`GLFW`/GLEW includes; case-insensitive `gl` outside `gfx_` prefix is empty.

---

## Phase 1b progress — Step 6: final mop-up + full GL isolation

**Date:** 2026-07-14  
**Scope:** Every remaining raw GL call site outside `gfx_gl.c`. Mechanical wrap only. Zero behavior change. `gfx_gl.c` is the sole OpenGL owner in `src/`.

### API added

| API | Role |
|-----|------|
| `gfx_color4f` / `gfx_color4ub` / `gfx_get_color4f` | HUD/microui colors; CPU-tracked for font/texture shadows |
| `gfx_line_width` | HUD / block-outline widths |
| `gfx_draw_lines_2f` / `gfx_draw_lines_3s` | Replace `glBegin(GL_LINES)` / client-array outline |
| `gfx_depth_range_weapon` / `gfx_depth_range_reset` | Exact `0..0.05` / `0..1` FP weapon pair |
| `gfx_depth_test` / `gfx_depth_func_notequal` / `gfx_depth_func_lequal` | Network-stats outline technique |
| `gfx_scissor` / `gfx_scissor_off` | microui clip |
| `gfx_viewport` | HUD rotating-model viewport hack |
| `gfx_clear_color` / `gfx_clear` / `gfx_clear_color_only` | Frame clear |
| `gfx_shade_smooth` / `gfx_shade_flat` | AO / flat shading |
| `gfx_light0_position` | Per-frame light position |
| `gfx_capture_framebuffer` | Screenshot `RGBA8` readback (row flip stays in `main.c`) |
| `gfx_gl2` | Former `glx_version` flag (set in `gfx_init`) |

### Per-file conversion

| File | Approx. sites before | Result |
|------|---------------------:|--------|
| `hud.c` | ~140 | all → `gfx_*` (colors, blend, lines, depth range, netstat mask, viewport) |
| `main.c` | ~35 | shade/clear/light/outline/weapon depth/microui/screenshot → `gfx_*`; `glx_init` removed |
| `font.c` / `texture.c` | 3 each | shadows → `gfx_get_color4f` + `gfx_color4f` |
| `player.c` | 2 | nametag color → `gfx_color3ub` |
| `map.c` / `chunk.c` | commented only | comments scrubbed so isolation greps pass |
| `common.h` | GLEW/ES includes | moved into `gfx_gl.c` only |
| `glx.c` / `glx.h` | thin wrappers + unused shader | **deleted**; CMakeLists updated |
| `model.h` / `model.c` | `glx_displaylist` / `glx_version` | → `gfx_mesh_t` / `gfx_gl2()` |

### Deferred / Phase 2+ (justified)

| Item | Note |
|------|------|
| Screenshot path | Isolates readback only; Vulkan capture is a later concern |
| `settings.opengl14` name | Config flag name; not a GL call |
| GLFW may transitively include system GL headers via `glfw3.h` | Not present as a `GL/gl` string in `src/` sources; context still created by `gfx_*` |

---

## Phase 1 exit criteria

**Date:** 2026-07-14

### Isolation greps (verbatim)

**1.** `rg -n "GL/gl" src/ --glob "*.c" --glob "*.h"`

```
src/gfx_gl.c:26:#include <GL/glew.h>
```

(ES builds also allow `#include <SDL2/SDL_opengles.h>` inside `gfx_gl.c` only.)

**2.** `rg -n "\bgl[A-Z]" src/ --glob "*.c"` → matches **only** in `src/gfx_gl.c` (no other `.c` files).

**3.** `rg -in "gl" src/gfx.h` → only `gfx_`-prefixed identifiers (e.g. `gfx_gl2`).

### Final per-file GL call count

| File | Original (Phase 1a) | After Step 6 |
|------|--------------------:|-------------:|
| `hud.c` | 154 | **0** |
| `glx.c` | 110 | **deleted** |
| `texture.c` | 70 | **0** |
| `model.c` | 63 | **0** |
| `main.c` | 63 | **0** |
| `font.c` | 35 | **0** |
| `tesselator.c` | 12 | **0** |
| `map.c` | 10 | **0** |
| `player.c` | 7 | **0** |
| `matrix.c` | 5 | **0** |
| `chunk.c` | 3 | **0** |
| `window.c` | 0 direct | **0** |
| `gfx_gl.c` | (created in 1b) | **all remaining GL** |

**Phase 1 complete:** game code talks only to `gfx.h`; OpenGL lives solely in `gfx_gl.c`.

---

## Phase 2 — Vulkan backend bootstrap

**Branch:** `phase-2-vulkan-bootstrap`  
**Date:** 2026-07-14  
**Scope:** Second gfx backend that opens a window and presents a solid clear-color frame (`--vulkan`). No world rendering, no pipelines/draws beyond clear.

### Dispatch

- `src/gfx.c` — thin function-pointer table dispatcher (`gfx_ops_t` in `src/gfx_backend.h`).
- Table populated once by `gfx_select_backend()` (default GL).
- `src/gfx_gl.c` — GL implementation renamed to `gfx_gl_*`, registered as `gfx_gl_ops`.
- `src/gfx_vk.cpp` — C++20 Vulkan implementation + stubs, registered as `gfx_vk_ops`.

### gfx.h delta

| Addition | Role |
|----------|------|
| `gfx_backend_t` (`GFX_BACKEND_GL`, `GFX_BACKEND_VULKAN`) | Backend enum |
| `gfx_select_backend(gfx_backend_t)` | Call before window hints / create |
| `gfx_selected_backend(void)` | Query current selection |

All other public signatures unchanged. Flag: parse `--vulkan` in `main.c` before `window_init()`.

### Vulkan choices

| Item | Choice |
|------|--------|
| API | Prefer **Vulkan 1.3** + dynamic rendering; fallback **1.2** + minimal render pass / framebuffers |
| Present mode | `VK_PRESENT_MODE_FIFO_KHR` (vsync parity with GL default) |
| Format | Prefer **UNORM** (`B8G8R8A8_UNORM`, then `R8G8B8A8_UNORM`) for visual parity with GL’s non-sRGB default framebuffer *(Phase 3 changed from sRGB-first)* |
| Frames in flight | 2 (semaphore + fence per frame) |
| Clear color | Game-driven via `gfx_clear_color` (menu uses fog clear; UI covers full window) |
| Validation | On in Debug when `VK_LAYER_KHRONOS_validation` is available; override `BUTTERSPADES_VK_VALIDATION=0\|1`. Set `VK_LAYER_PATH` to `C:\msys64\mingw64\bin` under MSYS2. Prefer loader via PATH (do not copy only `vulkan-1.dll` beside the exe). |
| Deps (FetchContent) | vk-bootstrap **`v1.4.356`**, VMA **`v3.3.0`** |
| Window note | MinGW GLFW may return a NULL primary monitor after `GLFW_NO_API` create — `window_init` caches monitor mode beforehand. |

### Stub worklist (updated by Phase 4)

**Implemented in Phase 3 (2D/UI):** matrices (proj/MV/texture scale), `GFX_PASS_UI_2D`, textures (RGBA+ALPHA expand, upload/sub/filter/wrap/bind/destroy), `max_texture_size` / NPOT, `texture_2d`, blend, `draw_quads_2d` / `_short`, `draw_lines_2f`, color3/4 f/ub + get, scissor/viewport, clear_color / clear / clear_color_only.

**Implemented in Phase 4 (world):** depth attachment, GL→VK Z remap, `mesh_*`, `draw_arrays`, `draw_lines_3s`, world/pass pipelines, fog (spherical + EXP2), `color_mask` (all-on / all-off), `depth_range_weapon` / `reset`, deferred deletion (meshes + textures).

**Still stubbed (Phase 5+, log-once):**

| Area | Stubs |
|------|-------|
| Misc state | multisample, shade_*, light0 *(line_width stored/ignored; lines fixed 1.0)* |
| Depth overlay (HUD netstat) | `depth_test`, `depth_func_notequal` / `lequal` |
| Capture / capability | capture_framebuffer, gfx_gl2 (=0) |
| Models (kv6) | model_light, mesh begin/texenv/end, points fixed + shader |

Lifecycle implemented on Vulkan: `apply_context_hints`, `init`, `shutdown`, `resize`, `swap_buffers`, `set_vsync` (FIFO only).

---

## Phase 3 — Vulkan 2D/UI pipeline

**Branch:** `phase-3-vulkan-2d`  
**Date:** 2026-07-14  
**Scope:** Under `--vulkan`, main menu / settings / server list render and navigate with visual parity to GL. World/3D remains stubbed.

### Decisions

| Item | Choice |
|------|--------|
| Shader embed | `glslc` → `.spv` at build → `cmake/EmbedSpirv.cmake` hex → `shaders_embedded.c/h` linked into `client` |
| Quad strategy | No expansion — game already sends triangle lists |
| Descriptors | One combined-image-sampler set per texture; free-list pools; 4 samplers (nearest/linear × repeat/clamp); filter changes deferred to frame begin after fence wait |
| sRGB | **UNORM** swapchain + `R8G8B8A8_UNORM` textures (match GL) |
| Y flip | Negative viewport height only (same ortho matrices as GL); scissor converts GL bottom-left → VK top-left |
| ALPHA fonts | Expand A→`RGBA(255,255,255,A)` at upload |
| Untextured draws | Automatic 1×1 white texture when unbound / `texture_2d` off |
| `color_mask` | Remains stubbed (netstat / collapsing map only) |

### New / touched files

- `shaders/ui.vert`, `shaders/ui.frag`
- `cmake/EmbedSpirv.cmake`, `src/CMakeLists.txt` (glslc + embed)
- `src/gfx_vk.cpp` (UI pipelines, textures, batcher, lazy frame begin)
- Docs: this section; `BUILDING-WINDOWS.md` (shaderc package)

**Untouched:** `gfx.h`, `gfx.c`, `gfx_gl.c`, game code.

---

## Phase 4 — Vulkan world rendering

**Branch:** `phase-4-vulkan-world`  
**Date:** 2026-07-19  
**Scope:** Under `--vulkan`, joining a server renders terrain/water/fog/outline/damage/collapse/particles. Models/kv6 stay stubbed (players invisible). GL backend and game code untouched.

### Pass → pipeline table

| Pipeline | Topology | Depth test/write | Compare | Blend | Color write | Notes |
|----------|----------|------------------|---------|-------|-------------|-------|
| `world_opaque` | tris | on/on | LEQUAL | off | RGBA | `GFX_PASS_WORLD_3D` |
| `world_outline` | lines | off/off | — | off | RGBA | `GFX_PASS_BLOCK_OUTLINE` |
| `world_damaged` | tris | on/on | EQUAL | on | RGBA | `GFX_PASS_DAMAGED` |
| `world_collapse` | tris | on/on | LEQUAL | on | RGBA | collapsing, mask on |
| `world_collapse_depth` | tris | on/on | LEQUAL | off | none | collapsing depth pre-pass |
| `ui_nametag` | tris (UI verts) | off/off | — | on | RGBA | discard if `a ≤ 0.5` |
| existing `ui_*` | — | off | — | — | RGBA | `GFX_PASS_UI_2D` |

Weapon depth range: dynamic viewport `maxDepth` 0.05 / 1.0 (not a separate pipeline).

### Mesh layouts

| Layout | Pos | Color | Callers |
|--------|-----|-------|---------|
| A | short3 | ubyte4 | chunks |
| B | float3 | ubyte4 | collapsing, particles, damaged |
| C | short3 lines | current color | block outline |

Quads expanded to triangle lists at upload. Normals ignored (model-only). Persistent meshes: host-visible VMA buffers; **new buffer per `mesh_update`**, old retired (perf TODO: ring). Transient 3D: per-frame world VB.

### Fog

- **Spherical:** `f = clamp(length(xz - cam_xz) / render_distance, 0, 1)`; mix toward `fog_color`.
- **EXP2:** `f = exp(-(density * eyeDist)^2)`; `C = f*Cin + (1-f)*Cfog`.
- Model/fog params in a 112-byte std140 UBO; MVP remains a 64-byte push constant.

### Depth / clip

- Depth: `D32_SFLOAT` (fallback `D24_UNORM_S8_UINT`), recreated with swapchain.
- GL→VK Z: clip matrix in `rebuild_mvp` (`clip[10]=clip[14]=0.5`). Y: negative viewport height (Phase 3).

### Deferred deletion

Global `pending_retire` with `free_after_serial = frame_serial + frames_in_flight`. Covers mesh buffers, texture images/views, and descriptor sets (dsets freed before views). Fixes mid-frame font atlas rebuild validation errors.

### Validation

Live server (`aos://74.91.124.129:32000`) with `BUTTERSPADES_VK_VALIDATION=1`: ~90s play, **zero** `[vk] ERROR` lines after retire fix. Expected model stubs only.

### Phase 4.1 — mesh update path (2026-07-19)

**Symptoms:** intermittent missing chunks while moving; hitchy movement under `--vulkan` (absent on GL). Correlated with `gfx_mesh_update` storms (map load / block edits / rebuild waves).

**Instrumentation:** `BUTTERSPADES_VK_STATS=1` logs once/sec: upd/f, alloc/f, bytes/f, retire_max, same_frame_draw/f, inplace/f, pool hit rate, pool_bytes.

**Before (no pool):** map-load ~112 upd/f and ~112 alloc/f (~4 MB/f); remesh waves ~17 upd/f ≈ 17 alloc/f (~1.5 MB/f). `same_frame_draw ≈ upd` (new buffer bound same frame — Hypothesis B OK). `HOST_COHERENT=1` (Hypothesis C OK; flush retained as belt-and-suspenders). Retire flush already after fence wait (Hypothesis A ordering correct; documented in `RetireResource` comment).

**Fix:** size-bucketed 64 MB mesh buffer recycle pool + in-place rewrite when `bytes ≤ capacity` and `last_used_serial ≤ frame_serial - FIF`. Empty updates no longer destroy the GPU buffer.

**After:** remesh waves ~17 upd/f with **alloc/f → 0** and **pool_hit 88–100%**; steady-state alloc/f=0. ~2 min live, zero validation errors.

### New / touched files

- `shaders/world.vert`, `shaders/world.frag`, `shaders/ui_nametag.frag`
- `src/CMakeLists.txt` (shader list)
- `src/gfx_vk.cpp`
- Docs: this section; `BUILDING-WINDOWS.md` (shader list)

**Untouched:** `gfx.h`, `gfx.c`, `gfx_gl.c`, game code.

