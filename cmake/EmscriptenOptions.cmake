# Root CMakeLists.txt — force SDL + GLES when targeting Emscripten
if(EMSCRIPTEN)
	set(ENABLE_SDL ON CACHE BOOL "Build against SDL backend" FORCE)
	set(ENABLE_GLFW OFF CACHE BOOL "Build against GLFW3 backend" FORCE)
	# Use desktop GL + LEGACY_GL_EMULATION (not GLES headers) for fixed-function AoS client.
	set(ENABLE_OPENGLES OFF CACHE BOOL "Build for OpenGL ES" FORCE)
	set(ENABLE_SOUND OFF CACHE BOOL "Enable sound support using OpenAL" FORCE)
	set(ENABLE_RPC OFF CACHE BOOL "Enable Discord Rich Presence support" FORCE)
	set(ENABLE_TOUCH OFF CACHE BOOL "" FORCE)
	set(ENABLE_ANDROID_FILE OFF CACHE BOOL "" FORCE)
endif()
