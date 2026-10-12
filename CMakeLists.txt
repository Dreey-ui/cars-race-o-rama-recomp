cmake_minimum_required(VERSION 3.20)
project(CarsRecomp CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Caminhos do seu clone do XenonRecomp e da saida do recompilador
set(XENONRECOMP_DIR "C:/Users/Drey/XenonRecomp" CACHE PATH "Clone do XenonRecomp")
set(PPC_DIR "C:/DEB/cars/ppc" CACHE PATH "Pasta ppc gerada pelo XenonRecomp")
set(XENOSRECOMP_DIR "C:/Users/Drey/XenosRecomp" CACHE PATH "Clone do XenosRecomp (tradutor de shaders)")

file(GLOB PPC_SOURCES "${PPC_DIR}/ppc_recomp.*.cpp" "${PPC_DIR}/ppc_func_mapping.cpp")
# mftb -> timebase de 50 MHz do runtime (src/guest_timebase.h)
set_source_files_properties(${PPC_SOURCES} PROPERTIES
    COMPILE_OPTIONS "/FI${CMAKE_CURRENT_SOURCE_DIR}/src/guest_timebase.h")

add_executable(CarsRecomp
    src/main.cpp src/memory.cpp src/thread.cpp src/kobjects.cpp
    src/kernel_sync.cpp src/kernel_mem.cpp src/kernel_io.cpp src/kernel_misc.cpp src/kernel_xam.cpp src/kernel_gpu.cpp src/kernel_audio.cpp src/input.cpp src/stubs_gen.cpp
    src/game_hooks.cpp src/host_profiler.cpp src/xma.cpp src/settings.cpp src/session_log.cpp src/scene_assets.cpp
    src/d3d_profile.cpp src/d3d_profile_gen.cpp
    src/gpu/renderer.cpp src/gpu/shaders.cpp src/gpu/textures.cpp src/gpu/memwatch.cpp src/gpu/model_capture.cpp
    ${XENOSRECOMP_DIR}/XenosRecomp/shader_recompiler.cpp
    ${PPC_SOURCES})

target_include_directories(CarsRecomp PRIVATE
    src
    ${PPC_DIR}
    ${XENONRECOMP_DIR}/XenonUtils
    ${XENONRECOMP_DIR}/thirdparty/simde
    ${XENOSRECOMP_DIR}/XenosRecomp
    ${XENOSRECOMP_DIR}/thirdparty/fmt/include
    ${XENOSRECOMP_DIR}/thirdparty/xxHash
    ${XENOSRECOMP_DIR}/thirdparty/dxc-bin/inc)

# XenonUtils (leitor de XEX) e dependencias do repositorio do XenonRecomp
add_subdirectory(${XENONRECOMP_DIR}/thirdparty/disasm disasm_build EXCLUDE_FROM_ALL)
set(THIRDPARTY_ROOT ${XENONRECOMP_DIR}/thirdparty)
add_subdirectory(${XENONRECOMP_DIR}/XenonUtils XenonUtils_build EXCLUDE_FROM_ALL)
target_link_libraries(CarsRecomp PRIVATE XenonUtils d3d12 dxgi)
# Map file (CarsRecomp.map) para traduzir enderecos de crash em funcoes
target_link_options(CarsRecomp PRIVATE "/lldmap:${CMAKE_BINARY_DIR}/CarsRecomp.map")

# O recompilador de shaders do XenosRecomp e compilado aqui dentro, com um pch proprio
set_source_files_properties(${XENOSRECOMP_DIR}/XenosRecomp/shader_recompiler.cpp PROPERTIES
    COMPILE_OPTIONS "/FI${CMAKE_CURRENT_SOURCE_DIR}/src/gpu/xenos_recomp_pch.h;-Wno-switch;-Wno-null-arithmetic")

# XEX descriptografado embutido no exe (recurso RCDATA): o jogo nao precisa mais do default_dec.xex ao lado
set(CARS_XEX_PATH "C:/DEB/cars/default_dec.xex" CACHE FILEPATH "XEX descriptografado/descomprimido embutido no executavel")
configure_file(src/xex_image.rc.in ${CMAKE_BINARY_DIR}/xex_image.rc @ONLY)
target_sources(CarsRecomp PRIVATE ${CMAKE_BINARY_DIR}/xex_image.rc)
# Sem janela de console (dois cliques): o log vai para <exe>.log; main() continua sendo o ponto de entrada
set_target_properties(CarsRecomp PROPERTIES WIN32_EXECUTABLE TRUE)
target_link_options(CarsRecomp PRIVATE "/ENTRY:mainCRTStartup")

# FFmpeg (LGPL, BtbN FFmpeg-Builds n8.1) em third_party/ffmpeg: decodificador xma2 para o audio XMA
set(FFMPEG_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/ffmpeg" CACHE PATH "FFmpeg com include/ lib/ bin/")
target_include_directories(CarsRecomp PRIVATE ${FFMPEG_DIR}/include)
target_link_directories(CarsRecomp PRIVATE ${FFMPEG_DIR}/lib)

# dxcompiler.dll / dxil.dll (shaders em tempo de execucao) e as DLLs do FFmpeg ao lado do executavel
add_custom_command(TARGET CarsRecomp POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${XENOSRECOMP_DIR}/thirdparty/dxc-bin/bin/x64/dxcompiler.dll
        ${XENOSRECOMP_DIR}/thirdparty/dxc-bin/bin/x64/dxil.dll
        ${FFMPEG_DIR}/bin/avcodec-62.dll
        ${FFMPEG_DIR}/bin/avutil-60.dll
        ${FFMPEG_DIR}/bin/swresample-6.dll
        $<TARGET_FILE_DIR:CarsRecomp>)

if (CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    # o codigo gerado usa __builtin_debugtrap, alias e atributos do Clang
    target_compile_options(CarsRecomp PRIVATE -Wno-unused-variable -Wno-unused-but-set-variable -Wno-unused-label -msse4.1)
endif()
if (MSVC OR CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    target_compile_definitions(CarsRecomp PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS)
endif()
