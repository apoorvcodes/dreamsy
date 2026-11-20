# Dreamsy engine v2 (docs/engine.md §14).
#
#   cmake --preset debug && cmake --build --preset debug && ctest --preset debug
#
# Presets: debug (ASan + UBSan), release, web (Emscripten; needs `source ~/emsdk/emsdk_env.sh`),
# ios-sim / ios-device / android-arm64 (phones; see README.md "Phones" and platform/).
cmake_minimum_required(VERSION 3.22)
project(dreamsy_engine LANGUAGES C CXX)

if(APPLE)
  enable_language(OBJC)
endif()
# On iOS CMake makes every executable an app bundle by default; only the player is one (platform/ios).
if(IOS)
  set(CMAKE_MACOSX_BUNDLE OFF)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(DREAMSY_SANITIZE "Build everything with AddressSanitizer + UndefinedBehaviorSanitizer" OFF)
option(DREAMSY_BUILD_PLAYER "Build the windowed player (needs a graphics stack)" ON)
option(DREAMSY_BUILD_TESTS "Build the test runner" ON)

set(THIRD_PARTY ${CMAKE_CURRENT_SOURCE_DIR}/../third_party)

# Determinism (§14.4): no fused multiply-add contraction, no fast-math, anywhere.
add_compile_options(-ffp-contract=off -fno-fast-math)

# Luau raises script errors as C++ exceptions (§14.2). On the web they need native wasm exceptions,
# otherwise every Luau error() aborts the module.
if(EMSCRIPTEN)
  add_compile_options(-fwasm-exceptions)
  # Emscripten's default stack is 64 KB; Luau recursion and file buffers need more.
  add_link_options(-fwasm-exceptions -sSTACK_SIZE=1048576)
endif()

if(DREAMSY_SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

# ---------------------------------------------------------------- third-party

# Luau: compiler + VM only. Built with its own flags (it relies on C++ exceptions and RTTI).
set(LUAU_BUILD_CLI OFF CACHE BOOL "" FORCE)
set(LUAU_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(${THIRD_PARTY}/luau ${CMAKE_BINARY_DIR}/third_party/luau EXCLUDE_FROM_ALL)

# Box2D v3: library only.
set(BOX2D_SAMPLES OFF CACHE BOOL "" FORCE)
set(BOX2D_BENCHMARKS OFF CACHE BOOL "" FORCE)
set(BOX2D_DOCS OFF CACHE BOOL "" FORCE)
set(BOX2D_UNIT_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(${THIRD_PARTY}/box2d ${CMAKE_BINARY_DIR}/third_party/box2d EXCLUDE_FROM_ALL)

# yyjson (JSON) and miniz (zip import for tools only): plain C, built here.
add_library(yyjson STATIC ${THIRD_PARTY}/yyjson/yyjson.c)
target_include_directories(yyjson SYSTEM PUBLIC ${THIRD_PARTY}/yyjson)
add_library(miniz STATIC ${THIRD_PARTY}/miniz/miniz.c)
target_include_directories(miniz SYSTEM PUBLIC ${THIRD_PARTY}/miniz)
target_compile_definitions(miniz PUBLIC MINIZ_NO_TIME MINIZ_NO_STDIO)

# ---------------------------------------------------------------- engine code

# Warnings and style flags for code we write (not applied to third-party targets).
# Designated initializers that skip fields are intended (rule structs have defaults), so newer
# clang's -Wmissing-designated-field-initializers is turned off where it exists.
include(CheckCXXCompilerFlag)
check_cxx_compiler_flag(-Wno-missing-designated-field-initializers DREAMSY_HAS_WNO_DESIGNATED)
set(DREAMSY_EXTRA_WARNINGS "")
if(DREAMSY_HAS_WNO_DESIGNATED)
  set(DREAMSY_EXTRA_WARNINGS -Wno-missing-designated-field-initializers)
endif()
function(dreamsy_target_defaults target)
  target_compile_options(${target} PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion -fno-rtti ${DREAMSY_EXTRA_WARNINGS}>)
  target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
endfunction()

add_library(dreamsy_base STATIC
  src/base/arena.cpp
  src/base/log.cpp
  src/base/sha256.cpp)
dreamsy_target_defaults(dreamsy_base)
target_include_directories(dreamsy_base PUBLIC src)

# E0 toolchain checks; replaced by src/script (E2) and src/physics (E3).
add_library(dreamsy_dev STATIC src/dev/smoke.cpp)
dreamsy_target_defaults(dreamsy_dev)
target_link_libraries(dreamsy_dev PUBLIC dreamsy_base Luau.Compiler Luau.VM box2d::box2d)

# Packages (§3): Dreamsy packs, directories, the catalog, the validator.
# format/catalog.json and kits/*/kit.json are embedded at build time.
set(DREAMSY_CATALOG ${CMAKE_CURRENT_SOURCE_DIR}/../format/catalog.json)
set(DREAMSY_KITS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../kits)
file(GLOB DREAMSY_KIT_MANIFESTS CONFIGURE_DEPENDS ${DREAMSY_KITS_DIR}/*/kit.json)
set(DREAMSY_EMBEDDED_CPP ${CMAKE_BINARY_DIR}/generated/embedded.cpp)
add_custom_command(
  OUTPUT ${DREAMSY_EMBEDDED_CPP}
  COMMAND ${CMAKE_COMMAND} -DCATALOG=${DREAMSY_CATALOG} -DKITS_DIR=${DREAMSY_KITS_DIR} -DOUT=${DREAMSY_EMBEDDED_CPP}
          -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_data.cmake
  DEPENDS ${DREAMSY_CATALOG} ${DREAMSY_KIT_MANIFESTS} ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_data.cmake
  COMMENT "Embedding catalog.json and kit manifests")

add_library(dreamsy_pkg STATIC
  src/pkg/catalog.cpp
  src/pkg/files.cpp
  src/pkg/formats.cpp
  src/pkg/json.cpp
  src/pkg/manifest.cpp
  src/pkg/pack.cpp
  src/pkg/text.cpp
  src/pkg/validate.cpp
  ${DREAMSY_EMBEDDED_CPP})
dreamsy_target_defaults(dreamsy_pkg)
target_include_directories(dreamsy_pkg SYSTEM PRIVATE ${THIRD_PARTY}/tomlplusplus)
target_link_libraries(dreamsy_pkg PUBLIC dreamsy_base yyjson)

# Deterministic math (§14.4): musl's libm under the standard names, linked straight into every
# executable (dreamsy_link_dmath) so it wins over the platform libm for all engine code, Luau included.
set(DREAMSY_DMATH_DIR ${THIRD_PARTY}/musl-libm)
file(GLOB DREAMSY_DMATH_SOURCES ${DREAMSY_DMATH_DIR}/src/math/*.c)
add_library(dreamsy_dmath OBJECT ${DREAMSY_DMATH_SOURCES})
target_include_directories(dreamsy_dmath PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src/dmath/compat ${DREAMSY_DMATH_DIR}/src/internal ${DREAMSY_DMATH_DIR}/arch/generic)
target_compile_definitions(dreamsy_dmath PRIVATE "hidden=__attribute__((__visibility__(\"hidden\")))")
target_compile_options(dreamsy_dmath PRIVATE -U__FP_FAST_FMA -fno-builtin -w)
function(dreamsy_link_dmath target)
  target_sources(${target} PRIVATE $<TARGET_OBJECTS:dreamsy_dmath>)
endfunction()
# Luau must call these functions, not fold or replace them with builtins.
target_compile_options(Luau.VM PRIVATE -fno-builtin)

# Scripting (E2, §14.13). The prelude (src/script/prelude.luau, op numbers from ops.h) and the kit
# sources are embedded at build time.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(DREAMSY_PRELUDE_LUAU ${CMAKE_CURRENT_SOURCE_DIR}/src/script/prelude.luau)
set(DREAMSY_OPS_H ${CMAKE_CURRENT_SOURCE_DIR}/src/script/ops.h)
file(GLOB_RECURSE DREAMSY_KIT_SOURCES CONFIGURE_DEPENDS ${DREAMSY_KITS_DIR}/*.luau)
set(DREAMSY_PRELUDE_CPP ${CMAKE_BINARY_DIR}/generated/prelude.cpp)
set(DREAMSY_KITS_CPP ${CMAKE_BINARY_DIR}/generated/kits.cpp)
add_custom_command(
  OUTPUT ${DREAMSY_PRELUDE_CPP} ${DREAMSY_KITS_CPP}
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/cmake/gen_script_data.py ${DREAMSY_PRELUDE_LUAU} ${DREAMSY_OPS_H} ${DREAMSY_KITS_DIR} ${DREAMSY_PRELUDE_CPP} ${DREAMSY_KITS_CPP}
  DEPENDS ${DREAMSY_PRELUDE_LUAU} ${DREAMSY_OPS_H} ${DREAMSY_KIT_SOURCES} ${CMAKE_CURRENT_SOURCE_DIR}/cmake/gen_script_data.py
  COMMENT "Embedding the Luau prelude and kit sources")
add_library(dreamsy_script_data STATIC ${DREAMSY_PRELUDE_CPP})
target_include_directories(dreamsy_script_data PRIVATE src)

# Every kit compiled in: tools, tests and the type checker (players load kit packs instead).
add_library(dreamsy_kits_embedded STATIC src/script/embedded_kits.cpp ${DREAMSY_KITS_CPP})
dreamsy_target_defaults(dreamsy_kits_embedded)
target_link_libraries(dreamsy_kits_embedded PUBLIC dreamsy_script)

# One Dreamsy pack per kit (build/<preset>/kits/<kit>.dpk) for players.
set(DREAMSY_KIT_PACKS_DIR ${CMAKE_BINARY_DIR}/kits)
add_custom_command(
  OUTPUT ${DREAMSY_KIT_PACKS_DIR}/core.dpk
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/cmake/gen_kit_packs.py ${DREAMSY_KITS_DIR} ${DREAMSY_KIT_PACKS_DIR}
  DEPENDS ${DREAMSY_KIT_SOURCES} ${DREAMSY_KIT_MANIFESTS} ${CMAKE_CURRENT_SOURCE_DIR}/cmake/gen_kit_packs.py
  COMMENT "Packing kits (one .dpk per kit)")
add_custom_target(dreamsy_kit_packs DEPENDS ${DREAMSY_KIT_PACKS_DIR}/core.dpk)

# World (E3, §14.5): entities with 2.5D fields, scene tree, props, tile grids, tweens, particles.
add_library(dreamsy_world STATIC
  src/world/particles.cpp
  src/world/tilemap.cpp
  src/world/tween.cpp
  src/world/world.cpp)
dreamsy_target_defaults(dreamsy_world)
target_link_libraries(dreamsy_world PUBLIC dreamsy_pkg)

# Physics (E3): Box2D v3 in the ground plane + engine-handled height (z).
add_library(dreamsy_physics STATIC src/physics/physics2d.cpp)
dreamsy_target_defaults(dreamsy_physics)
target_link_libraries(dreamsy_physics PUBLIC dreamsy_world box2d::box2d)

# Input (E5): what scripts see of the player's input (InputState), keyboard / gamepad / touch
# mapping, gestures, the on-screen controls model. Platform-free (servers and tools link it too).
add_library(dreamsy_input STATIC
  src/input/gamepad.cpp
  src/input/input_state.cpp
  src/input/keyboard.cpp
  src/input/touch_controls.cpp
  src/input/usage.cpp)
dreamsy_target_defaults(dreamsy_input)
target_link_libraries(dreamsy_input PUBLIC dreamsy_base)

# Platform UI state (E5): ui.dialog / ui.choice state machine, toasts, UI entity layout and hit
# testing. Deterministic, no drawing (ui/overlay.cpp draws, in dreamsy_overlay).
add_library(dreamsy_ui STATIC
  src/ui/dialog.cpp
  src/ui/layout.cpp
  src/ui/toasts.cpp)
dreamsy_target_defaults(dreamsy_ui)
target_link_libraries(dreamsy_ui PUBLIC dreamsy_input dreamsy_world)

add_library(dreamsy_script STATIC
  src/script/host.cpp
  src/script/host_input.cpp
  src/script/host_world.cpp
  src/script/json_value.cpp
  src/script/kits.cpp
  src/script/require_path.cpp
  src/script/theme.cpp
  src/script/vm.cpp)
dreamsy_target_defaults(dreamsy_script)
target_link_libraries(dreamsy_script PUBLIC dreamsy_pkg dreamsy_physics dreamsy_ui Luau.Compiler Luau.VM PRIVATE dreamsy_script_data)

# Type checking (tools only): Luau's analyzer + the API definitions (format/dreamsy.d.luau with
# Entity props generated from format/catalog.json).
set(DREAMSY_DEFS ${CMAKE_CURRENT_SOURCE_DIR}/../format/dreamsy.d.luau)
set(DREAMSY_DEFS_CPP ${CMAKE_BINARY_DIR}/generated/defs.cpp)
add_custom_command(
  OUTPUT ${DREAMSY_DEFS_CPP}
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/cmake/gen_defs.py ${DREAMSY_DEFS} ${DREAMSY_CATALOG} ${DREAMSY_DEFS_CPP}
  DEPENDS ${DREAMSY_DEFS} ${DREAMSY_CATALOG} ${CMAKE_CURRENT_SOURCE_DIR}/cmake/gen_defs.py
  COMMENT "Building the Luau API definitions")
add_library(dreamsy_check STATIC src/check/check.cpp ${DREAMSY_DEFS_CPP})
target_include_directories(dreamsy_check PRIVATE src)
# Luau.Analysis uses RTTI and exceptions; this tool-only module is the boundary (see check.cpp).
target_compile_options(dreamsy_check PRIVATE -frtti)
target_link_libraries(dreamsy_check PUBLIC dreamsy_pkg PRIVATE dreamsy_script dreamsy_kits_embedded Luau.Analysis)

# Rendering, CPU side (E4): draw list, depth sorting, view math, image decoding, atlas, fonts,
# batching. No GPU, so it builds everywhere (tests, headless tools); the sokol renderer is below.
# stb_image / stb_rect_pack / stb_truetype / nanosvg compile in one C file, warnings off (vendored).
add_library(dreamsy_media_libs STATIC src/render/third_party_impl.c)
target_include_directories(dreamsy_media_libs SYSTEM PUBLIC ${THIRD_PARTY}/stb ${THIRD_PARTY}/nanosvg)
target_compile_options(dreamsy_media_libs PRIVATE -w)

# The bundled theme fonts (third_party/fonts), embedded as byte arrays. Order matters:
# pixel, rounded, bold, serif, then the symbol fallback (render/font.cpp bundled_font).
set(DREAMSY_FONT_FILES
  ${THIRD_PARTY}/fonts/dejavu/DejaVuSansMono-Bold-Latin1.ttf
  ${THIRD_PARTY}/fonts/nunito/Nunito-ExtraBold-Latin1.ttf
  ${THIRD_PARTY}/fonts/inter/Inter-Bold-Latin1.ttf
  ${THIRD_PARTY}/fonts/source-serif-4/SourceSerif4-Bold-Latin1.ttf
  ${THIRD_PARTY}/fonts/dejavu/DejaVuSans-Bold-Symbols.ttf)
set(DREAMSY_FONTS_CPP ${CMAKE_BINARY_DIR}/generated/fonts.cpp)
string(JOIN "|" DREAMSY_FONT_FILES_ARG ${DREAMSY_FONT_FILES})  # "|": a ";" list would split the argument
add_custom_command(
  OUTPUT ${DREAMSY_FONTS_CPP}
  COMMAND ${CMAKE_COMMAND} "-DFILES=${DREAMSY_FONT_FILES_ARG}" -DNAMESPACE=dreamsy::render::embedded
          -DOUT=${DREAMSY_FONTS_CPP} -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_files.cmake
  DEPENDS ${DREAMSY_FONT_FILES} ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_files.cmake
  COMMENT "Embedding the bundled fonts"
  VERBATIM)

add_library(dreamsy_render STATIC
  src/render/atlas.cpp
  src/render/font.cpp
  src/render/geometry.cpp
  src/render/image.cpp
  src/render/sort.cpp
  src/render/style.cpp
  src/render/textures.cpp
  src/render/view.cpp
  ${DREAMSY_FONTS_CPP})
dreamsy_target_defaults(dreamsy_render)
target_link_libraries(dreamsy_render PUBLIC dreamsy_pkg PRIVATE dreamsy_media_libs)

# Platform UI drawing (E5): UI entities, toasts, dialogs, touch controls, pause button -> draw list.
add_library(dreamsy_overlay STATIC src/ui/overlay.cpp src/ui/shell.cpp)
dreamsy_target_defaults(dreamsy_overlay)
target_link_libraries(dreamsy_overlay PUBLIC dreamsy_ui dreamsy_render dreamsy_script)

add_library(dreamsy_zip_import STATIC src/pkg/zip_import.cpp)
dreamsy_target_defaults(dreamsy_zip_import)
target_link_libraries(dreamsy_zip_import PUBLIC dreamsy_pkg PRIVATE miniz)

add_executable(dreamsy-tool apps/tool/main.cpp apps/tool/playtest.cpp)
dreamsy_target_defaults(dreamsy-tool)
target_link_libraries(dreamsy-tool PRIVATE dreamsy_dev dreamsy_zip_import dreamsy_script dreamsy_kits_embedded dreamsy_check)
dreamsy_link_dmath(dreamsy-tool)
if(EMSCRIPTEN)
  target_link_options(dreamsy-tool PRIVATE -sENVIRONMENT=node -sALLOW_MEMORY_GROWTH=1 -sNODERAWFS=1)
endif()

# ---------------------------------------------------------------- player (sokol)

if(DREAMSY_BUILD_PLAYER)
  if(APPLE)
    set(SOKOL_IMPL_SRC src/platform/sokol_impl.m)
    set(SOKOL_BACKEND SOKOL_METAL)
  elseif(EMSCRIPTEN OR ANDROID)
    set(SOKOL_IMPL_SRC src/platform/sokol_impl.c)
    set(SOKOL_BACKEND SOKOL_GLES3)
  else()
    set(SOKOL_IMPL_SRC src/platform/sokol_impl.c)
    set(SOKOL_BACKEND SOKOL_GLCORE)
  endif()

  add_library(dreamsy_sokol STATIC ${SOKOL_IMPL_SRC})
  target_include_directories(dreamsy_sokol PUBLIC ${THIRD_PARTY}/sokol)
  target_compile_definitions(dreamsy_sokol PUBLIC ${SOKOL_BACKEND})
  if(IOS)
    target_compile_options(dreamsy_sokol PRIVATE -fobjc-arc)
    target_link_libraries(dreamsy_sokol PUBLIC "-framework Foundation" "-framework UIKit"
      "-framework QuartzCore" "-framework CoreGraphics" "-framework Metal" "-framework MetalKit")
  elseif(APPLE)
    target_compile_options(dreamsy_sokol PRIVATE -fobjc-arc)
    target_link_libraries(dreamsy_sokol PUBLIC
      "-framework Cocoa" "-framework QuartzCore" "-framework Metal" "-framework MetalKit")
  elseif(ANDROID)
    target_link_libraries(dreamsy_sokol PUBLIC GLESv3 EGL android log)
  elseif(EMSCRIPTEN)
    # em_library_funcs.json works around an Emscripten bug (seen in 4.0.11 and 6.0.13): with
    # -fwasm-exceptions and -O1 or higher, sokol_app's `withStackSave` fails to link with
    # "undefined symbol: emscripten_stack_get_current". Listing the stack helpers explicitly fixes it.
    target_link_options(dreamsy_sokol PUBLIC -sUSE_WEBGL2=1 -sMIN_WEBGL_VERSION=2 -sMAX_WEBGL_VERSION=2
      -sDEFAULT_LIBRARY_FUNCS_TO_INCLUDE=@${CMAKE_CURRENT_SOURCE_DIR}/src/platform/web/em_library_funcs.json)
  else()
    find_package(X11 REQUIRED)
    find_package(OpenGL REQUIRED)
    target_link_libraries(dreamsy_sokol PUBLIC X11::X11 X11::Xi X11::Xcursor OpenGL::GL dl m pthread)
  endif()

  # The GPU renderer (E4): sokol_gfx, hand-written Metal / GLSL ES 3.0 shaders (render/renderer.cpp).
  # World -> draw list for running games (play::draw_world).
  add_library(dreamsy_play STATIC src/play/platform_ui.cpp src/play/scene_draw.cpp)
  dreamsy_target_defaults(dreamsy_play)
  target_link_libraries(dreamsy_play PUBLIC dreamsy_script dreamsy_render dreamsy_overlay)

  # dreamsy-bench: CPU time per frame by stage, no GPU (README "Performance"); on the web it runs under Node.
  if(NOT IOS AND NOT ANDROID)
    add_executable(dreamsy-bench apps/bench/main.cpp)
    dreamsy_target_defaults(dreamsy-bench)
    target_link_libraries(dreamsy-bench PRIVATE dreamsy_play dreamsy_kits_embedded)
    target_compile_definitions(dreamsy-bench PRIVATE
      DREAMSY_BENCH_DEFAULT_PACKAGE="${CMAKE_CURRENT_SOURCE_DIR}/tests/bench/stress")
    dreamsy_link_dmath(dreamsy-bench)
    if(EMSCRIPTEN)
      target_link_options(dreamsy-bench PRIVATE -sENVIRONMENT=node -sALLOW_MEMORY_GROWTH=1 -sNODERAWFS=1)
    endif()
  endif()

  add_library(dreamsy_render_gpu STATIC src/render/renderer.cpp)
  dreamsy_target_defaults(dreamsy_render_gpu)
  target_link_libraries(dreamsy_render_gpu PUBLIC dreamsy_render dreamsy_sokol)

  # The demo package the player shows until it runs games (E6): native reads it from the repo,
  # the web build embeds it at /pkg.
  set(DREAMSY_DEMO_PACKAGE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../games/examples/feature-demo CACHE PATH
      "Package the player runs by default (the web build embeds it at /pkg)")
  # Web only: also embed every kit pack at /kits, so kit games run before games load by URL (E6).
  option(DREAMSY_WEB_EMBED_KITS "Embed all kit packs in the web player (for testing kit games)" OFF)
  if(ANDROID)
    # Android loads the player as libdreamsy-player.so from a NativeActivity (platform/android).
    add_library(dreamsy-player SHARED apps/player/main.cpp apps/player/demo_scene.cpp)
  else()
    add_executable(dreamsy-player apps/player/main.cpp apps/player/demo_scene.cpp)
  endif()
  dreamsy_target_defaults(dreamsy-player)
  target_link_libraries(dreamsy-player PRIVATE dreamsy_render_gpu dreamsy_play dreamsy_zip_import)
  add_dependencies(dreamsy-player dreamsy_kit_packs)
  dreamsy_link_dmath(dreamsy-player)
  if(EMSCRIPTEN)
    set_target_properties(dreamsy-player PROPERTIES SUFFIX ".html")
    target_link_options(dreamsy-player PRIVATE
      --shell-file ${CMAKE_CURRENT_SOURCE_DIR}/apps/player/shell.html -sALLOW_MEMORY_GROWTH=1
      --pre-js ${CMAKE_CURRENT_SOURCE_DIR}/apps/player/embed.js
      "SHELL:--pre-js ${CMAKE_CURRENT_SOURCE_DIR}/apps/player/audio.js"
      --embed-file ${DREAMSY_DEMO_PACKAGE_DIR}@/pkg)
    if(DREAMSY_WEB_EMBED_KITS)
      # SHELL: keeps CMake from merging the repeated --embed-file flag with the one above
      target_link_options(dreamsy-player PRIVATE "SHELL:--embed-file ${DREAMSY_KIT_PACKS_DIR}@/kits")
    endif()
    set_property(TARGET dreamsy-player APPEND PROPERTY LINK_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/apps/player/shell.html
                                                                    ${CMAKE_CURRENT_SOURCE_DIR}/apps/player/embed.js
                                                                    ${CMAKE_CURRENT_SOURCE_DIR}/apps/player/audio.js)
  elseif(IOS)
    include(platform/ios/ios.cmake)
    dreamsy_ios_app(dreamsy-player)
    # TODO(E9): bundle the demo package into the app; until then the player falls back to its built-in scene path.
    target_compile_definitions(dreamsy-player PRIVATE DREAMSY_DEMO_PACKAGE="${DREAMSY_DEMO_PACKAGE_DIR}"
                                                      DREAMSY_KIT_PACKS_DIR="${DREAMSY_KIT_PACKS_DIR}")
  elseif(ANDROID)
    # sokol_app defines the NativeActivity entry point; nothing references it, so keep it explicitly.
    target_link_options(dreamsy-player PRIVATE -u ANativeActivity_onCreate)
  else()
    target_compile_definitions(dreamsy-player PRIVATE DREAMSY_DEMO_PACKAGE="${DREAMSY_DEMO_PACKAGE_DIR}"
                                                      DREAMSY_KIT_PACKS_DIR="${DREAMSY_KIT_PACKS_DIR}")
  endif()
endif()

# Favor size for the web package loaders and Luau compiler. Keep the release speed
# optimization for the VM, simulation and rendering that run every frame.
if(EMSCRIPTEN)
  foreach(target IN ITEMS dreamsy_pkg dreamsy_zip_import miniz Luau.Ast Luau.Bytecode Luau.Compiler)
    target_compile_options(${target} PRIVATE $<$<CONFIG:Release>:-Oz>)
  endforeach()
endif()

# ---------------------------------------------------------------- tests

if(DREAMSY_BUILD_TESTS)
  enable_testing()
  # Ports of engine-web/test: tests/kits/*_test.cpp (one file per kit, plus shared helpers in
  # tests/kits/kit_test.*) and tests/api/*_test.cpp (Luau API behavior, every game headless).
  # DREAMSY_TESTS_ONLY="tycoon;games" builds just those files from either folder (faster iteration).
  file(GLOB DREAMSY_PORTED_TESTS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tests/kits/*_test.cpp
       ${CMAKE_CURRENT_SOURCE_DIR}/tests/api/*_test.cpp)
  set(DREAMSY_TESTS_ONLY "" CACHE STRING "Build only these tests/{kits,api}/<name>_test.cpp files (;-list, empty = all)")
  if(DREAMSY_TESTS_ONLY)
    set(DREAMSY_PORTED_TESTS "")
    foreach(name ${DREAMSY_TESTS_ONLY})
      file(GLOB found ${CMAKE_CURRENT_SOURCE_DIR}/tests/kits/${name}_test.cpp ${CMAKE_CURRENT_SOURCE_DIR}/tests/api/${name}_test.cpp)
      list(APPEND DREAMSY_PORTED_TESTS ${found})
    endforeach()
  endif()
  add_executable(dreamsy-tests tests/main.cpp tests/base_test.cpp tests/smoke_test.cpp tests/pkg_test.cpp tests/pack_test.cpp tests/script_test.cpp tests/dmath_test.cpp
    tests/check_test.cpp tests/render_test.cpp tests/input_test.cpp tests/world_test.cpp tests/physics_test.cpp tests/host_world_test.cpp
    tests/kits/kit_test.cpp ${DREAMSY_PORTED_TESTS})
  target_include_directories(dreamsy-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
  dreamsy_target_defaults(dreamsy-tests)
  # doctest needs RTTI; tests are not engine code.
  target_compile_options(dreamsy-tests PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-frtti>)
  # UBSan's vptr check only works when every translation unit has RTTI. Standard-library templates
  # with virtual internals (std::function, shared_ptr control blocks) are shared between these RTTI
  # tests and -fno-rtti engine code, which gives false "invalid vptr" reports; turn that one check off.
  if(DREAMSY_SANITIZE)
    target_compile_options(dreamsy-tests PRIVATE -fno-sanitize=vptr)
  endif()
  target_include_directories(dreamsy-tests PRIVATE ${THIRD_PARTY}/doctest)
  target_link_libraries(dreamsy-tests PRIVATE dreamsy_dev dreamsy_zip_import dreamsy_script dreamsy_kits_embedded dreamsy_check dreamsy_render dreamsy_overlay miniz)
  dreamsy_link_dmath(dreamsy-tests)
  target_compile_definitions(dreamsy-tests PRIVATE DREAMSY_REPO_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/..")
  # Two ctest entries over the one binary (they run in parallel with ctest -j):
  #   dreamsy-tests       everything except the doctest suites "slow" and "engine-gap"
  #   dreamsy-tests-slow  the "slow" suite: kit balance sims and long scripted runs (label "slow";
  #                       skip with ctest -LE slow)
  # "engine-gap" holds ported engine-web tests the C++ engine doesn't pass yet (run them with
  # dreamsy-tests -ts=engine-gap); see the comment on each.
  set(DREAMSY_TEST_ARGS --test-suite-exclude=slow,engine-gap)
  set(DREAMSY_SLOW_TEST_ARGS --test-suite=slow)
  if(EMSCRIPTEN)
    target_link_options(dreamsy-tests PRIVATE -sENVIRONMENT=node -sALLOW_MEMORY_GROWTH=1 -sNODERAWFS=1)
    add_test(NAME dreamsy-tests COMMAND node $<TARGET_FILE:dreamsy-tests> ${DREAMSY_TEST_ARGS})
    add_test(NAME dreamsy-tests-slow COMMAND node $<TARGET_FILE:dreamsy-tests> ${DREAMSY_SLOW_TEST_ARGS})
  elseif(IOS)
    # A plain command-line binary run inside the booted simulator (it can read the host's files).
    add_test(NAME dreamsy-tests COMMAND xcrun simctl spawn booted $<TARGET_FILE:dreamsy-tests> ${DREAMSY_TEST_ARGS})
    add_test(NAME dreamsy-tests-slow COMMAND xcrun simctl spawn booted $<TARGET_FILE:dreamsy-tests> ${DREAMSY_SLOW_TEST_ARGS})
  elseif(ANDROID)
    # Push and run by hand: adb push dreamsy-tests /data/local/tmp (tests needing repo files will fail).
  else()
    add_test(NAME dreamsy-tests COMMAND dreamsy-tests ${DREAMSY_TEST_ARGS})
    add_test(NAME dreamsy-tests-slow COMMAND dreamsy-tests ${DREAMSY_SLOW_TEST_ARGS})
  endif()
  if(TEST dreamsy-tests-slow)
    set_tests_properties(dreamsy-tests-slow PROPERTIES LABELS slow)
  endif()
endif()

# ---------------------------------------------------------------- fuzzing (§14.9)
#
# Each target in fuzz/ builds two ways:
#   fuzz-<name>-replay  runs a corpus (+ random mutations) with any compiler; ctest runs it
#   fuzz-<name>         libFuzzer, coverage-guided (DREAMSY_FUZZ=ON, upstream clang)
option(DREAMSY_FUZZ "Build libFuzzer targets (needs clang with -fsanitize=fuzzer)" OFF)

if(NOT EMSCRIPTEN AND NOT IOS AND NOT ANDROID)
  set(DREAMSY_FUZZ_TARGETS pack package zip_import)
  foreach(name IN LISTS DREAMSY_FUZZ_TARGETS)
    add_executable(fuzz-${name}-replay fuzz/fuzz_${name}.cpp fuzz/replay_main.cpp)
    dreamsy_target_defaults(fuzz-${name}-replay)
    target_link_libraries(fuzz-${name}-replay PRIVATE dreamsy_zip_import)
    dreamsy_link_dmath(fuzz-${name}-replay)
    if(DREAMSY_FUZZ)
      add_executable(fuzz-${name} fuzz/fuzz_${name}.cpp)
      dreamsy_target_defaults(fuzz-${name})
      target_compile_options(fuzz-${name} PRIVATE -fsanitize=fuzzer)
      target_link_options(fuzz-${name} PRIVATE -fsanitize=fuzzer)
      target_link_libraries(fuzz-${name} PRIVATE dreamsy_zip_import)
    endif()
  endforeach()

  find_package(Python3 COMPONENTS Interpreter)
  if(DREAMSY_BUILD_TESTS AND Python3_FOUND)
    set(DREAMSY_CORPUS ${CMAKE_BINARY_DIR}/fuzz-corpus)
    add_test(NAME fuzz-corpus
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/fuzz/make_corpus.py ${DREAMSY_CORPUS} $<TARGET_FILE:dreamsy-tool>)
    set_tests_properties(fuzz-corpus PROPERTIES FIXTURES_SETUP fuzz_corpus)
    foreach(name pack package zip_import)
      set(corpus_dir ${name})
      if(name STREQUAL "zip_import")
        set(corpus_dir zip)
      endif()
      add_test(NAME fuzz-${name} COMMAND fuzz-${name}-replay --mutate 100 ${DREAMSY_CORPUS}/${corpus_dir})
      set_tests_properties(fuzz-${name} PROPERTIES FIXTURES_REQUIRED fuzz_corpus)
      # Inputs that once crashed (fuzz/regressions/<target>/): must stay fixed.
      add_test(NAME fuzz-${name}-regressions COMMAND fuzz-${name}-replay ${CMAKE_CURRENT_SOURCE_DIR}/fuzz/regressions/${name})
    endforeach()
  endif()
endif()
