# Copyright (C) 2023 Advanced Micro Devices, Inc.  All rights reserved.
# SPDX-License-Identifier: MIT
cmake_minimum_required(VERSION 3.16)
set(APP_NAME minicast)
project(${APP_NAME})

enable_language(C ASM CXX)

add_link_options("-Wl,--build-id=sha1")

set(_sources
    ${CMAKE_SOURCE_DIR}/../main.cpp
)

set(swrl ${CMAKE_SOURCE_DIR}/../libswirl)
list(APPEND _sources
    ${swrl}/stdclass.cpp
    ${swrl}/reios/reios.cpp
    ${swrl}/reios/descrambl.cpp
    ${swrl}/reios/reios_elf.cpp
    ${swrl}/reios/gdrom_hle.cpp

    # ${swrl}/linux-dist/emscripten.cpp
    # ${swrl}/linux-dist/main.cpp
    # ${swrl}/linux-dist/joystick.cpp
    # ${swrl}/linux-dist/dispmanx.cpp
    # ${swrl}/linux-dist/evdev.cpp
    # ${swrl}/linux-dist/x11.cpp

    # ${swrl}/oslib/audiobackend_xaudio.cpp
    # ${swrl}/oslib/audiobackend_sdl2.cpp
    # ${swrl}/oslib/audiobackend_pulseaudio.cpp
    # ${swrl}/oslib/audiobackend_libretro.cpp
    # ${swrl}/oslib/audiobackend_directsound.cpp
    ${swrl}/oslib/audiostream.cpp
    # ${swrl}/oslib/posix/threading.cpp
    # ${swrl}/oslib/audiobackend_oss.cpp
    # ${swrl}/oslib/audiobackend_coreaudio.cpp
    # ${swrl}/oslib/audiobackend_libao.cpp
    # ${swrl}/oslib/audiobackend_alsa.cpp
    # ${swrl}/oslib/windows/threading.cpp
    # ${swrl}/oslib/audiobackend_omx.cpp
    ${swrl}/hw/arm7/virt_arm.cpp
    ${swrl}/hw/arm7/arm7.cpp
    # ${swrl}/hw/arm7/arm7_jit_virt_arm64.cpp
    # ${swrl}/hw/arm7/arm7_jit_virt_x86.cpp
    ${swrl}/hw/arm7/arm7_jit_virt.cpp
    ${swrl}/hw/arm7/arm7_jit_virt_arm32.cpp
    ${swrl}/hw/arm7/SoundCPU.cpp
    ${swrl}/hw/arm7/arm7_interpreter.cpp
    # ${swrl}/hw/modem/modem.cpp
    # ${swrl}/hw/modem/dns.cpp
    # ${swrl}/hw/modem/picoppp.cpp
    ${swrl}/hw/pvr/spg.cpp
    ${swrl}/rend/mister_rend/mister_support.cpp
    ${swrl}/hw/pvr/pvr_sb_regs.cpp
    # ${swrl}/hw/pvr/Renderer_if.cpp
    ${swrl}/hw/pvr/pvr_mem.cpp
    ${swrl}/hw/pvr/pvr_yuv.cpp
    ${swrl}/hw/pvr/pvr_regs.cpp
    # ${swrl}/hw/pvr/ta_ctx.cpp
    # ${swrl}/hw/pvr/ta_vtx.cpp
    ${swrl}/hw/pvr/ta.cpp
    # ${swrl}/hw/naomi/m4cartridge.cpp
    # ${swrl}/hw/naomi/decrypt.cpp
    # ${swrl}/hw/naomi/naomi_cart.cpp
    # ${swrl}/hw/naomi/gdcartridge.cpp
    # ${swrl}/hw/naomi/m1cartridge.cpp
    # ${swrl}/hw/naomi/awcartridge.cpp
    # ${swrl}/hw/naomi/naomi.cpp
    ${swrl}/hw/aica/dsp.cpp
    # ${swrl}/hw/aica/dsp_arm64.cpp
    ${swrl}/hw/aica/sgc_if.cpp
    ${swrl}/hw/aica/aica_mmio.cpp
    ${swrl}/hw/aica/aica.cpp
    ${swrl}/hw/aica/dsp_interp.cpp
    ${swrl}/hw/aica/dsp_helpers.cpp
    # ${swrl}/hw/aica/dsp_x86.cpp
    ${swrl}/hw/gdrom/gdromv3.cpp
    ${swrl}/hw/gdrom/disc_common.cpp
    ${swrl}/hw/gdrom/gdrom_response.cpp
    ${swrl}/hw/holly/holly_intc.cpp
    ${swrl}/hw/holly/sh4_mem_area0.cpp
    ${swrl}/hw/holly/sb.cpp
    ${swrl}/hw/sh4/interpr/sh4_fpu.cpp
    ${swrl}/hw/sh4/interpr/sh4_opcodes.cpp
    ${swrl}/hw/sh4/interpr/sh4_interpreter.cpp
    ${swrl}/hw/sh4/sh4_core_regs.cpp
    ${swrl}/hw/sh4/sh4_sched.cpp
    ${swrl}/hw/sh4/sh4_opcode_list.cpp
    ${swrl}/hw/sh4/SuperH4_impl.cpp
    ${swrl}/hw/sh4/modules/intc.cpp
    ${swrl}/hw/sh4/modules/tmu.cpp
    ${swrl}/hw/sh4/modules/mmu.cpp
    ${swrl}/hw/sh4/modules/cpg.cpp
    ${swrl}/hw/sh4/modules/rtc.cpp
    ${swrl}/hw/sh4/modules/ccn.cpp
    ${swrl}/hw/sh4/modules/dmac.cpp
    ${swrl}/hw/sh4/modules/ubc.cpp
    ${swrl}/hw/sh4/modules/scif.cpp
    ${swrl}/hw/sh4/modules/bsc.cpp
    ${swrl}/hw/sh4/modules/sci.cpp
    ${swrl}/hw/sh4/sh4_interrupts.cpp
    ${swrl}/hw/sh4/sh4_rom.cpp
    ${swrl}/hw/sh4/sh4_mem.cpp
    ${swrl}/hw/sh4/sh4_mmr.cpp
    ${swrl}/hw/sh4/dyna/blockmanager.cpp
    ${swrl}/hw/sh4/dyna/driver.cpp
    ${swrl}/hw/sh4/dyna/perf_map.cpp
    ${swrl}/hw/sh4/dyna/blockmanager-extras.cpp
    ${swrl}/hw/sh4/dyna/shil.cpp
    ${swrl}/hw/sh4/dyna/decoder.cpp
    ${swrl}/hw/mem/_vmem.cpp
    # ${swrl}/hw/bba/pcap_io.cpp
    # ${swrl}/hw/bba/rtl8139c.cpp
    # ${swrl}/hw/bba/bba.cpp
    # ${swrl}/hw/bba/VirtualNetwork.cpp
    ${swrl}/hw/maple/maple_helper.cpp
    ${swrl}/hw/maple/maple_cfg.cpp
    ${swrl}/hw/maple/maple_devs.cpp
    ${swrl}/hw/maple/maple_if.cpp
    # ${swrl}/serialize.cpp
    # ${swrl}/linux/nixprof/nixprof.cpp
    # ${swrl}/linux/common.cpp
    # ${swrl}/linux/context.cpp
    ${swrl}/libswirl.cpp

    # ${swrl}/utils/http.cpp
    # ${swrl}/utils/cloudrom.cpp
    ${swrl}/cfg/cl.cpp
    ${swrl}/cfg/cfg.cpp
    ${swrl}/cfg/ini.cpp
    # ${swrl}/input/gamepad_device.cpp
    # ${swrl}/input/keyboard_device.cpp
    # ${swrl}/input/mapping.cpp
    ${swrl}/gpl/lxdream/tacore-stta.cpp
    ${swrl}/gpl/lxdream/tacore-mtta-decoupled.cpp
    # ${swrl}/jit/backend/arm64/rec_arm64.cpp
    ${swrl}/imgread/cdi.cpp
    ${swrl}/imgread/imgread_common.cpp
    ${swrl}/imgread/gdi.cpp
    ${swrl}/imgread/cue.cpp
    ${swrl}/imgread/chd.cpp
    # ${swrl}/profiler/profiler.cpp
    ${swrl}/deps/cdipsr/cdipsr.cpp
    ${swrl}/deps/libelf/elf32.cpp
    ${swrl}/deps/libelf/elf.cpp
    ${swrl}/deps/libelf/elf64.cpp
    ${swrl}/deps/xxhash/xxhash.c
    ${swrl}/deps/crypto/sha256.cpp
    ${swrl}/deps/crypto/sha1.cpp
    ${swrl}/deps/crypto/md5.cpp
    ${swrl}/deps/chdr/huffman.c
    ${swrl}/deps/chdr/flac.c
    ${swrl}/deps/chdr/chd.c
    ${swrl}/deps/chdr/cdrom.c
    ${swrl}/deps/chdr/bitstream.c
    # ${swrl}/deps/khronos/GL3/gl3w.c
    # ${swrl}/deps/libpng/pngset.c
    # ${swrl}/deps/libpng/pngrutil.c
    # ${swrl}/deps/libpng/pngwtran.c
    # ${swrl}/deps/libpng/pngwutil.c
    # ${swrl}/deps/libpng/pngmem.c
    # ${swrl}/deps/libpng/arm/arm_init.c
    # ${swrl}/deps/libpng/arm/filter_neon_intrinsics.c
    # ${swrl}/deps/libpng/pngwio.c
    # ${swrl}/deps/libpng/pngrio.c
    # ${swrl}/deps/libpng/pngwrite.c
    # ${swrl}/deps/libpng/pngtrans.c
    # ${swrl}/deps/libpng/contrib/arm-neon/android-ndk.c
    # ${swrl}/deps/libpng/contrib/arm-neon/linux.c
    # ${swrl}/deps/libpng/contrib/arm-neon/linux-auxv.c
    # ${swrl}/deps/libpng/pngrtran.c
    # ${swrl}/deps/libpng/pngget.c
    # ${swrl}/deps/libpng/pngpread.c
    # ${swrl}/deps/libpng/png.c
    # ${swrl}/deps/libpng/pngerror.c
    # ${swrl}/deps/libpng/pngread.c
    ${swrl}/deps/coreio/coreio.cpp
    # ${swrl}/deps/lzma/CpuArch.c
    # ${swrl}/deps/lzma/7zDec.c
    # ${swrl}/deps/lzma/LzmaDec.c
    # ${swrl}/deps/lzma/Lzma86Dec.c
    # ${swrl}/deps/lzma/7zStream.c
    # ${swrl}/deps/lzma/LzFind.c
    # ${swrl}/deps/lzma/7zCrc.c
    # ${swrl}/deps/lzma/Bra.c
    # ${swrl}/deps/lzma/LzmaLib.c
    # ${swrl}/deps/lzma/LzmaEnc.c
    # ${swrl}/deps/lzma/Bra86.c
    # ${swrl}/deps/lzma/Lzma2Dec.c
    # ${swrl}/deps/lzma/BraIA64.c
    # ${swrl}/deps/lzma/7zCrcOpt.c
    # ${swrl}/deps/lzma/Bcj2.c
    # ${swrl}/deps/lzma/Sort.c
    # ${swrl}/deps/lzma/7zBuf.c
    # ${swrl}/deps/lzma/Alloc.c
    # ${swrl}/deps/lzma/Delta.c
    # ${swrl}/deps/lzma/7zFile.c
    # ${swrl}/deps/lzma/7zArcIn.c
    # ${swrl}/deps/lzma/Lzma86Enc.c
    # ${swrl}/deps/vixl/aarch32/instructions-aarch32.cc
    # ${swrl}/deps/vixl/aarch32/constants-aarch32.cc
    # ${swrl}/deps/vixl/aarch32/disasm-aarch32.cc
    # ${swrl}/deps/vixl/aarch32/assembler-aarch32.cc
    # ${swrl}/deps/vixl/aarch32/location-aarch32.cc
    # ${swrl}/deps/vixl/aarch32/operands-aarch32.cc
    # ${swrl}/deps/vixl/aarch32/macro-assembler-aarch32.cc

    # ${swrl}/deps/vixl/cpu-features.cc
    # ${swrl}/deps/vixl/code-buffer-vixl.cc

    # ${swrl}/deps/vixl/compiler-intrinsics-vixl.cc
    # ${swrl}/deps/vixl/utils-vixl.cc
    # ${swrl}/deps/vixl/aarch64/assembler-aarch64.cc
    # ${swrl}/deps/vixl/aarch64/operands-aarch64.cc
    # ${swrl}/deps/vixl/aarch64/disasm-aarch64.cc
    # ${swrl}/deps/vixl/aarch64/cpu-features-auditor-aarch64.cc
    # ${swrl}/deps/vixl/aarch64/instrument-aarch64.cc
    # ${swrl}/deps/vixl/aarch64/simulator-aarch64.cc
    # ${swrl}/deps/vixl/aarch64/decoder-aarch64.cc
    # ${swrl}/deps/vixl/aarch64/cpu-aarch64.cc
    # ${swrl}/deps/vixl/aarch64/pointer-auth-aarch64.cc
    # ${swrl}/deps/vixl/aarch64/macro-assembler-aarch64.cc
    # ${swrl}/deps/vixl/aarch64/logic-aarch64.cc
    # ${swrl}/deps/vixl/aarch64/instructions-aarch64.cc
    ${swrl}/deps/zlib/inftrees.c
    ${swrl}/deps/zlib/uncompr.c
    ${swrl}/deps/zlib/infback.c
    ${swrl}/deps/zlib/deflate.c
    ${swrl}/deps/zlib/adler32.c
    ${swrl}/deps/zlib/trees.c
    ${swrl}/deps/zlib/compress.c
    ${swrl}/deps/zlib/crc32.c
    ${swrl}/deps/zlib/inffast.c
    ${swrl}/deps/zlib/inflate.c
    ${swrl}/deps/zlib/zutil.c
    # ${swrl}/deps/imgui/imgui.cpp
    # ${swrl}/deps/imgui/imgui_widgets.cpp
    # ${swrl}/deps/imgui/imgui_demo.cpp
    # ${swrl}/deps/imgui/imgui_draw.cpp
    # ${swrl}/deps/flac/src/libFLAC/fixed.c
    # ${swrl}/deps/flac/src/libFLAC/stream_decoder.c
    # ${swrl}/deps/flac/src/libFLAC/fixed_intrin_sse2.c
    # ${swrl}/deps/flac/src/libFLAC/bitreader.c
    # ${swrl}/deps/flac/src/libFLAC/fixed_intrin_ssse3.c
    # ${swrl}/deps/flac/src/libFLAC/format.c
    # ${swrl}/deps/flac/src/libFLAC/lpc_intrin_avx2.c
    # ${swrl}/deps/flac/src/libFLAC/lpc.c
    # ${swrl}/deps/flac/src/libFLAC/window.c
    # ${swrl}/deps/flac/src/libFLAC/memory.c
    # ${swrl}/deps/flac/src/libFLAC/lpc_intrin_sse2.c
    # ${swrl}/deps/flac/src/libFLAC/windows_unicode_filenames.c
    # ${swrl}/deps/flac/src/libFLAC/metadata_object.c
    # ${swrl}/deps/flac/src/libFLAC/crc.c
    # ${swrl}/deps/flac/src/libFLAC/lpc_intrin_sse41.c
    # ${swrl}/deps/flac/src/libFLAC/metadata_iterators.c
    # ${swrl}/deps/flac/src/libFLAC/md5.c
    # ${swrl}/deps/flac/src/libFLAC/float.c
    # ${swrl}/deps/flac/src/libFLAC/lpc_intrin_sse.c
    # ${swrl}/deps/flac/src/libFLAC/bitmath.c
    # ${swrl}/deps/flac/src/libFLAC/cpu.c
)

# find X11
find_package(X11)
if (X11_FOUND)
    list(APPEND _sources
        ${swrl}/rend/refsw/refsw_lists.cpp
        ${swrl}/rend/refsw/refsw_tile.cpp
        ${swrl}/rend/refsw/TexUtils.cpp
        ${swrl}/rend/refsw/refsw_if.cpp
    )
else()
    list(APPEND _sources
        ${swrl}/rend/mister_rend/mister_if.cpp
        ${swrl}/jit/backend/arm32/rec_arm.cpp
        ${swrl}/jit/backend/arm32/ngen_arm.S
        
        ${swrl}/deps/vixl/aarch32/instructions-aarch32.cc
        ${swrl}/deps/vixl/aarch32/constants-aarch32.cc
        ${swrl}/deps/vixl/aarch32/disasm-aarch32.cc
        ${swrl}/deps/vixl/aarch32/assembler-aarch32.cc
        ${swrl}/deps/vixl/aarch32/location-aarch32.cc
        ${swrl}/deps/vixl/aarch32/operands-aarch32.cc
        ${swrl}/deps/vixl/aarch32/macro-assembler-aarch32.cc

        ${swrl}/deps/vixl/cpu-features.cc
        ${swrl}/deps/vixl/code-buffer-vixl.cc
        ${swrl}/deps/vixl/utils-vixl.cc
        ${swrl}/deps/vixl/compiler-intrinsics-vixl.cc

        ${swrl}/linux/posix_vmem.cpp
        ${swrl}/linux/common.cpp
        ${swrl}/linux/context.cpp
    )
endif()

# RetroAchievements: ra/ (minicast glue) + external/rcheevos (v12.4.0)
include(${CMAKE_SOURCE_DIR}/../external/rcheevos.cmake)
list(APPEND _sources ${RA_SOURCES})

add_executable(${APP_NAME}.elf ${_sources})
set_property(TARGET ${APP_NAME}.elf PROPERTY CXX_STANDARD 17)
target_compile_definitions(${APP_NAME}.elf PRIVATE  TARGET_NO_NIXPROF TARGET_NO_COREIO_HTTP HOST_NO_THREADS FAUX96 FEAT_HAS_SERIAL_TTY=0)
target_include_directories(${APP_NAME}.elf PRIVATE ${swrl} ${swrl}/deps ${swrl}/deps/vixl ${RA_INCLUDE_DIRS})
target_link_libraries(${APP_NAME}.elf PRIVATE stdc++ rt pthread)
add_subdirectory(${CMAKE_SOURCE_DIR}/../tests ${CMAKE_BINARY_DIR}/tests)

if (X11_FOUND)
    target_include_directories(${APP_NAME}.elf PRIVATE ${X11_INCLUDE_DIR})
    target_link_libraries(${APP_NAME}.elf PRIVATE ${X11_LIBRARIES})
    target_compile_definitions(${APP_NAME}.elf PRIVATE TARGET_NO_REC TARGET_ULTRA96 TARGET_NO_EXCEPTIONS)
else()
    target_link_libraries(${APP_NAME}.elf PRIVATE -static)
    target_compile_definitions(${APP_NAME}.elf PRIVATE NDEBUG TARGET_MISTER)
    # target_compile_options(${APP_NAME}.elf PRIVATE -Ofast -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -marm -funroll-loops)
	target_compile_options(${APP_NAME}.elf PRIVATE
    -O3
	-marm
    #-mcpu=cortex-a9
	#-mtune=cortex-a9
	-march=armv7-a
    -mfpu=neon
	-mfloat-abi=hard
	-ffast-math
	-flto
	-fwhole-program
    -fno-exceptions
    -fdata-sections
    -ffunction-sections	
	-fno-semantic-interposition
    -funsafe-math-optimizations
    -fomit-frame-pointer
	)
endif()
