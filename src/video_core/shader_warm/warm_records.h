// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "common/common_types.h"

namespace VideoCommon::ShaderWarm {

/// Precache file records: compute entries in the disk pipeline cache format, with no recorded
/// reads (the emulator guesses them, see GuessMissing).

/// Precache files use the Vulkan pipeline cache version: the emulator loads one only when it
/// matches, so the two must move together (vk_pipeline_cache.cpp checks it).
constexpr u32 WarmCacheVersion = 35; // Duet Plus: see CACHE_VERSION in vk_pipeline_cache.cpp

/// What the builder finds in the game files, recorded in the precache's count file. A file built by
/// an older builder is rebuilt at boot (RefreshWarmFiles). 2: Mario Kart World's .esetb effects.
/// 3: .ptcl particle effects (Fire Emblem).
constexpr u32 WarmBuilderVersion = 3;

using ComputeKey = std::array<u8, 24>;

/// A compute program lowered and laid out like GenericEnvironment::LowerNativeCode.
struct LoweredCode {
    std::vector<u8> code;
    /// Constant buffer the program's bound textures read, ~0U when it names none.
    u32 texture_buffer{~0U};
};

/// Captures and lowers one extracted compute program as the runtime does for a dispatch and lays
/// it out with LayOutLoweredNativeCode (a zero shader header). Matrix multiply programs, lowered
/// per title at runtime, are not converted. Returns the error text when the program cannot be
/// lowered.
std::optional<std::string> LowerProgram(std::span<const u8> program, LoweredCode& out);

/// Captures and lowers one extracted graphics program (a 128-byte header, then the native code) as
/// GraphicsEnvironment does: the header's first 80 bytes are the shader program header in front
/// of the lowered code. A geometry program that loads its own address lowers to other code than
/// at runtime (the address is not known here).
std::optional<std::string> LowerGraphicsProgram(std::span<const u8> program, bool geometry,
                                                LoweredCode& out);

/// A Switch 1 program as the runtime caches it: the bytes from its start address to its end
/// (GenericEnvironment::TryFindSize) and the hash Analyze gives them. The cache holds two words
/// more than the hashed range (the self-branch and the word behind it, never executed).
struct MaxwellProgram {
    std::vector<u8> code;
    u64 hash{};
    /// The last cached word lies outside the code area, where the guest's memory holds whatever
    /// the game put next (often the next program's header): it is zero here, so a record with
    /// this code works the same but is not byte for byte the runtime's.
    bool padded{};
};

/// The program that starts `start` bytes into a scanned code area (graphics: 0x30, at the shader
/// program header). Empty when it does not end inside the area: the runtime then reads on into
/// whatever follows it in guest memory, which the file does not tell.
std::optional<MaxwellProgram> FindMaxwellProgram(std::span<const u8> code_area, size_t start,
                                                 bool is_proprietary_driver);

/// The pipeline cache key hash of laid out code (NativeShaderHash).
u64 StageHash(std::span<const u8> code);

struct ComputeControl {
    std::array<u32, 3> workgroup{};
    u32 shared_memory{};
    u32 local_memory{};
};

/// Workgroup (control bytes 88, 92, 96), shared memory (byte 100 rounded up to 256) and local
/// memory (byte 104). Empty when the control is too short.
std::optional<ComputeControl> ReadComputeControl(std::span<const u8> control);

/// One compute entry (env count, env, code, compute trailer, key); `key` gets the 24-byte key.
std::vector<u8> ComputeEntry(const LoweredCode& lowered, const ComputeControl& control,
                             ComputeKey& key);

/// Peanuts Native's key of a shader: the first 16 hex digits of SHA-256 over its control bytes
/// and the whole code area. NVN2: control bytes 0 to 0x30, four zero bytes and bytes 0x34 to
/// 0x48. Switch 1: the first 0x880 control bytes, or the section's own length (at 0x20) when
/// that is 0x760 to 0x87f. Empty when the control section is shorter than that. (Peanuts'
/// per-game shader_key_skip, one title so far, is not applied.)
std::optional<u64> PeanutsKey(std::span<const u8> control, std::span<const u8> code);

/// A Peanuts Native prewarm list (<title id>.tsv.zst): the shaders a game used (S and C rows) and
/// its render pipelines' vertex and fragment shaders (R rows; their state is not read).
struct PeanutsList {
    std::unordered_set<u64> shaders;
    std::vector<std::pair<u64, u64>> pairs; ///< Vertex and fragment key, each pair once
};

/// Empty when `compressed` is not a zstd frame.
std::optional<PeanutsList> ReadPeanutsList(std::span<const u8> compressed);

/// A pipeline cache record of a vertex and a fragment program with no recorded reads and an
/// all-zero pipeline state: the emulator gives it a state and guesses the reads (see
/// PairsWarmPath). The code is LowerGraphicsProgram's or, with texture buffer 2 (the guest
/// driver Switch 1 titles use), MaxwellProgram's; the hashes are the runtime's for it.
std::vector<u8> PairEntry(const LoweredCode& vertex, u64 vertex_hash, const LoweredCode& fragment,
                          u64 fragment_hash);

/// A pipeline list ("plan"): the records of a pipeline cache with each environment's code replaced
/// by the hash the runtime gives it (the code size stays in the environment header), so the file
/// holds no game code. Layout: "edenplan", PlanVersion, cache version, then per record
///   u32 environments, each: header (as serialized), u64 hash, u32 size + the bytes after the code
///   u32 size + key
constexpr u32 PlanVersion = 1;

struct PlanEnv {
    std::span<const u8> header; ///< GenericEnvironment::Serialize up to the code
    u64 hash{};
    std::span<const u8> tail; ///< Recorded reads and the stage's trailer
    [[nodiscard]] u64 CodeSize() const;
};

struct PlanRecord {
    std::vector<PlanEnv> envs;
    std::span<const u8> key;
};

/// Writes the plan of the pipeline cache at `cache_file` (current cache version) to `plan_file`.
/// Returns the record count, empty when the cache cannot be read or the plan not written.
std::optional<size_t> ExportPlan(const std::filesystem::path& cache_file,
                                 const std::filesystem::path& plan_file);

/// The records of a plan file's bytes (the spans point into `plan`). Empty when it is not a
/// well-formed plan of this plan and cache version.
std::optional<std::vector<PlanRecord>> ReadPlan(std::span<const u8> plan);

/// `record` as a pipeline cache record; `code` must hold every environment's code by hash.
std::vector<u8> FilledRecord(const PlanRecord& record,
                             const std::unordered_map<u64, std::vector<u8>>& code);

struct PlanCheck {
    size_t matched{};    ///< Cache records the filled file holds byte for byte
    size_t mismatched{}; ///< Filled from the same plan record, but with other bytes
    size_t not_found{};  ///< Not filled
};

/// Compares every record of `cache_file` with the records of `filled_file`.
PlanCheck CheckFilledRecords(const std::filesystem::path& cache_file,
                             const std::filesystem::path& filled_file);

} // namespace VideoCommon::ShaderWarm
