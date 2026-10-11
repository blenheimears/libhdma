cmake_minimum_required(VERSION 3.16)
project(libhdma VERSION 0.0.49 LANGUAGES C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
include(GNUInstallDirs)
option(LIBHDMA_ENABLE_SNDFILE_INPUT "Allow optional runtime libsndfile input (OFF for raw PCM only frontend)" ON)
option(LIBHDMA_TESTS "Build and run the C and independent decode test suite" ON)
option(LIBHDMA_REQUIRE_FFMPEG_TESTS "Fail configuration if independent FFmpeg decode tools are missing" OFF)
option(LIBHDMA_X86_V3 "Compile the optional x86-64-v3 XLL kernel (runtime dispatched)" ON)
add_library(hdma STATIC src/core.c src/core_long.c src/core_tables.c src/core_adpcm.c src/core_adpcm_tables.c src/mdct.c src/xll.c src/xll_long.c src/simd.c src/downmix.c)
# Only this translation unit receives -march=x86-64-v3. All other objects,
# including the CPU detector and the public API, retain the baseline ISA.
if(LIBHDMA_X86_V3 AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$"
   AND CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    include(CheckCCompilerFlag)
    check_c_compiler_flag("-march=x86-64-v3" LIBHDMA_HAS_V3_COMPILER)
    if(LIBHDMA_HAS_V3_COMPILER)
        target_sources(hdma PRIVATE src/simd_x86_v3.c)
        set_source_files_properties(src/simd_x86_v3.c PROPERTIES COMPILE_OPTIONS "-march=x86-64-v3")
        target_compile_definitions(hdma PRIVATE HDMA_COMPILED_X86_V3=1)
    endif()
endif()
target_include_directories(hdma PUBLIC "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>" "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
target_include_directories(hdma PRIVATE src)
target_link_libraries(hdma PUBLIC m)
find_package(Threads REQUIRED)
target_link_libraries(hdma PRIVATE Threads::Threads)
if(LIBHDMA_ENABLE_SNDFILE_INPUT)
    set(HDMA_INPUT_SOURCE src/sndfile_input.c)
else()
    set(HDMA_INPUT_SOURCE src/sndfile_disabled.c)
endif()
add_executable(hdmaenc src/main.c src/matroska.c ${HDMA_INPUT_SOURCE} src/raw_pcm_input.c src/xll_workers.c src/core_signature.c src/progress.c src/cpu_count.c)
target_include_directories(hdmaenc PRIVATE include src)
target_link_libraries(hdmaenc PRIVATE hdma Threads::Threads ${CMAKE_DL_LIBS})
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(hdma PRIVATE -Wall -Wextra -Werror)
    target_compile_options(hdmaenc PRIVATE -Wall -Wextra -Werror)
endif()
install(TARGETS hdmaenc hdma RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES include/libhdma.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
if(LIBHDMA_TESTS)
    find_program(FFMPEG_EXE ffmpeg)
    find_program(FFPROBE_EXE ffprobe)
    if(LIBHDMA_REQUIRE_FFMPEG_TESTS AND NOT (FFMPEG_EXE AND FFPROBE_EXE))
        message(FATAL_ERROR "FFmpeg/ffprobe are required for independent codec acceptance tests")
    endif()
    enable_testing()
    add_executable(test_downmix tests/test_downmix.c src/downmix.c)
    target_include_directories(test_downmix PRIVATE src)
    add_test(NAME core-surround-downmix-invariants COMMAND test_downmix)
    add_executable(test_simd tests/test_simd.c)
    target_include_directories(test_simd PRIVATE src)
    target_link_libraries(test_simd PRIVATE hdma)
    add_test(NAME xll-simd-bitexact-kernels COMMAND test_simd)
    add_executable(test_core_adpcm tests/test_core_adpcm.c)
    target_link_libraries(test_core_adpcm PRIVATE hdma)
    add_test(NAME core-adpcm-vq-512-and-1024 COMMAND test_core_adpcm)
    add_executable(test_codec tests/test_codec.c)
    target_link_libraries(test_codec PRIVATE hdma)
    add_test(NAME core-unit COMMAND test_codec)
    add_test(NAME project-identity COMMAND sh ${PROJECT_SOURCE_DIR}/tests/project_identity.sh
                 ${PROJECT_SOURCE_DIR} $<TARGET_FILE:hdmaenc>)
    add_executable(test_cpu_count tests/test_cpu_count.c src/cpu_count.c)
    target_include_directories(test_cpu_count PRIVATE src)
    add_test(NAME runtime-cpu-count COMMAND test_cpu_count)
    add_test(NAME darwin-feature-macro-contract COMMAND sh ${PROJECT_SOURCE_DIR}/tests/darwin_feature_macro_contract.sh ${PROJECT_SOURCE_DIR})
    add_test(NAME gcc11-xll-channel-bounds-contract COMMAND sh ${PROJECT_SOURCE_DIR}/tests/gcc11_xll_bounds_contract.sh ${PROJECT_SOURCE_DIR})
    add_executable(test_fixture tests/fixture.c)
    target_link_libraries(test_fixture PRIVATE m)
    add_executable(test_xll_tools tests/test_xll_tools.c)
    add_executable(test_official_structure tests/test_official_structure.c)
    add_executable(test_xll_segment_ceiling tests/test_xll_segment_ceiling.c)
    add_test(NAME xll-segment-ceiling-48k-512-1024
             COMMAND sh ${PROJECT_SOURCE_DIR}/tests/segment_limit_runtime.sh
                     $<TARGET_FILE:hdmaenc> $<TARGET_FILE:test_xll_segment_ceiling>)
    if(FFMPEG_EXE)
        find_package(Python3 COMPONENTS Interpreter REQUIRED)
        add_test(NAME xll-independent-decoder-controls-bitexact
                 COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/xll_decoder_controls_runtime.py
                         $<TARGET_FILE:hdmaenc> ${FFMPEG_EXE})
        add_test(NAME receiver-71-embedded-downmix-runtime
                 COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/receiver_71_downmix_roundtrip.py
                         $<TARGET_FILE:hdmaenc> ${FFMPEG_EXE})
    endif()
    add_executable(test_core_signature tests/test_core_signature.c src/core_signature.c)
    target_include_directories(test_core_signature PRIVATE src)
    target_link_libraries(test_core_signature PRIVATE m)
    add_test(NAME debug-core-melody-generator COMMAND test_core_signature)
    # Historical integration fixtures feed WAV through the optional libsndfile
    # input frontend. Do not register them for a raw-PCM-only build.
    if(LIBHDMA_ENABLE_SNDFILE_INPUT)
    add_test(NAME progress-tty-ratio-and-disable
             COMMAND sh ${PROJECT_SOURCE_DIR}/tests/progress_ratio.sh
                     $<TARGET_FILE:hdmaenc> $<TARGET_FILE:test_fixture>)
    add_executable(test_matroska tests/test_matroska.c)
    target_link_libraries(test_matroska PRIVATE m)
    add_executable(test_seek_pcm tests/test_seek_pcm.c)
    add_executable(test_xll_structure tests/test_xll_structure.c)
    add_executable(test_xll_split tests/test_xll_split.c)
    add_executable(test_xll_7x_reference tests/test_xll_7x_reference.c)
    add_executable(test_frame_lengths tests/test_frame_lengths.c)
    if(FFMPEG_EXE AND FFPROBE_EXE)
        add_test(NAME core-adpcm-512-1024-lossless-reference-and-decode
                 COMMAND sh ${PROJECT_SOURCE_DIR}/tests/adpcm_roundtrip.sh
                         $<TARGET_FILE:hdmaenc> ${FFMPEG_EXE} ${FFPROBE_EXE}
                         $<TARGET_FILE:test_fixture> $<TARGET_FILE:test_core_adpcm>)
        add_test(NAME xll-fast-default-and-ultra-split-bitexact
                 COMMAND sh ${PROJECT_SOURCE_DIR}/tests/default_ultra_mode.sh
                         $<TARGET_FILE:hdmaenc> ${FFMPEG_EXE}
                         $<TARGET_FILE:test_fixture> $<TARGET_FILE:test_xll_split>)
        add_test(NAME xll-long-1024-and-ultra-bitexact
                 COMMAND sh ${PROJECT_SOURCE_DIR}/tests/long_frame_ultra_roundtrip.sh
                         $<TARGET_FILE:hdmaenc> ${FFMPEG_EXE} ${FFPROBE_EXE}
                         $<TARGET_FILE:test_fixture> $<TARGET_FILE:test_frame_lengths>)
        add_test(NAME xll-7x-core-reference-and-side-policies-bitexact
                 COMMAND sh ${PROJECT_SOURCE_DIR}/tests/seven_core_reference_roundtrip.sh
                         $<TARGET_FILE:hdmaenc> ${FFMPEG_EXE} ${FFPROBE_EXE}
                         $<TARGET_FILE:test_fixture> $<TARGET_FILE:test_xll_7x_reference>)
        add_test(NAME xll-7x-hardware-compatibility-baseline
                 COMMAND sh ${PROJECT_SOURCE_DIR}/tests/hardware_7x_compat_roundtrip.sh
                         $<TARGET_FILE:hdmaenc> ${FFMPEG_EXE}
                         $<TARGET_FILE:test_fixture> $<TARGET_FILE:test_xll_7x_reference>)
        add_test(NAME xll-scalable-lsbs-and-reference-bitexact
                 COMMAND sh ${PROJECT_SOURCE_DIR}/tests/scalable_reference_roundtrip.sh
                         $<TARGET_FILE:hdmaenc> ${FFMPEG_EXE}
                         $<TARGET_FILE:test_fixture> $<TARGET_FILE:test_xll_split>)
        add_test(NAME extended-layout-downmix-and-bitexact
                 COMMAND sh ${PROJECT_SOURCE_DIR}/tests/extended_layout_roundtrip.sh
                         $<TARGET_FILE:hdmaenc> ${FFMPEG_EXE} ${FFPROBE_EXE}
                         $<TARGET_FILE:test_fixture>)
        add_test(NAME xll-simd-scalar-v3-bitstream-and-decode
                 COMMAND sh ${PROJECT_SOURCE_DIR}/tests/simd_roundtrip.sh
                         $<TARGET_FILE:hdmaenc> ${FFMPEG_EXE} $<TARGET_FILE:test_fixture>)
        add_test(NAME parallel-xll-bitexact-and-determinism
                 COMMAND sh ${PROJECT_SOURCE_DIR}/tests/multithread_roundtrip.sh
                         $<TARGET_FILE:hdmaenc> ${FFMPEG_EXE} $<TARGET_FILE:test_fixture>)
        add_test(NAME core-independent-decode
                 COMMAND sh ${PROJECT_SOURCE_DIR}/tests/roundtrip.sh
                         $<TARGET_FILE:hdmaenc> ${FFMPEG_EXE} ${FFPROBE_EXE} $<TARGET_FILE:test_fixture>)
        add_test(NAME xll-independent-bitexact
                 COMMAND sh ${PROJECT_SOURCE_DIR}/tests/lossless_roundtrip.sh
                         $<TARGET_FILE:hdmaenc> ${FFMPEG_EXE} ${FFPROBE_EXE} $<TARGET_FILE:test_fixture>)
        add_test(NAME xll-predictor-decorrelation-regression
                 COMMAND sh ${PROJECT_SOURCE_DIR}/tests/compression_regression.sh
                         $<TARGET_FILE:hdmaenc> ${FFMPEG_EXE} ${FFPROBE_EXE} $<TARGET_FILE:test_fixture> $<TARGET_FILE:test_xll_structure>)
        add_test(NAME xll-entropy-reuse-and-joint-predictor-grid
                 COMMAND sh ${PROJECT_SOURCE_DIR}/tests/entropy_joint_roundtrip.sh
                         $<TARGET_FILE:hdmaenc> ${FFMPEG_EXE}
                         $<TARGET_FILE:test_fixture> $<TARGET_FILE:test_xll_tools>)
        add_test(NAME xll-order15-expanded-channel-pairs-bitexact
                 COMMAND sh ${PROJECT_SOURCE_DIR}/tests/expanded_prediction_decor.sh
                         $<TARGET_FILE:hdmaenc> ${FFMPEG_EXE}
                         $<TARGET_FILE:test_fixture> $<TARGET_FILE:test_xll_tools>)
        add_test(NAME xll-highorder-hybrid-bitexact
                 COMMAND sh ${PROJECT_SOURCE_DIR}/tests/higher_order_hybrid.sh
                         $<TARGET_FILE:hdmaenc> ${FFMPEG_EXE}
                         $<TARGET_FILE:test_fixture> $<TARGET_FILE:test_xll_tools>)
        add_test(NAME xll-per-channel-reference-and-grid-bitexact
                 COMMAND sh ${PROJECT_SOURCE_DIR}/tests/channel_segment_roundtrip.sh
                         $<TARGET_FILE:hdmaenc> ${FFMPEG_EXE} ${FFPROBE_EXE}
                         $<TARGET_FILE:test_fixture> $<TARGET_FILE:test_xll_structure>)
        add_test(NAME xll-core-reference-bitexact
                 COMMAND sh ${PROJECT_SOURCE_DIR}/tests/core_reference_roundtrip.sh
                         $<TARGET_FILE:hdmaenc> ${FFMPEG_EXE} ${FFPROBE_EXE} $<TARGET_FILE:test_fixture>)
        add_test(NAME aligned-7x-lookahead-bitexact-layout-and-size
                 COMMAND sh ${PROJECT_SOURCE_DIR}/tests/aligned_7x_roundtrip.sh
                         $<TARGET_FILE:hdmaenc> ${FFMPEG_EXE} ${FFPROBE_EXE} $<TARGET_FILE:test_fixture>)
        add_test(NAME aligned-core-lookahead-bitexact-and-size
                 COMMAND sh ${PROJECT_SOURCE_DIR}/tests/aligned_core_roundtrip.sh
                         $<TARGET_FILE:hdmaenc> ${FFMPEG_EXE} ${FFPROBE_EXE} $<TARGET_FILE:test_fixture>)
        add_test(NAME matroska-duration-seeking-compression
                 COMMAND sh ${PROJECT_SOURCE_DIR}/tests/seek_duration.sh
                         $<TARGET_FILE:hdmaenc> ${FFMPEG_EXE} ${FFPROBE_EXE}
                         $<TARGET_FILE:test_fixture> $<TARGET_FILE:test_matroska>
                         $<TARGET_FILE:test_seek_pcm>)
    endif()
    endif() # LIBHDMA_ENABLE_SNDFILE_INPUT
    add_test(NAME raw-pcm-stdin-lossless-and-malformed
             COMMAND sh ${PROJECT_SOURCE_DIR}/tests/raw_pcm_runtime.sh
                     $<TARGET_FILE:hdmaenc>)
    if(FFMPEG_EXE AND FFPROBE_EXE)
        add_test(NAME official-observed-core-exss-xll-structure
                 COMMAND sh ${PROJECT_SOURCE_DIR}/tests/official_structure_runtime.sh
                         $<TARGET_FILE:hdmaenc> ${FFMPEG_EXE}
                         $<TARGET_FILE:test_official_structure>
                         $<TARGET_FILE:test_fixture>)
        add_test(NAME xll-prediction-order-cap-roundtrip
                 COMMAND sh ${PROJECT_SOURCE_DIR}/tests/prediction_order_roundtrip.sh
                         $<TARGET_FILE:hdmaenc> ${FFMPEG_EXE}
                         $<TARGET_FILE:test_fixture> $<TARGET_FILE:test_xll_tools>)
        add_test(NAME debug-core-melody-independent-lossless
                 COMMAND sh ${PROJECT_SOURCE_DIR}/tests/core_signature_roundtrip.sh
                         $<TARGET_FILE:hdmaenc> ${FFMPEG_EXE} ${FFPROBE_EXE})
        add_test(NAME debug-core-melody-nonzero-21-51-71
                 COMMAND sh ${PROJECT_SOURCE_DIR}/tests/core_signature_nonzero.sh
                         $<TARGET_FILE:hdmaenc> ${FFMPEG_EXE})
        add_test(NAME layout21-raw-pcm-lossless-and-core
                 COMMAND sh ${PROJECT_SOURCE_DIR}/tests/layout21_runtime.sh
                         $<TARGET_FILE:hdmaenc> ${FFMPEG_EXE} ${FFPROBE_EXE}
                         $<TARGET_FILE:test_fixture>)
    endif()
endif()
