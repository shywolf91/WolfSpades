include(FetchContent)

FetchContent_Declare(
	cglm
	GIT_REPOSITORY	https://github.com/recp/cglm
	GIT_TAG			v0.8.5
)

set(CGLM_STATIC ON CACHE INTERNAL "")
set(CGLM_USE_C99 ON CACHE INTERNAL "")
set(MIRRORS https://butter.penguins.win/mirror)
FetchContent_MakeAvailable(cglm)

FetchContent_Declare(
	vxl
	GIT_REPOSITORY	https://github.com/xtreme8000/libvxl
	GIT_TAG			origin/master
)

FetchContent_MakeAvailable(vxl)

if(NOT EMSCRIPTEN)
	FetchContent_Declare(
		vk-bootstrap
		GIT_REPOSITORY	https://github.com/charles-lunarg/vk-bootstrap
		GIT_TAG			v1.4.356
	)
	set(VK_BOOTSTRAP_TEST OFF CACHE BOOL "" FORCE)
	FetchContent_MakeAvailable(vk-bootstrap)

	FetchContent_Declare(
		VulkanMemoryAllocator
		GIT_REPOSITORY	https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator
		GIT_TAG			v3.3.0
	)
	FetchContent_MakeAvailable(VulkanMemoryAllocator)
endif()

macro(download_file_if_it_doesnt_exist filename url)
	set(argn ${ARGN})
	list(LENGTH argn no_fallback)
	if (NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${filename})
		message(STATUS "Downloading ${filename}")
		file(DOWNLOAD ${url} ${CMAKE_CURRENT_SOURCE_DIR}/${filename} STATUS stat)
		list(GET stat 0 success)
		list(GET stat 1 error)
		if (NOT ${success})
			message(STATUS "Downloaded ${filename} successfully")
		else()
			file(REMOVE ${CMAKE_CURRENT_SOURCE_DIR}/${filename})
			message(WARNING "Failed to download ${filename} (${error})")

			if(${no_fallback} EQUAL 0)
				foreach(mirror ${MIRRORS})
					string(SHA256 mirror_url_hash ${url})
					set(mirror_url ${mirror}/${mirror_url_hash})
					message(STATUS "Trying alternate URL ${mirror_url}")
					download_file_if_it_doesnt_exist(${filename} ${mirror_url} 1)
					if (EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${filename})
						# Success
						break()
					endif()
				endforeach()

				if (NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${filename})
					# Everything failed, exit
					message(FATAL_ERROR "Failed to download ${filename} from all sources.")
				endif()
			endif()
		endif()
	endif()
endmacro(download_file_if_it_doesnt_exist)

download_file_if_it_doesnt_exist(../bsresources.zip http://aos.party/bsresources.zip)

list(APPEND CLIENT_SOURCES aabb.c)
list(APPEND CLIENT_SOURCES camera.c)
list(APPEND CLIENT_SOURCES cameracontroller.c)
list(APPEND CLIENT_SOURCES chunk.c)
list(APPEND CLIENT_SOURCES config.c)
list(APPEND CLIENT_SOURCES dr_wav.c)
list(APPEND CLIENT_SOURCES file.c)
list(APPEND CLIENT_SOURCES font.c)
list(APPEND CLIENT_SOURCES gfx.c)
list(APPEND CLIENT_SOURCES gfx_gl.c)
if(EMSCRIPTEN)
	list(APPEND CLIENT_SOURCES gl_emscripten_stubs.c)
endif()
if(NOT EMSCRIPTEN)
	list(APPEND CLIENT_SOURCES gfx_vk.cpp)

	# --- Phase 3: compile UI shaders with glslc and embed SPIR-V as C arrays ---
	find_program(GLSLC glslc HINTS "$ENV{MSYSTEM_PREFIX}/bin" "C:/msys64/mingw64/bin")
	if(NOT GLSLC)
		message(FATAL_ERROR
			"glslc not found. Install mingw-w64-x86_64-shaderc (provides glslc) and reconfigure.")
	endif()
	message(STATUS "glslc: ${GLSLC}")

	set(SHADER_SRC_DIR ${BetterSpades_SOURCE_DIR}/shaders)
	set(SHADER_SPV_DIR ${CMAKE_BINARY_DIR}/shaders)
	set(SHADER_EMBED_C ${CMAKE_BINARY_DIR}/shaders_embedded.c)
	set(SHADER_EMBED_H ${CMAKE_BINARY_DIR}/shaders_embedded.h)
	file(MAKE_DIRECTORY ${SHADER_SPV_DIR})

	set(SHADER_SOURCES
		${SHADER_SRC_DIR}/ui.vert
		${SHADER_SRC_DIR}/ui.frag
		${SHADER_SRC_DIR}/ui_nametag.frag
		${SHADER_SRC_DIR}/world.vert
		${SHADER_SRC_DIR}/world.frag
	)
	set(SHADER_SPV_FILES "")
	# Explicit output names so EmbedSpirv symbols match (ui_vert_spv, world_vert_spv, …)
	set(SHADER_SPV_MAP
		${SHADER_SRC_DIR}/ui.vert|${SHADER_SPV_DIR}/ui_vert.spv
		${SHADER_SRC_DIR}/ui.frag|${SHADER_SPV_DIR}/ui_frag.spv
		${SHADER_SRC_DIR}/ui_nametag.frag|${SHADER_SPV_DIR}/ui_nametag_frag.spv
		${SHADER_SRC_DIR}/world.vert|${SHADER_SPV_DIR}/world_vert.spv
		${SHADER_SRC_DIR}/world.frag|${SHADER_SPV_DIR}/world_frag.spv
	)
	foreach(entry ${SHADER_SPV_MAP})
		string(REPLACE "|" ";" pair ${entry})
		list(GET pair 0 src)
		list(GET pair 1 spv)
		add_custom_command(
			OUTPUT ${spv}
			COMMAND ${GLSLC} -O -o ${spv} ${src}
			DEPENDS ${src}
			COMMENT "glslc ${spv}"
			VERBATIM
		)
		list(APPEND SHADER_SPV_FILES ${spv})
	endforeach()
	add_custom_command(
		OUTPUT ${SHADER_EMBED_C} ${SHADER_EMBED_H}
		COMMAND ${CMAKE_COMMAND}
			-DOUT_C=${SHADER_EMBED_C}
			-DOUT_H=${SHADER_EMBED_H}
			"-DSPV_FILES=${SHADER_SPV_FILES}"
			-P ${BetterSpades_SOURCE_DIR}/cmake/EmbedSpirv.cmake
		DEPENDS ${SHADER_SPV_FILES} ${BetterSpades_SOURCE_DIR}/cmake/EmbedSpirv.cmake
		COMMENT "Embedding SPIR-V shaders into shaders_embedded.c"
		VERBATIM
	)
	list(APPEND CLIENT_SOURCES ${SHADER_EMBED_C})
	# Ensure the embed outputs rebuild with the target
	set_source_files_properties(${SHADER_EMBED_C} PROPERTIES GENERATED TRUE)
endif()
list(APPEND CLIENT_SOURCES grenade.c)
list(APPEND CLIENT_SOURCES http.c)
list(APPEND CLIENT_SOURCES gmi.c)
list(APPEND CLIENT_SOURCES hud.c)
list(APPEND CLIENT_SOURCES ini.c)
list(APPEND CLIENT_SOURCES list.c)
list(APPEND CLIENT_SOURCES main.c)
list(APPEND CLIENT_SOURCES map.c)
list(APPEND CLIENT_SOURCES matrix.c)
list(APPEND CLIENT_SOURCES model.c)
list(APPEND CLIENT_SOURCES network.c)
if(EMSCRIPTEN)
	list(APPEND CLIENT_SOURCES net_transport_null.c)
else()
	list(APPEND CLIENT_SOURCES net_transport_enet.c)
endif()
list(APPEND CLIENT_SOURCES parson.c)
list(APPEND CLIENT_SOURCES particle.c)
list(APPEND CLIENT_SOURCES player.c)
list(APPEND CLIENT_SOURCES sound.c)
list(APPEND CLIENT_SOURCES stb_truetype.c)
list(APPEND CLIENT_SOURCES texture.c)
list(APPEND CLIENT_SOURCES tracer.c)
list(APPEND CLIENT_SOURCES weapon.c)
list(APPEND CLIENT_SOURCES window.c)
list(APPEND CLIENT_SOURCES utils.c)
list(APPEND CLIENT_SOURCES ping.c)
list(APPEND CLIENT_SOURCES log.c)
list(APPEND CLIENT_SOURCES minheap.c)
list(APPEND CLIENT_SOURCES hashtable.c)
list(APPEND CLIENT_SOURCES rpc.c)
list(APPEND CLIENT_SOURCES tesselator.c)
list(APPEND CLIENT_SOURCES microui.c)
list(APPEND CLIENT_SOURCES channel.c)
list(APPEND CLIENT_SOURCES entitysystem.c)
if(NOT EMSCRIPTEN)
	list(APPEND CLIENT_SOURCES ${BetterSpades_SOURCE_DIR}/resources/icon.rc)
endif()

add_executable(client ${CLIENT_SOURCES})

if(EMSCRIPTEN)
	set_target_properties(client PROPERTIES OUTPUT_NAME "rl")
endif()

if(CMAKE_BUILD_TYPE STREQUAL "Release")
	set_property(TARGET client PROPERTY WIN32_EXECUTABLE true)
	target_compile_definitions(client PRIVATE NDEBUG)
endif()

set(GIT_COMMIT_HASH "?")

execute_process(
  COMMAND git describe --always --dirty
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  OUTPUT_VARIABLE GIT_COMMIT_HASH
  OUTPUT_STRIP_TRAILING_WHITESPACE
)

include(CheckIPOSupported)
check_ipo_supported(RESULT lto_supported OUTPUT error)

if(NOT EMSCRIPTEN AND lto_supported AND CMAKE_BUILD_TYPE STREQUAL "Release")
	message(STATUS "IPO / LTO enabled")
	set_property(TARGET client PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
else()
	message(STATUS "IPO / LTO disabled for this build (supported=${lto_supported}, type=${CMAKE_BUILD_TYPE})")
endif()

target_compile_definitions(client PRIVATE GIT_COMMIT_HASH="${GIT_COMMIT_HASH}")
target_compile_definitions(client PRIVATE DR_WAV_IMPLEMENTATION)
target_compile_definitions(client PRIVATE LOG_USE_COLOR)

if(ENABLE_TOUCH)
	target_compile_definitions(client PRIVATE USE_TOUCH)
endif()
if(ENABLE_ANDROID_FILE)
	target_compile_definitions(client PRIVATE USE_ANDROID_FILE)
endif()

target_compile_definitions(client PRIVATE BETTERSPADES_MAJOR=${BETTERSPADES_MAJOR})
target_compile_definitions(client PRIVATE BETTERSPADES_MINOR=${BETTERSPADES_MINOR})
target_compile_definitions(client PRIVATE BETTERSPADES_PATCH=${BETTERSPADES_PATCH})
target_compile_definitions(client PRIVATE BETTERSPADES_VERSION="v${BETTERSPADES_MAJOR}.${BETTERSPADES_MINOR}.${BETTERSPADES_PATCH}")


set_target_properties(
	client PROPERTIES
	RUNTIME_OUTPUT_DIRECTORY ${BetterSpades_SOURCE_DIR}/build/BetterSpades
	C_STANDARD 99
	CXX_STANDARD 20
	CXX_STANDARD_REQUIRED ON
)

if(EMSCRIPTEN)
	set_target_properties(
		client PROPERTIES
		RUNTIME_OUTPUT_DIRECTORY ${BetterSpades_SOURCE_DIR}/build/web
	)
endif()

set(CMAKE_PREFIX_PATH  ${CMAKE_PREFIX_PATH} ${BetterSpades_SOURCE_DIR}/deps)
set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} ${BetterSpades_SOURCE_DIR}/cmake/Modules)
set(OpenGL_GL_PREFERENCE LEGACY)
if(ENABLE_GLFW)
	find_package(glfw3 REQUIRED)
	target_link_libraries(client glfw3::glfw3)
	target_compile_definitions(client PRIVATE USE_GLFW)
endif()
if(ENABLE_SDL)
	if(EMSCRIPTEN)
		target_compile_definitions(client PRIVATE USE_SDL)
	else()
		find_package(SDL2 REQUIRED)
		target_link_libraries(client ${SDL2_LIBRARY})
		target_compile_definitions(client PRIVATE USE_SDL)
	endif()
endif()
if(ENABLE_SOUND)
	find_package(OpenAL REQUIRED)
	target_link_libraries(client ${OPENAL_LIBRARY})
	target_compile_definitions(client PRIVATE USE_SOUND)
endif()

if(NOT EMSCRIPTEN)
	find_package(enet REQUIRED)
	find_package(deflate REQUIRED)
else()
	FetchContent_Declare(
		libdeflate
		GIT_REPOSITORY https://github.com/ebiggers/libdeflate
		GIT_TAG v1.22
	)
	set(LIBDEFLATE_BUILD_STATIC_LIB ON CACHE BOOL "" FORCE)
	set(LIBDEFLATE_BUILD_SHARED_LIB OFF CACHE BOOL "" FORCE)
	set(LIBDEFLATE_BUILD_GZIP OFF CACHE BOOL "" FORCE)
	FetchContent_MakeAvailable(libdeflate)
	add_library(deflate::deflate ALIAS libdeflate_static)
endif()
find_package(Threads REQUIRED)
if(NOT ENABLE_OPENGLES)
	if(NOT EMSCRIPTEN)
		find_package(OpenGL REQUIRED)
		find_package(GLEW REQUIRED)
		target_link_libraries(client GLEW::GLEW)
	endif()
else()
	if(NOT EMSCRIPTEN)
		find_package(OpenGL REQUIRED COMPONENTS EGL)
		target_link_libraries(client OpenGL::EGL)
	endif()
	target_compile_definitions(client PRIVATE OPENGL_ES)
endif()

if(NOT EMSCRIPTEN)
	# Vulkan loader + headers via CMake's FindVulkan (MSYS2: mingw-w64-x86_64-vulkan-loader / vulkan-headers).
	find_package(Vulkan REQUIRED)
	target_link_libraries(client Vulkan::Vulkan vk-bootstrap::vk-bootstrap)
	target_include_directories(client PRIVATE ${vulkanmemoryallocator_SOURCE_DIR}/include)
endif()
if(ENABLE_RPC)
	target_link_libraries(client discord-rpc stdc++)
	target_compile_definitions(client PRIVATE USE_RPC DISCORD_DISABLE_IO_THREAD)
endif()
if(DEFINED ENV{BUILD_NUMBER})
	target_compile_definitions(client PRIVATE JENKINS_BUILD="$ENV{BUILD_NUMBER}")
endif()


if(WIN32 AND NOT EMSCRIPTEN)
	target_compile_definitions(client PRIVATE LIBDEFLATE_STATIC)
endif()

if(EMSCRIPTEN)
	target_link_libraries(client ${CMAKE_THREAD_LIBS_INIT} deflate::deflate m cglm vxl)
	target_include_directories(client PRIVATE ${CMAKE_BINARY_DIR})
	# Ports must be enabled at compile-time so headers (SDL2/SDL.h) resolve.
	target_compile_options(client PRIVATE
		"SHELL:-sUSE_SDL=2"
		"SHELL:-sLEGACY_GL_EMULATION=1"
	)

	# Stage assets into MEMFS (mirrors aoscam rl.data layout intent).
	set(WEB_ASSET_ROOT ${CMAKE_BINARY_DIR}/web_assets)
	file(MAKE_DIRECTORY ${WEB_ASSET_ROOT})
	add_custom_command(
		TARGET client PRE_BUILD
		COMMAND ${CMAKE_COMMAND} -E copy_directory ${BetterSpades_SOURCE_DIR}/resources ${WEB_ASSET_ROOT}
		COMMAND ${CMAKE_COMMAND} -E tar xzf ${BetterSpades_SOURCE_DIR}/bsresources.zip
		WORKING_DIRECTORY ${WEB_ASSET_ROOT}
		COMMENT "Staging web assets for --preload-file"
		VERBATIM
	)

	target_link_options(client PRIVATE
		"SHELL:-sUSE_SDL=2"
		"SHELL:-sMIN_WEBGL_VERSION=2"
		"SHELL:-sMAX_WEBGL_VERSION=2"
		"SHELL:-sLEGACY_GL_EMULATION=1"
		"SHELL:-sALLOW_MEMORY_GROWTH=1"
		"SHELL:-sMODULARIZE=1"
		"SHELL:-sEXPORT_NAME=createRlModule"
		"SHELL:-sENVIRONMENT=web"
		"SHELL:-sFORCE_FILESYSTEM=1"
		"SHELL:--preload-file ${WEB_ASSET_ROOT}/fonts@/fonts"
		"SHELL:--preload-file ${WEB_ASSET_ROOT}/kv6@/kv6"
		"SHELL:--preload-file ${WEB_ASSET_ROOT}/png@/png"
		"SHELL:--preload-file ${WEB_ASSET_ROOT}/wav@/wav"
		"SHELL:--preload-file ${WEB_ASSET_ROOT}/config.ini@/config.ini"
	)

	# Produce rl.js / rl.wasm / rl.data next to a hand-written HTML loader.
	set_target_properties(client PROPERTIES
		SUFFIX ".js"
	)

	add_custom_command(
		TARGET client POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E copy_if_different
			${BetterSpades_SOURCE_DIR}/web/rl.html
			${BetterSpades_SOURCE_DIR}/build/web/rl.html
		COMMENT "Copying web/rl.html shell"
	)
else()
	target_link_libraries(client ${CMAKE_THREAD_LIBS_INIT} ${OPENGL_LIBRARIES} enet::enet deflate::deflate m cglm vxl)
	target_include_directories(client PRIVATE ${OPENAL_INCLUDE_DIRS} ${OPENGL_INCLUDE_DIR} ${OPENGL_EGL_INCLUDE_DIRS} ${CMAKE_BINARY_DIR})

	add_custom_command(
		TARGET client
		POST_BUILD
		COMMENT "Copying resources..."
		COMMAND ${CMAKE_COMMAND} -E copy_directory ${BetterSpades_SOURCE_DIR}/resources ${BetterSpades_SOURCE_DIR}/build/BetterSpades
	)

	add_custom_command(
		TARGET client
		POST_BUILD
		COMMENT "Extracting additional third-party assets..."
		DEPENDS ${BetterSpades_SOURCE_DIR}/bsresources.zip
		WORKING_DIRECTORY ${BetterSpades_SOURCE_DIR}/build/BetterSpades
		COMMAND ${CMAKE_COMMAND} -E tar xzf ${BetterSpades_SOURCE_DIR}/bsresources.zip
	)

	add_custom_command(
		TARGET client
		POST_BUILD
		COMMENT "Copying overrides..."
		COMMAND ${CMAKE_COMMAND} -E copy_directory ${BetterSpades_SOURCE_DIR}/resources.override ${BetterSpades_SOURCE_DIR}/build/BetterSpades
	)
endif()

if(NOT EMSCRIPTEN)
install(
	TARGETS client
	RUNTIME
	DESTINATION .
)


install(
	FILES "${CMAKE_BINARY_DIR}/BetterSpades/config.ini" "${CMAKE_BINARY_DIR}/BetterSpades/OpenAL32.dll"
	DESTINATION .
	PERMISSIONS	OWNER_EXECUTE OWNER_WRITE OWNER_READ
				GROUP_EXECUTE GROUP_READ
)

install(
	DIRECTORY "${CMAKE_BINARY_DIR}/BetterSpades/fonts"
	DESTINATION .
	FILES_MATCHING PATTERN "*.ttf"
)

install(
	DIRECTORY "${CMAKE_BINARY_DIR}/BetterSpades/kv6"
	DESTINATION .
	FILES_MATCHING PATTERN "*.kv6"
)

install(
	DIRECTORY "${CMAKE_BINARY_DIR}/BetterSpades/png"
	DESTINATION .
	FILES_MATCHING PATTERN "*.png"
)

install(
	DIRECTORY "${CMAKE_BINARY_DIR}/BetterSpades/wav"
	DESTINATION .
	FILES_MATCHING PATTERN "*.wav"
)

install(
	DIRECTORY
	DESTINATION "cache"
)

install(
	DIRECTORY
	DESTINATION "logs"
)

install(
	DIRECTORY
	DESTINATION "screenshots"
)

set(CPACK_GENERATOR NSIS)
set(CPACK_PACKAGE_NAME "BetterSpades")
set(CPACK_PACKAGE_VENDOR "github.com")
set(CPACK_PACKAGE_VERSION "v${BETTERSPADES_MAJOR}.${BETTERSPADES_MINOR}.${BETTERSPADES_PATCH}")
set(CPACK_PACKAGE_VERSION_MAJOR "${BETTERSPADES_MAJOR}")
set(CPACK_PACKAGE_VERSION_MINOR "${BETTERSPADES_MINOR}")
set(CPACK_PACKAGE_VERSION_PATCH "${BETTERSPADES_PATCH}")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "${CPACK_PACKAGE_NAME}")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_SOURCE_DIR}/LICENSE")
set(CPACK_STRIP_FILES TRUE)
set(CPACK_NSIS_MENU_LINKS "client" "BetterSpades")
set(CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL ON)
set(CPACK_PACKAGE_CHECKSUM SHA256)

INCLUDE(CPack)
endif() # NOT EMSCRIPTEN (install/cpack)
