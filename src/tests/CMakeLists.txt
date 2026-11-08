# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

# SPDX-FileCopyrightText: 2018 yuzu Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later

add_executable(tests
    common/bit_field.cpp
    common/cityhash.cpp
    common/container_hash.cpp
    common/event.cpp
    common/fibers.cpp
    common/game_presets.cpp
    common/host_memory.cpp
    common/page_table.cpp
    common/param_package.cpp
    common/range_map.cpp
    common/ring_buffer.cpp
    common/scratch_buffer.cpp
    common/slot_vector.cpp
    common/undefined_fix.cpp
    common/unique_function.cpp
    common/zstd_compression.cpp
    core/core_timing.cpp
    core/ldn.cpp
    core/internal_network/socket_proxy.cpp
    core/nup_writer.cpp
    core/nx2_debug_server.cpp
    core/nx2_movie_pacing.cpp
    core/priority_queue.cpp
    core/internal_network/network.cpp
    video_core/frame_dump_schedule.cpp
    video_core/framebuffer_limits.cpp
    video_core/memory_tracker.cpp
    video_core/spirv_storage_alignment.cpp
    video_core/pipeline_optimization.cpp
    video_core/spirv_atomic_retry.cpp
    video_core/spirv_fragment_shuffle.cpp
    video_core/ampere_depth_compare.cpp
    video_core/h264_packet.cpp
    video_core/vertex_repack.cpp
    video_core/ampere_lowering.cpp
    video_core/spirv_software_matrix.cpp
    video_core/ampere_geometry.cpp
    video_core/ampere_i2f.cpp
    video_core/ampere_gradient.cpp
    video_core/ampere_returns.cpp
    video_core/ampere_guarded_ulea.cpp
    video_core/ampere_compute_lowering.cpp
    video_core/native_macro_fifo.cpp
    video_core/uniform_stream_reuse.cpp
    video_core/shader_invalidation.cpp
    video_core/descriptor_table.cpp
    video_core/global_address_tracking.cpp
    video_core/global_atomic_routing.cpp
    video_core/nvn_storage_descriptor.cpp
    video_core/ampere_f2i.cpp
    video_core/ampere_ldc.cpp
    video_core/ampere_vote.cpp
    video_core/shader_attributes.cpp
    video_core/indirect_reset.cpp
    input_common/calibration_configuration_job.cpp
)

create_target_directory_groups(tests)

target_link_libraries(tests PRIVATE common core input_common video_core)
target_link_libraries(tests PRIVATE ${PLATFORM_LIBRARIES} Catch2::Catch2WithMain
    Threads::Threads OpenSSL::SSL OpenSSL::Crypto)

add_test(NAME tests COMMAND tests)

add_executable(composite_completion_test video_core/composite_completion_standalone.cpp)
target_include_directories(composite_completion_test PRIVATE "${PROJECT_SOURCE_DIR}/src")
add_test(NAME composite_completion_test COMMAND composite_completion_test)

add_executable(subrect_runs_test video_core/subrect_runs_standalone.cpp)
target_include_directories(subrect_runs_test PRIVATE "${PROJECT_SOURCE_DIR}/src")
add_test(NAME subrect_runs_test COMMAND subrect_runs_test)

# needed for vma
if (NOT MSVC)
    target_compile_options(tests PRIVATE
        $<$<COMPILE_LANGUAGE:C,CXX>:-Wno-conversion>
        $<$<COMPILE_LANGUAGE:C,CXX>:-Wno-unused-variable>
        $<$<COMPILE_LANGUAGE:C,CXX>:-Wno-unused-parameter>
        $<$<COMPILE_LANGUAGE:C,CXX>:-Wno-missing-field-initializers>)
endif()

include(core/mods/duo_tests.cmake) # Eden Duo: dual-screen runtime tests
