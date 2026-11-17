// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <charconv>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>

#include <openssl/evp.h>

#include "common/cityhash.h"
#include "common/zstd_compression.h"
#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"
#include "shader_recompiler/frontend/ampere/program_address_map.h"
#include "shader_recompiler/frontend/ampere/program_capture.h"
#include "video_core/renderer_vulkan/vk_graphics_pipeline.h"
#include "video_core/shader_environment.h"
#include "video_core/shader_warm/warm_records.h"

namespace VideoCommon::ShaderWarm {
namespace {

/// The shader header a compute environment holds in front of its code (all zero).
constexpr size_t ComputeHeaderWords = 10;
constexpr size_t EnvHeaderSize = 6 * 8 + 7 * 4;
constexpr u32 ComputeStage = 5;

template <typename T>
void Put(std::vector<u8>& out, T value) {
    const size_t at = out.size();
    out.resize(at + sizeof(T));
    std::memcpy(out.data() + at, &value, sizeof(T));
}

void PutBytes(std::vector<u8>& out, std::span<const u8> bytes) {
    out.insert(out.end(), bytes.begin(), bytes.end());
}

template <typename T>
T Get(std::span<const u8> data, size_t offset) {
    T value;
    std::memcpy(&value, data.data() + offset, sizeof(T));
    return value;
}

/// The GenericEnvironment::Serialize header: six u64 counts and seven u32 fields. The code is
/// cached from `read_lowest`.
void PutEnvHeader(std::vector<u8>& out, u64 code_size, u64 cbuf_count, u32 local_memory,
                  u32 texture_bound, u32 start_address, u32 stage, u32 read_lowest = 0) {
    Put<u64>(out, code_size);
    Put<u64>(out, 0); // texture types
    Put<u64>(out, 0); // texture pixel formats
    Put<u64>(out, cbuf_count);
    Put<u64>(out, 0); // storage pointer types
    Put<u64>(out, 0); // cbuf replacements
    Put<u32>(out, local_memory);
    Put<u32>(out, texture_bound);
    Put<u32>(out, start_address);
    Put<u32>(out, read_lowest);
    Put<u32>(out, read_lowest + static_cast<u32>(code_size - 8)); // read highest
    Put<u32>(out, 1);                                  // viewport transform state
    Put<u32>(out, stage);
}

/// Captures the program in `program` like GenericEnvironment::LowerNativeCode, lowers it and lays
/// it out behind `header`. `whole` accepts a program that ends before its capture is complete (at
/// runtime, one that ends with its mapping).
std::optional<std::string> Lower(std::span<const u64> header, std::span<const u8> program,
                                 const Shader::Ampere::LoweringOptions& options, bool whole,
                                 LoweredCode& out) {
    using Shader::Ampere::Instruction;
    if (program.empty() || program.size() % sizeof(Instruction) ||
        program.size() / sizeof(Instruction) > 65536) {
        return "Native input must contain 1..65536 complete 16-byte instructions";
    }
    std::vector<Instruction> native(program.size() / sizeof(Instruction));
    std::memcpy(native.data(), program.data(), native.size() * sizeof(Instruction));
    Shader::Ampere::ProgramCaptureState capture;
    bool complete = whole;
    for (u32 index = 0; index < native.size(); ++index) {
        const auto result = capture.Observe(native[index], index);
        if (result == Shader::Ampere::ProgramCaptureState::Result::InvalidTarget) {
            return "Native allocation has an out-of-range branch target";
        }
        if (result == Shader::Ampere::ProgramCaptureState::Result::Complete) {
            native.resize(index + 1);
            complete = true;
            break;
        }
    }
    if (!complete) {
        return "Native allocation lacks a complete runtime capture";
    }
    auto lowered = Shader::Ampere::LowerToMaxwell(native, options);
    if (!lowered) {
        return std::string(lowered.error);
    }
    const std::vector<u64> words = LayOutLoweredNativeCode(header, native, lowered);
    out.code.resize(words.size() * sizeof(u64));
    std::memcpy(out.code.data(), words.data(), out.code.size());
    out.texture_buffer = lowered.required_texture_buffer;
    return std::nullopt;
}

} // Anonymous namespace

std::optional<std::string> LowerProgram(std::span<const u8> program, LoweredCode& out) {
    // As GenericEnvironment::LowerNativeCode for a compute dispatch, without the tensor forms.
    constexpr std::array<u64, ComputeHeaderWords> header{};
    return Lower(header, program, {}, false, out);
}

std::optional<std::string> LowerGraphicsProgram(std::span<const u8> program, bool geometry,
                                                LoweredCode& out) {
    constexpr size_t NativeHeaderSize = 128;
    if (program.size() <= NativeHeaderSize) {
        return "Native graphics program has no code";
    }
    std::array<u64, sizeof(Shader::ProgramHeader) / sizeof(u64)> header;
    std::memcpy(header.data(), program.data(), sizeof(header));
    return Lower(header, program.subspan(NativeHeaderSize), {.geometry = geometry}, true, out);
}

std::optional<MaxwellProgram> FindMaxwellProgram(std::span<const u8> code_area, size_t start,
                                                 bool is_proprietary_driver) {
    if (start >= code_area.size()) {
        return std::nullopt;
    }
    const auto bytes = code_area.subspan(start);
    std::vector<u64> words(bytes.size() / sizeof(u64));
    std::memcpy(words.data(), bytes.data(), words.size() * sizeof(u64));
    const auto size = FindMaxwellCodeEnd(words, is_proprietary_driver);
    // The end word must be in the area too (the first EXIT can be its last word).
    if (!size || *size + sizeof(u64) > words.size() * sizeof(u64)) {
        return std::nullopt;
    }
    MaxwellProgram program;
    program.hash = Common::CityHash64(reinterpret_cast<const char*>(bytes.data()), *size);
    const size_t cached = *size + 2 * sizeof(u64);
    program.padded = cached > bytes.size();
    program.code.assign(bytes.begin(), bytes.begin() + (std::min)(cached, bytes.size()));
    program.code.resize(cached);
    return program;
}

u64 StageHash(std::span<const u8> code) {
    std::vector<u64> words(code.size() / sizeof(u64));
    std::memcpy(words.data(), code.data(), words.size() * sizeof(u64));
    return NativeShaderHash(words);
}

std::optional<ComputeControl> ReadComputeControl(std::span<const u8> control) {
    if (control.size() < 108) {
        return std::nullopt;
    }
    ComputeControl result;
    for (size_t i = 0; i < 3; ++i) {
        result.workgroup[i] = Get<u32>(control, 88 + i * 4);
    }
    result.shared_memory =
        static_cast<u32>((u64{Get<u32>(control, 100)} + 255) / 256 * 256);
    result.local_memory = Get<u32>(control, 104);
    return result;
}

std::vector<u8> ComputeEntry(const LoweredCode& lowered, const ComputeControl& control,
                             ComputeKey& key) {
    std::vector<u8> out;
    out.reserve(4 + EnvHeaderSize + lowered.code.size() + 16 + key.size());
    Put<u32>(out, 1);
    // Like InitializeNativeCode: the program's own bank when it names one.
    const u32 texture_bound = lowered.texture_buffer != ~0U ? lowered.texture_buffer : 0;
    PutEnvHeader(out, lowered.code.size(), 0, control.local_memory, texture_bound, 80,
                 ComputeStage);
    PutBytes(out, lowered.code);
    for (const u32 size : control.workgroup) {
        Put<u32>(out, size);
    }
    Put<u32>(out, control.shared_memory);
    std::vector<u8> key_bytes;
    Put<u64>(key_bytes, StageHash(lowered.code));
    Put<u32>(key_bytes, control.shared_memory);
    for (const u32 size : control.workgroup) {
        Put<u32>(key_bytes, size);
    }
    std::memcpy(key.data(), key_bytes.data(), key.size());
    PutBytes(out, key_bytes);
    return out;
}

std::optional<u64> PeanutsKey(std::span<const u8> control, std::span<const u8> code) {
    if (control.size() < 0x48) {
        return std::nullopt;
    }
    const bool native = Get<u32>(control, 4) == 0xc9;
    size_t length = 0x48;
    if (!native) {
        const u32 own = Get<u32>(control, 0x20);
        length = own >= 0x760 && own < 0x880 ? own : 0x880;
        if (control.size() < length) {
            return std::nullopt;
        }
    }
    // The native control's own length (at 0x30) differs between containers: hashed as zero.
    const size_t skip = native ? 0x30 : length;
    constexpr std::array<u8, 4> zero{};
    std::array<u8, EVP_MAX_MD_SIZE> digest{};
    EVP_MD_CTX* const context = EVP_MD_CTX_new();
    const bool ok = context != nullptr && EVP_DigestInit_ex(context, EVP_sha256(), nullptr) == 1 &&
                    EVP_DigestUpdate(context, control.data(), skip) == 1 &&
                    (!native ||
                     (EVP_DigestUpdate(context, zero.data(), zero.size()) == 1 &&
                      EVP_DigestUpdate(context, control.data() + 0x34, length - 0x34) == 1)) &&
                    EVP_DigestUpdate(context, code.data(), code.size()) == 1 &&
                    EVP_DigestFinal_ex(context, digest.data(), nullptr) == 1;
    EVP_MD_CTX_free(context);
    if (!ok) {
        return std::nullopt;
    }
    u64 key = 0;
    for (size_t i = 0; i < sizeof(u64); ++i) {
        key = key << 8 | digest[i];
    }
    return key;
}

std::optional<PeanutsList> ReadPeanutsList(std::span<const u8> compressed) {
    const std::vector<u8> bytes = Common::Compression::DecompressDataZSTD(compressed);
    if (bytes.empty()) {
        return std::nullopt;
    }
    const auto parse_key = [](std::string_view text) -> std::optional<u64> {
        u64 key{};
        if (text.size() < 16 ||
            std::from_chars(text.data(), text.data() + 16, key, 16).ptr != text.data() + 16) {
            return std::nullopt;
        }
        return key;
    };
    // A shader key inside an R row: 16 ASCII hex digits, themselves written as hex.
    const auto row_key = [&](std::string_view row) -> std::optional<u64> {
        std::array<char, 16> text{};
        for (size_t i = 0; i < text.size() && row.size() >= 32; ++i) {
            u8 character{};
            std::from_chars(row.data() + i * 2, row.data() + i * 2 + 2, character, 16);
            text[i] = static_cast<char>(character);
        }
        return parse_key(std::string_view{text.data(), text.size()});
    };
    PeanutsList list;
    std::unordered_set<u64> seen_pairs;
    const std::string_view text{reinterpret_cast<const char*>(bytes.data()), bytes.size()};
    for (size_t at = 0; at < text.size();) {
        const size_t end = (std::min)(text.find('\n', at), text.size());
        const std::string_view line = text.substr(at, end - at);
        at = end + 1;
        if (line.size() < 2 || line[1] != '\t') {
            continue;
        }
        const std::string_view row = line.substr(2);
        if (line[0] == 'S' || line[0] == 'C') {
            if (const auto key = parse_key(row)) {
                list.shaders.insert(*key);
            }
        } else if (line[0] == 'R' && row.size() >= 96) {
            // pso_skey starts with char vs[24], fs[24]. Peanuts' own state (formats, blend,
            // vertex layout) follows; translating it to a pipeline state would start here.
            const auto vertex = row_key(row);
            const auto fragment = row_key(row.substr(48));
            if (vertex && fragment && seen_pairs.insert(*vertex * 31 + *fragment).second) {
                list.pairs.emplace_back(*vertex, *fragment);
            }
        }
    }
    return list;
}

std::vector<u8> PairEntry(const LoweredCode& vertex, u64 vertex_hash, const LoweredCode& fragment,
                          u64 fragment_hash) {
    using Maxwell = Tegra::Engines::Maxwell3D::Regs;
    std::vector<u8> out;
    Put<u32>(out, 2);
    const auto put_env = [&out](const LoweredCode& lowered, Shader::Stage stage) {
        // What GraphicsEnvironment holds: the header in front of the code gives the local
        // memory, and a native program its own texture bank when it names one.
        const std::span<const u8> header{lowered.code.data(), sizeof(Shader::ProgramHeader)};
        Shader::ProgramHeader sph;
        std::memcpy(&sph, header.data(), sizeof(sph));
        const u32 local_memory =
            static_cast<u32>(sph.LocalMemorySize()) + sph.common3.shader_local_memory_crs_size;
        const u32 texture_bound = lowered.texture_buffer != ~0U ? lowered.texture_buffer : 0;
        // Maxwell code has a scheduling word at every address that is a multiple of 32, and
        // its header sits 0x30 into the code area: a start address of 0 would shift them.
        const u32 start = Get<u64>(lowered.code, lowered.code.size() - sizeof(u64)) ==
                                  Shader::Ampere::NativeAddressMapMagic
                              ? 0
                              : 0x30;
        PutEnvHeader(out, lowered.code.size(), 0, local_memory, texture_bound, start,
                     static_cast<u32>(stage), start);
        PutBytes(out, lowered.code);
        PutBytes(out, header);
    };
    put_env(vertex, Shader::Stage::VertexB);
    put_env(fragment, Shader::Stage::Fragment);
    Vulkan::GraphicsPipelineCacheKey key;
    std::memset(&key, 0, sizeof(key));
    key.unique_hashes[static_cast<size_t>(Maxwell::ShaderType::VertexB)] = vertex_hash;
    key.unique_hashes[static_cast<size_t>(Maxwell::ShaderType::Pixel)] = fragment_hash;
    PutBytes(out, std::span<const u8>{reinterpret_cast<const u8*>(&key), sizeof(key)});
    return out;
}

namespace {

constexpr std::array<char, 8> CacheMagic{'y', 'u', 'z', 'u', 'c', 'a', 'c', 'h'};
constexpr std::array<char, 8> PlanMagic{'e', 'd', 'e', 'n', 'p', 'l', 'a', 'n'};
constexpr size_t MaxEnvs = 6;

/// Walks the records of the pipeline cache at `path`: `on_record` gets each one as stored and as
/// a plan holds it. Returns false when it is not a cache of the current version; a cut-off last
/// record ends the walk.
bool WalkCache(const std::filesystem::path& path,
               const std::function<void(std::span<const u8>, std::span<const u8>)>& on_record) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        return false;
    }
    const auto end = file.tellg();
    file.seekg(0);
    std::array<char, 8> magic{};
    u32 version{};
    file.read(magic.data(), magic.size()).read(reinterpret_cast<char*>(&version), sizeof(version));
    if (!file || magic != CacheMagic || version != WarmCacheVersion) {
        return false;
    }
    file.exceptions(std::ios::failbit);
    std::vector<u8> raw;
    std::vector<u8> plan;
    try {
        while (file.tellg() != end) {
            const auto begin = file.tellg();
            u32 num_envs{};
            file.read(reinterpret_cast<char*>(&num_envs), sizeof(num_envs));
            if (num_envs == 0 || num_envs > MaxEnvs) {
                break;
            }
            // FileEnvironment finds where each environment ends.
            std::vector<size_t> bounds;
            bool compute = false;
            for (u32 i = 0; i < num_envs; ++i) {
                bounds.push_back(static_cast<size_t>(file.tellg() - begin));
                FileEnvironment env;
                env.Deserialize(file, false);
                compute = i == 0 ? env.ShaderStage() == Shader::Stage::Compute : compute;
            }
            bounds.push_back(static_cast<size_t>(file.tellg() - begin));
            const size_t key_size =
                compute ? sizeof(ComputeKey) : sizeof(Vulkan::GraphicsPipelineCacheKey);
            raw.resize(bounds.back() + key_size);
            file.seekg(begin);
            file.read(reinterpret_cast<char*>(raw.data()), static_cast<std::streamsize>(raw.size()));
            const std::span<const u8> record{raw};
            const auto key = record.last(key_size);
            plan.clear();
            Put<u32>(plan, num_envs);
            size_t stage_hash = 0;
            for (u32 i = 0; i < num_envs; ++i) {
                const auto env = record.subspan(bounds[i], bounds[i + 1] - bounds[i]);
                const auto code = env.subspan(EnvHeaderSize, Get<u64>(env, 0));
                const auto tail = env.subspan(EnvHeaderSize + code.size());
                // The key holds the hashes of its environments in order (one for compute).
                while (!compute && stage_hash + 1 < MaxEnvs &&
                       Get<u64>(key, stage_hash * sizeof(u64)) == 0) {
                    ++stage_hash;
                }
                u64 hash = Get<u64>(key, (std::min)(stage_hash++, MaxEnvs - 1) * sizeof(u64));
                if (code.size() % sizeof(u64) == 0 && code.size() >= 2 * sizeof(u64) &&
                    Get<u64>(code, code.size() - sizeof(u64)) ==
                        Shader::Ampere::NativeAddressMapMagic) {
                    // Lowered native code: what Analyze returns, which a scan can compute too.
                    hash = StageHash(code);
                }
                PutBytes(plan, env.first(EnvHeaderSize));
                Put<u64>(plan, hash);
                Put<u32>(plan, static_cast<u32>(tail.size()));
                PutBytes(plan, tail);
            }
            Put<u32>(plan, static_cast<u32>(key.size()));
            PutBytes(plan, key);
            on_record(record, plan);
        }
    } catch (const std::exception&) {
        // A record cut off by a crash: the ones before it stand.
    }
    return true;
}

u128 Hash128(std::span<const u8> bytes) {
    return Common::CityHash128(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

} // Anonymous namespace

u64 PlanEnv::CodeSize() const {
    return Get<u64>(header, 0);
}

std::optional<size_t> ExportPlan(const std::filesystem::path& cache_file,
                                 const std::filesystem::path& plan_file) {
    std::filesystem::path tmp = plan_file;
    tmp += ".tmp";
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    out.write(PlanMagic.data(), PlanMagic.size())
        .write(reinterpret_cast<const char*>(&PlanVersion), sizeof(PlanVersion))
        .write(reinterpret_cast<const char*>(&WarmCacheVersion), sizeof(WarmCacheVersion));
    size_t records = 0;
    const bool walked = WalkCache(cache_file, [&](std::span<const u8>, std::span<const u8> plan) {
        out.write(reinterpret_cast<const char*>(plan.data()),
                  static_cast<std::streamsize>(plan.size()));
        ++records;
    });
    out.close();
    std::error_code ec;
    if (walked && out) {
        std::filesystem::rename(tmp, plan_file, ec);
        if (!ec) {
            return records;
        }
    }
    std::filesystem::remove(tmp, ec);
    return std::nullopt;
}

std::optional<std::vector<PlanRecord>> ReadPlan(std::span<const u8> plan) {
    size_t at = 0;
    bool bad = false;
    const auto take = [&](u64 size) {
        if (bad || size > plan.size() - at) {
            bad = true;
            return std::span<const u8>{};
        }
        const auto bytes = plan.subspan(at, static_cast<size_t>(size));
        at += bytes.size();
        return bytes;
    };
    const auto take_u32 = [&] {
        const auto bytes = take(sizeof(u32));
        return bad ? u32{} : Get<u32>(bytes, 0);
    };
    const auto magic = take(PlanMagic.size());
    if (bad || std::memcmp(magic.data(), PlanMagic.data(), PlanMagic.size()) != 0 ||
        take_u32() != PlanVersion || take_u32() != WarmCacheVersion || bad) {
        return std::nullopt;
    }
    std::vector<PlanRecord> records;
    while (at != plan.size()) {
        PlanRecord record;
        const u32 num_envs = take_u32();
        if (num_envs == 0 || num_envs > MaxEnvs) {
            return std::nullopt;
        }
        for (u32 i = 0; i < num_envs; ++i) {
            PlanEnv env;
            env.header = take(EnvHeaderSize);
            const auto hash = take(sizeof(u64));
            env.tail = take(take_u32());
            if (bad) {
                return std::nullopt;
            }
            env.hash = Get<u64>(hash, 0);
            record.envs.push_back(env);
        }
        record.key = take(take_u32());
        if (bad) {
            return std::nullopt;
        }
        records.push_back(std::move(record));
    }
    return records;
}

std::vector<u8> FilledRecord(const PlanRecord& record,
                             const std::unordered_map<u64, std::vector<u8>>& code) {
    std::vector<u8> out;
    Put<u32>(out, static_cast<u32>(record.envs.size()));
    for (const PlanEnv& env : record.envs) {
        PutBytes(out, env.header);
        PutBytes(out, code.at(env.hash));
        PutBytes(out, env.tail);
    }
    PutBytes(out, record.key);
    return out;
}

PlanCheck CheckFilledRecords(const std::filesystem::path& cache_file,
                             const std::filesystem::path& filled_file) {
    // Filled records by the plan record they came from.
    std::map<u128, u128> filled;
    WalkCache(filled_file, [&](std::span<const u8> raw, std::span<const u8> plan) {
        filled.insert_or_assign(Hash128(plan), Hash128(raw));
    });
    PlanCheck check;
    WalkCache(cache_file, [&](std::span<const u8> raw, std::span<const u8> plan) {
        const auto it = filled.find(Hash128(plan));
        if (it == filled.end()) {
            ++check.not_found;
        } else if (it->second == Hash128(raw)) {
            ++check.matched;
        } else {
            ++check.mismatched;
        }
    });
    return check;
}

} // namespace VideoCommon::ShaderWarm
