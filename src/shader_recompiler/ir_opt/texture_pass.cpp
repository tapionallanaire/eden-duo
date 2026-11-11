// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <functional>
#include <bit>
#include <numeric>
#include <optional>
#include <string>
#include "common/container/unordered_map.h"
#include <tuple>
#include <limits>
#include <boost/container/small_vector.hpp>

#include "common/logging.h"
#include "common/settings.h"
#include "shader_recompiler/environment.h"
#include "shader_recompiler/frontend/ir/basic_block.h"
#include "shader_recompiler/frontend/ir/breadth_first_search.h"
#include "shader_recompiler/frontend/ir/ir_emitter.h"
#include "shader_recompiler/host_translate_info.h"
#include "shader_recompiler/ir_opt/passes.h"
#include "shader_recompiler/shader_info.h"

namespace Shader::Optimization {
namespace {
struct TextureInst {
    ConstBufferAddr cbuf;
    IR::Inst* inst;
    IR::Block* block;
};

using TextureInstVector = boost::container::small_vector<TextureInst, 24>;

constexpr u32 DESCRIPTOR_SIZE = 8;
constexpr u32 DESCRIPTOR_SIZE_SHIFT = u32(std::countr_zero(DESCRIPTOR_SIZE));
constexpr u32 DESCRIPTOR_MAX_COUNT = 1024;

u32 MaxTableDescriptors() {
    const u32 by_title = Settings::values.bindless_table_max_by_title;
    return by_title != 0 ? (std::min)(by_title, DESCRIPTOR_MAX_COUNT) : DESCRIPTOR_MAX_COUNT;
}

static bool IsMemoryLoad(IR::Opcode opcode);

/// The block holding inst, or nullptr
IR::Block* BlockOf(IR::Program& program, const IR::Inst* inst) {
    for (IR::Block* const block : program.blocks) {
        for (const IR::Inst& candidate : *block) {
            if (&candidate == inst) {
                return block;
            }
        }
    }
    return nullptr;
}

/// A table index the shader computes from constant buffers and immediates alone, as a program
/// the renderer evaluates per draw (IndexProgram); its result is the element index, size_shift and
/// base_elements folded in. A Phi at the root gives one candidate per operand. nullopt: the index
/// depends on per-vertex, per-instance or memory data, or is too large.
std::optional<IndexProgram> CompileIndexProgram(const IR::Value& offset, u32 size_shift,
                                                u32 base_elements) {
    IndexProgram program{};
    size_t n{};
    const auto push{[&](IndexOp op, u32 imm = 0) {
        if (n >= IndexProgram::MAX_INSNS) {
            return false;
        }
        program.insns[n++] = IndexInsn{op, imm};
        return true;
    }};
    std::function<bool(const IR::Value&, int)> emit{[&](const IR::Value& value, int depth) -> bool {
        if (depth > 24) {
            return false;
        }
        if (value.IsImmediate()) {
            switch (value.Type()) {
            case IR::Type::U32:
                return push(IndexOp::Imm, value.U32());
            case IR::Type::F32:
                return push(IndexOp::Imm, std::bit_cast<u32>(value.F32()));
            default:
                return false;
            }
        }
        const IR::Inst* const inst{value.InstRecursive()};
        const auto unary{[&](IndexOp op) { return emit(inst->Arg(0), depth + 1) && push(op); }};
        const auto binary{[&](IndexOp op) {
            return emit(inst->Arg(0), depth + 1) && emit(inst->Arg(1), depth + 1) && push(op);
        }};
        const auto ternary{[&](IndexOp op) {
            return emit(inst->Arg(0), depth + 1) && emit(inst->Arg(1), depth + 1) &&
                   emit(inst->Arg(2), depth + 1) && push(op);
        }};
        switch (inst->GetOpcode()) {
        case IR::Opcode::GetCbufU32:
        case IR::Opcode::GetCbufF32:
            if (!inst->Arg(0).IsImmediate()) {
                return false;
            }
            return emit(inst->Arg(1), depth + 1) && push(IndexOp::CbufU32, inst->Arg(0).U32());
        case IR::Opcode::BitCastU32F32:
        case IR::Opcode::BitCastF32U32:
            return emit(inst->Arg(0), depth + 1);
        case IR::Opcode::PackUint2x32: {
            if (inst->Arg(0).IsImmediate()) {
                return false;
            }
            const IR::Inst* const pair{inst->Arg(0).InstRecursive()};
            if (pair->GetOpcode() != IR::Opcode::CompositeConstructU32x2) {
                return false;
            }
            return emit(pair->Arg(0), depth + 1) && emit(pair->Arg(1), depth + 1) &&
                   push(IndexOp::Pack64);
        }
        case IR::Opcode::CompositeExtractU32x2: {
            if (inst->Arg(0).IsImmediate() || !inst->Arg(1).IsImmediate()) {
                return false;
            }
            const IR::Inst* const unpack{inst->Arg(0).InstRecursive()};
            if (unpack->GetOpcode() != IR::Opcode::UnpackUint2x32) {
                return false;
            }
            return emit(unpack->Arg(0), depth + 1) && push(IndexOp::Word, inst->Arg(1).U32());
        }
        case IR::Opcode::IAdd32:
            return binary(IndexOp::IAdd);
        case IR::Opcode::ISub32:
            return binary(IndexOp::ISub);
        case IR::Opcode::IMul32:
            return binary(IndexOp::IMul);
        case IR::Opcode::ShiftLeftLogical32:
            return binary(IndexOp::Shl);
        case IR::Opcode::ShiftRightLogical32:
            return binary(IndexOp::Shr);
        case IR::Opcode::ShiftRightArithmetic32:
            return binary(IndexOp::Sar);
        case IR::Opcode::BitwiseAnd32:
            return binary(IndexOp::And);
        case IR::Opcode::BitwiseOr32:
            return binary(IndexOp::Or);
        case IR::Opcode::BitwiseXor32:
            return binary(IndexOp::Xor);
        case IR::Opcode::UMin32:
            return binary(IndexOp::UMin);
        case IR::Opcode::UMax32:
            return binary(IndexOp::UMax);
        case IR::Opcode::SMin32:
            return binary(IndexOp::SMin);
        case IR::Opcode::SMax32:
            return binary(IndexOp::SMax);
        case IR::Opcode::ShiftLeftLogical64:
            return binary(IndexOp::Shl64);
        case IR::Opcode::ShiftRightLogical64:
            return binary(IndexOp::Shr64);
        case IR::Opcode::IAdd64:
            return binary(IndexOp::Add64);
        case IR::Opcode::FPTrunc32:
            return unary(IndexOp::FTrunc);
        case IR::Opcode::FPFloor32:
            return unary(IndexOp::FFloor);
        case IR::Opcode::FPCeil32:
            return unary(IndexOp::FCeil);
        case IR::Opcode::FPAbs32:
            return unary(IndexOp::FAbs);
        case IR::Opcode::FPNeg32:
            return unary(IndexOp::FNeg);
        case IR::Opcode::FPSaturate32:
            return unary(IndexOp::FSat);
        case IR::Opcode::FPAdd32:
            return binary(IndexOp::FAdd);
        case IR::Opcode::FPMul32:
            return binary(IndexOp::FMul);
        case IR::Opcode::FPMin32:
            return binary(IndexOp::FMin);
        case IR::Opcode::FPMax32:
            return binary(IndexOp::FMax);
        case IR::Opcode::FPClamp32:
            return ternary(IndexOp::FClamp);
        case IR::Opcode::ConvertU32F32:
            return unary(IndexOp::U32F32);
        case IR::Opcode::ConvertS32F32:
            return unary(IndexOp::S32F32);
        case IR::Opcode::ConvertF32U32:
            return unary(IndexOp::F32U32);
        case IR::Opcode::ConvertF32S32:
            return unary(IndexOp::F32S32);
        case IR::Opcode::BitFieldUExtract:
            return ternary(IndexOp::BfeU);
        case IR::Opcode::BitFieldSExtract:
            return ternary(IndexOp::BfeS);
        default:
            return false;
        }
    }};
    const auto candidate{[&](const IR::Value& value) {
        if (program.num_candidates >= IndexProgram::MAX_CANDIDATES) {
            return false;
        }
        program.starts[program.num_candidates] = static_cast<u8>(n);
        // byte offset -> element index, then the rebase (texture pass base_elements)
        if (!emit(value, 0) || !push(IndexOp::Imm, size_shift) || !push(IndexOp::Shr) ||
            !push(IndexOp::Imm, base_elements) || !push(IndexOp::IAdd)) {
            return false;
        }
        ++program.num_candidates;
        program.starts[program.num_candidates] = static_cast<u8>(n);
        return true;
    }};
    const IR::Inst* const root{offset.IsImmediate() ? nullptr : offset.InstRecursive()};
    if (root != nullptr && root->GetOpcode() == IR::Opcode::Phi) {
        for (size_t i = 0; i < root->NumArgs(); ++i) {
            const IR::Value arg{root->Arg(i)};
            if (!arg.IsImmediate() && arg.InstRecursive()->GetOpcode() == IR::Opcode::Phi) {
                return std::nullopt;
            }
            if (!candidate(arg)) {
                return std::nullopt;
            }
        }
    } else if (!candidate(offset)) {
        return std::nullopt;
    }
    return program;
}

u32 DynamicDescriptorSizeShift(const IR::U32& dynamic_offset) {
    const IR::Inst* const inst = dynamic_offset.InstRecursive();
    if (!inst || inst->GetOpcode() != IR::Opcode::ShiftLeftLogical32)
        return DESCRIPTOR_SIZE_SHIFT;
    const IR::Value shift = inst->Arg(1);
    if (!shift.IsImmediate())
        return DESCRIPTOR_SIZE_SHIFT;
    const u32 size_shift = shift.U32();
    return size_shift >= DESCRIPTOR_SIZE_SHIFT && size_shift < 31 ? size_shift : DESCRIPTOR_SIZE_SHIFT;
}

u32 DynamicDescriptorCount(u32 base_offset, u32 size_shift, u32 max_descriptors) {
    auto const descriptor_limit = (std::max)(1U, max_descriptors);
    auto const max_cbuf_bytes = 16 * descriptor_limit;
    if (size_shift >= 31 || base_offset >= max_cbuf_bytes)
        return 1;
    auto const stride = 1U << size_shift;
    auto const available = max_cbuf_bytes - base_offset;
    if (available < DESCRIPTOR_SIZE)
        return 1;
    auto const available_count = 1U + (available - DESCRIPTOR_SIZE) / stride;
    return std::min(descriptor_limit, available_count);
}

// The values an offset expression can take, as stride * k + phase for integers k: run-time inputs
// count as any integer, immediates as themselves (signed), adds, multiplies and left shifts by
// immediates combine them. Hogwarts Legacy indexes handle tables as attr * 0x48 + field (or the
// XMAD form ((hi * 0x48) << 16) + (lo * 0x48 + field)): only one 8-byte element in nine is reached.
struct OffsetLattice {
    u64 stride; // 0: constant
    s64 phase;
};

OffsetLattice AnalyzeOffset(const IR::Value& value, int depth = 0) {
    if (value.IsImmediate()) {
        return {0, static_cast<s64>(static_cast<s32>(value.U32()))};
    }
    const IR::Inst* const inst{value.TryInstRecursive()};
    if (inst == nullptr || depth > 8) {
        return {1, 0};
    }
    switch (inst->GetOpcode()) {
    case IR::Opcode::IAdd32: {
        const OffsetLattice a{AnalyzeOffset(inst->Arg(0), depth + 1)};
        const OffsetLattice b{AnalyzeOffset(inst->Arg(1), depth + 1)};
        return {std::gcd(a.stride, b.stride), a.phase + b.phase};
    }
    case IR::Opcode::IMul32: {
        const IR::Value lhs{inst->Arg(0)};
        const IR::Value rhs{inst->Arg(1)};
        if (!lhs.IsImmediate() && !rhs.IsImmediate()) {
            return {1, 0};
        }
        const u64 m{lhs.IsImmediate() ? lhs.U32() : rhs.U32()};
        const OffsetLattice a{AnalyzeOffset(lhs.IsImmediate() ? rhs : lhs, depth + 1)};
        return {a.stride * m, a.phase * static_cast<s64>(m)};
    }
    case IR::Opcode::ShiftLeftLogical32: {
        if (!inst->Arg(1).IsImmediate() || inst->Arg(1).U32() >= 32) {
            return {1, 0};
        }
        const u32 shift{inst->Arg(1).U32()};
        const OffsetLattice a{AnalyzeOffset(inst->Arg(0), depth + 1)};
        return {a.stride << shift, a.phase * (s64{1} << shift)};
    }
    default:
        return {1, 0};
    }
}

u32 SaturatingSub(u32 lhs, u32 rhs) {
    return lhs > rhs ? lhs - rhs : 0;
}

template <typename T>
[[nodiscard]] u32 StaticDescriptorCount(T const& descriptors) noexcept {
    return std::accumulate(descriptors.cbegin(), descriptors.cend(), 0U, [](auto const& acc, auto const& e) {
        return acc + (e.count <= 1 ? e.count : 0);
    });
}

u32 DynamicSampledTextureCap(const Info& info, const HostTranslateInfo& host_info, u32 dynamic_arrays) {
    auto const sampled_limit = (std::max)(1U, std::min(host_info.max_per_stage_descriptor_sampled_images,
                                                       host_info.max_descriptor_set_sampled_images));
    auto const resource_limit = (std::max)(1U, host_info.max_per_stage_resources);
    if (dynamic_arrays > 0) {
        auto const sampled_static_count = StaticDescriptorCount(info.texture_buffer_descriptors) + StaticDescriptorCount(info.texture_descriptors);
        auto const resource_static_count =
            NumDescriptors(info.constant_buffer_descriptors)
            + NumDescriptors(info.storage_buffers_descriptors)
            + sampled_static_count + NumDescriptors(info.image_buffer_descriptors)
            + NumDescriptors(info.image_descriptors);
        auto const sampled_budget = SaturatingSub(sampled_limit, sampled_static_count);
        auto const resource_budget = SaturatingSub(resource_limit, resource_static_count);
        auto const sampled_cap = sampled_budget / dynamic_arrays;
        auto const resource_cap = resource_budget / dynamic_arrays;
        return (std::max)(1U, (std::min)(sampled_cap, resource_cap));
    }
    return (std::min)({MaxTableDescriptors(), sampled_limit, resource_limit});
}

IR::Opcode IndexedInstruction(const IR::Inst& inst) {
    switch (inst.GetOpcode()) {
    case IR::Opcode::BindlessImageSampleImplicitLod:
    case IR::Opcode::BoundImageSampleImplicitLod:
        return IR::Opcode::ImageSampleImplicitLod;
    case IR::Opcode::BoundImageSampleExplicitLod:
    case IR::Opcode::BindlessImageSampleExplicitLod:
        return IR::Opcode::ImageSampleExplicitLod;
    case IR::Opcode::BoundImageSampleDrefImplicitLod:
    case IR::Opcode::BindlessImageSampleDrefImplicitLod:
        return IR::Opcode::ImageSampleDrefImplicitLod;
    case IR::Opcode::BoundImageSampleDrefExplicitLod:
    case IR::Opcode::BindlessImageSampleDrefExplicitLod:
        return IR::Opcode::ImageSampleDrefExplicitLod;
    case IR::Opcode::BindlessImageGather:
    case IR::Opcode::BoundImageGather:
        return IR::Opcode::ImageGather;
    case IR::Opcode::BindlessImageGatherDref:
    case IR::Opcode::BoundImageGatherDref:
        return IR::Opcode::ImageGatherDref;
    case IR::Opcode::BindlessImageFetch:
    case IR::Opcode::BoundImageFetch:
        return IR::Opcode::ImageFetch;
    case IR::Opcode::BoundImageQueryDimensions:
    case IR::Opcode::BindlessImageQueryDimensions:
        return IR::Opcode::ImageQueryDimensions;
    case IR::Opcode::BoundImageQueryLod:
    case IR::Opcode::BindlessImageQueryLod:
        return IR::Opcode::ImageQueryLod;
    case IR::Opcode::BoundImageGradient:
    case IR::Opcode::BindlessImageGradient:
        return IR::Opcode::ImageGradient;
    case IR::Opcode::BoundImageRead:
    case IR::Opcode::BindlessImageRead:
        return IR::Opcode::ImageRead;
    case IR::Opcode::BoundImageWrite:
    case IR::Opcode::BindlessImageWrite:
        return IR::Opcode::ImageWrite;
    case IR::Opcode::BoundImageAtomicIAdd32:
    case IR::Opcode::BindlessImageAtomicIAdd32:
        return IR::Opcode::ImageAtomicIAdd32;
    case IR::Opcode::BoundImageAtomicSMin32:
    case IR::Opcode::BindlessImageAtomicSMin32:
        return IR::Opcode::ImageAtomicSMin32;
    case IR::Opcode::BoundImageAtomicUMin32:
    case IR::Opcode::BindlessImageAtomicUMin32:
        return IR::Opcode::ImageAtomicUMin32;
    case IR::Opcode::BoundImageAtomicSMax32:
    case IR::Opcode::BindlessImageAtomicSMax32:
        return IR::Opcode::ImageAtomicSMax32;
    case IR::Opcode::BoundImageAtomicUMax32:
    case IR::Opcode::BindlessImageAtomicUMax32:
        return IR::Opcode::ImageAtomicUMax32;
    case IR::Opcode::BoundImageAtomicInc32:
    case IR::Opcode::BindlessImageAtomicInc32:
        return IR::Opcode::ImageAtomicInc32;
    case IR::Opcode::BoundImageAtomicDec32:
    case IR::Opcode::BindlessImageAtomicDec32:
        return IR::Opcode::ImageAtomicDec32;
    case IR::Opcode::BoundImageAtomicAnd32:
    case IR::Opcode::BindlessImageAtomicAnd32:
        return IR::Opcode::ImageAtomicAnd32;
    case IR::Opcode::BoundImageAtomicOr32:
    case IR::Opcode::BindlessImageAtomicOr32:
        return IR::Opcode::ImageAtomicOr32;
    case IR::Opcode::BoundImageAtomicXor32:
    case IR::Opcode::BindlessImageAtomicXor32:
        return IR::Opcode::ImageAtomicXor32;
    case IR::Opcode::BoundImageAtomicExchange32:
    case IR::Opcode::BindlessImageAtomicExchange32:
        return IR::Opcode::ImageAtomicExchange32;
    default:
        return IR::Opcode::Void;
    }
}

bool IsStorageImageOpcode(IR::Opcode opcode) {
    switch (opcode) {
    case IR::Opcode::ImageRead:
    case IR::Opcode::ImageAtomicIAdd32:
    case IR::Opcode::ImageAtomicSMin32:
    case IR::Opcode::ImageAtomicUMin32:
    case IR::Opcode::ImageAtomicSMax32:
    case IR::Opcode::ImageAtomicUMax32:
    case IR::Opcode::ImageAtomicInc32:
    case IR::Opcode::ImageAtomicDec32:
    case IR::Opcode::ImageAtomicAnd32:
    case IR::Opcode::ImageAtomicOr32:
    case IR::Opcode::ImageAtomicXor32:
    case IR::Opcode::ImageAtomicExchange32:
    case IR::Opcode::ImageWrite:
        return true;
    default:
        return false;
    }
}

u32 DynamicSampledTextureArrayCount(const TextureInstVector& to_replace) {
    u32 count{};
    for (const TextureInst& inst : to_replace) {
        const auto flags{inst.inst->Flags<IR::TextureInstInfo>()};
        if (inst.cbuf.count > 1 && !IsStorageImageOpcode(IndexedInstruction(*inst.inst)) &&
            flags.type != TextureType::Buffer) {
            ++count;
        }
    }
    return count;
}

bool IsBindless(const IR::Inst& inst) {
    switch (inst.GetOpcode()) {
    case IR::Opcode::BindlessImageSampleImplicitLod:
    case IR::Opcode::BindlessImageSampleExplicitLod:
    case IR::Opcode::BindlessImageSampleDrefImplicitLod:
    case IR::Opcode::BindlessImageSampleDrefExplicitLod:
    case IR::Opcode::BindlessImageGather:
    case IR::Opcode::BindlessImageGatherDref:
    case IR::Opcode::BindlessImageFetch:
    case IR::Opcode::BindlessImageQueryDimensions:
    case IR::Opcode::BindlessImageQueryLod:
    case IR::Opcode::BindlessImageGradient:
    case IR::Opcode::BindlessImageRead:
    case IR::Opcode::BindlessImageWrite:
    case IR::Opcode::BindlessImageAtomicIAdd32:
    case IR::Opcode::BindlessImageAtomicSMin32:
    case IR::Opcode::BindlessImageAtomicUMin32:
    case IR::Opcode::BindlessImageAtomicSMax32:
    case IR::Opcode::BindlessImageAtomicUMax32:
    case IR::Opcode::BindlessImageAtomicInc32:
    case IR::Opcode::BindlessImageAtomicDec32:
    case IR::Opcode::BindlessImageAtomicAnd32:
    case IR::Opcode::BindlessImageAtomicOr32:
    case IR::Opcode::BindlessImageAtomicXor32:
    case IR::Opcode::BindlessImageAtomicExchange32:
        return true;
    case IR::Opcode::BoundImageSampleImplicitLod:
    case IR::Opcode::BoundImageSampleExplicitLod:
    case IR::Opcode::BoundImageSampleDrefImplicitLod:
    case IR::Opcode::BoundImageSampleDrefExplicitLod:
    case IR::Opcode::BoundImageGather:
    case IR::Opcode::BoundImageGatherDref:
    case IR::Opcode::BoundImageFetch:
    case IR::Opcode::BoundImageQueryDimensions:
    case IR::Opcode::BoundImageQueryLod:
    case IR::Opcode::BoundImageGradient:
    case IR::Opcode::BoundImageRead:
    case IR::Opcode::BoundImageWrite:
    case IR::Opcode::BoundImageAtomicIAdd32:
    case IR::Opcode::BoundImageAtomicSMin32:
    case IR::Opcode::BoundImageAtomicUMin32:
    case IR::Opcode::BoundImageAtomicSMax32:
    case IR::Opcode::BoundImageAtomicUMax32:
    case IR::Opcode::BoundImageAtomicInc32:
    case IR::Opcode::BoundImageAtomicDec32:
    case IR::Opcode::BoundImageAtomicAnd32:
    case IR::Opcode::BoundImageAtomicOr32:
    case IR::Opcode::BoundImageAtomicXor32:
    case IR::Opcode::BoundImageAtomicExchange32:
        return false;
    default:
        throw InvalidArgument("Invalid opcode {}", inst.GetOpcode());
    }
}

bool IsTextureInstruction(const IR::Inst& inst) {
    return IndexedInstruction(inst) != IR::Opcode::Void;
}
// Per-pass caches

static inline u32 ReadCbufCached(Environment& env, u32 index, u32 offset) {
    const CbufWordKey k{index, offset};
    if (auto it = env.cbuf_word_cache.find(k); it != env.cbuf_word_cache.end())
        return it->second;
    const u32 v = env.ReadCbufValue(index, offset);
    env.cbuf_word_cache.emplace(k, v);
    return v;
}

static inline u32 GetTextureHandleCached(Environment& env, const ConstBufferAddr& cbuf) {
    // Must all be uniquely different variables
    // If has secondary, then it will be cbuf.secondary_{index|offset}, else its 0.
    // So we can just hand it out the raw variable without using sec_idx or sec_off
    // because comparing 0 against 0 will yield true.
    const HandleKey hk{cbuf.index, cbuf.offset, cbuf.shift_left, cbuf.secondary_index, cbuf.secondary_offset, cbuf.secondary_shift_left, cbuf.count, cbuf.has_secondary};
    if (auto it = env.handle_cache.find(hk); it != env.handle_cache.end())
        return it->second;
    const u32 sec_idx = cbuf.has_secondary ? cbuf.secondary_index  : cbuf.index;
    const u32 sec_off = cbuf.has_secondary ? cbuf.secondary_offset : cbuf.offset;
    const u32 lhs = ReadCbufCached(env, cbuf.index, cbuf.offset) << cbuf.shift_left;
    const u32 rhs = ReadCbufCached(env, sec_idx, sec_off) << cbuf.secondary_shift_left;
    const u32 handle = lhs | rhs;
    env.handle_cache.emplace(hk, handle);
    return handle;
}

// Cached variants of existing helpers
// A run-time handle names no descriptor here: a 2D colour texture, as for attribute handles.
static inline TextureType ReadTextureTypeCached(Environment& env, const ConstBufferAddr& cbuf) {
    if (cbuf.dynamic_handle) {
        return TextureType::Color2D;
    }
    return env.ReadTextureType(GetTextureHandleCached(env, cbuf));
}
static inline TexturePixelFormat ReadTexturePixelFormatCached(Environment& env,
                                                                const ConstBufferAddr& cbuf) {
    if (cbuf.dynamic_handle) {
        return TexturePixelFormat::A8B8G8R8_UNORM;
    }
    return env.ReadTexturePixelFormat(GetTextureHandleCached(env, cbuf));
}
static inline bool IsTexturePixelFormatIntegerCached(Environment& env,
                                                        const ConstBufferAddr& cbuf) {
    if (cbuf.dynamic_handle) {
        return false;
    }
    return env.IsTexturePixelFormatInteger(GetTextureHandleCached(env, cbuf));
}


std::optional<ConstBufferAddr> Track(const IR::Value& value, Environment& env, const HostTranslateInfo& host_info);
static inline std::optional<ConstBufferAddr> TrackCached(const IR::Value& v, Environment& env, const HostTranslateInfo& host_info) {
    if (const IR::Inst* key = v.InstRecursive()) {
        if (auto it = env.track_cache.find(key); it != env.track_cache.end()) return it->second;
        auto found = Track(v, env, host_info);
        if (found) env.track_cache.emplace(key, *found);
        return found;
    }
    return Track(v, env, host_info);
}

std::optional<ConstBufferAddr> TryGetConstBuffer(const IR::Inst* inst, Environment& env, const HostTranslateInfo& host_info);

// A value loaded from memory is not known when the shader is compiled, and the load's operands are
// its address, not the value: a handle must never be tracked through one. Hogwarts Legacy ORs a
// texture index loaded from a storage buffer with a sampler word (LoadStorage32(ssbo, base +
// i * 8) | c2[0x578]); searching the load's address found the storage buffer's own address in
// c0[0x270] and took (address << 4) as the texture: a descriptor far past the end of the TIC, so
// the shader was compiled with whatever type and format that memory held (format 90/105 asserts).
static bool IsMemoryLoad(IR::Opcode opcode) {
    switch (opcode) {
    case IR::Opcode::LoadGlobalU8:
    case IR::Opcode::LoadGlobalS8:
    case IR::Opcode::LoadGlobalU16:
    case IR::Opcode::LoadGlobalS16:
    case IR::Opcode::LoadGlobal32:
    case IR::Opcode::LoadGlobal64:
    case IR::Opcode::LoadGlobal128:
    case IR::Opcode::LoadStorageU8:
    case IR::Opcode::LoadStorageS8:
    case IR::Opcode::LoadStorageU16:
    case IR::Opcode::LoadStorageS16:
    case IR::Opcode::LoadStorage32:
    case IR::Opcode::LoadStorage64:
    case IR::Opcode::LoadStorage128:
    case IR::Opcode::LoadLocal:
    case IR::Opcode::LoadSharedU8:
    case IR::Opcode::LoadSharedS8:
    case IR::Opcode::LoadSharedU16:
    case IR::Opcode::LoadSharedS16:
    case IR::Opcode::LoadSharedU32:
    case IR::Opcode::LoadSharedU64:
    case IR::Opcode::LoadSharedU128:
        return true;
    default:
        return false;
    }
}

/// True when the value is computed from a memory load (searched to a bounded depth).
static bool DependsOnMemoryLoad(const IR::Value& value) {
    if (value.IsImmediate()) {
        return false;
    }
    boost::container::small_vector<const IR::Inst*, 16> visited;
    std::queue<const IR::Inst*> queue;
    queue.push(value.InstRecursive());
    while (!queue.empty() && visited.size() < 256) {
        const IR::Inst* const inst{queue.front()};
        queue.pop();
        if (IsMemoryLoad(inst->GetOpcode())) {
            return true;
        }
        for (size_t arg = 0; arg < inst->NumArgs(); ++arg) {
            const IR::Value arg_value{inst->Arg(arg)};
            if (arg_value.IsImmediate()) {
                continue;
            }
            const IR::Inst* const arg_inst{arg_value.InstRecursive()};
            if (std::ranges::find(visited, arg_inst) == visited.end()) {
                visited.push_back(arg_inst);
                queue.push(arg_inst);
            }
        }
    }
    return false;
}

/// A handle table in storage memory: value is LoadStorage32(ssbo, offset); word is the tracked
/// sampler word it is ORed with. Elements are 8 bytes (two words, the handle first). A load merged
/// by a Phi with another path is not one: binding the table there blacked out the lit materials.
std::optional<ConstBufferAddr> StorageTable(const IR::Value& value, const std::optional<ConstBufferAddr>& word) {
    if (value.IsImmediate() || !word || word->count != 1 || word->has_secondary ||
        word->dynamic_handle) {
        return std::nullopt;
    }
    const IR::Inst* const load{value.InstRecursive()};
    if (load->GetOpcode() != IR::Opcode::LoadStorage32 || !load->Arg(0).IsImmediate()) {
        return std::nullopt;
    }
    constexpr u32 STORAGE_ELEMENT_SHIFT{3};
    return ConstBufferAddr{
        .index = word->index,
        .offset = word->offset,
        .shift_left = 0,
        .secondary_index = word->index,
        .secondary_offset = word->offset,
        .secondary_shift_left = word->shift_left,
        .dynamic_offset = IR::U32{load->Arg(1)},
        .count = DynamicDescriptorCount(0, STORAGE_ELEMENT_SHIFT, MaxTableDescriptors()),
        .has_secondary = true,
        .dynamic_handle = true,
        .storage_table = true,
        .storage_index = load->Arg(0).U32(),
    };
}

std::optional<ConstBufferAddr> Track(const IR::Value& value, Environment& env, const HostTranslateInfo& host_info) {
    if (value.IsImmediate()) {
        return std::nullopt;
    }
    // IR::BreadthFirstSearch, except that it does not descend into memory loads.
    boost::container::small_vector<const IR::Inst*, 8> visited;
    std::queue<const IR::Inst*> queue;
    queue.push(value.InstRecursive());
    while (!queue.empty()) {
        const IR::Inst* const inst{queue.front()};
        queue.pop();
        if (IsMemoryLoad(inst->GetOpcode())) {
            continue;
        }
        if (const std::optional result = TryGetConstBuffer(inst, env, host_info)) {
            return result;
        }
        for (size_t arg = inst->NumArgs(); arg--;) {
            const IR::Value arg_value{inst->Arg(arg)};
            if (arg_value.IsImmediate()) {
                continue;
            }
            const IR::Inst* const arg_inst{arg_value.InstRecursive()};
            if (std::ranges::find(visited, arg_inst) == visited.end()) {
                visited.push_back(arg_inst);
                queue.push(arg_inst);
            }
        }
    }
    return std::nullopt;
}

std::optional<u32> TryGetConstant(IR::Value& value, Environment& env) {
    const IR::Inst* inst = value.InstRecursive();
    if (inst->GetOpcode() != IR::Opcode::GetCbufU32) {
        return std::nullopt;
    }
    const IR::Value index{inst->Arg(0)};
    const IR::Value offset{inst->Arg(1)};
    if (!index.IsImmediate()) {
        return std::nullopt;
    }
    if (!offset.IsImmediate()) {
        return std::nullopt;
    }
    const auto index_number = index.U32();
    if (index_number != 1) {
        return std::nullopt;
    }
    const auto offset_number = offset.U32();
    return ReadCbufCached(env, index_number, offset_number);
}

std::optional<ConstBufferAddr> TryGetConstBuffer(const IR::Inst* inst, Environment& env, const HostTranslateInfo& host_info) {
    switch (inst->GetOpcode()) {
    default:
        return std::nullopt;
    case IR::Opcode::BitwiseOr32: {
        std::optional lhs{TrackCached(inst->Arg(0), env, host_info)};
        std::optional rhs{TrackCached(inst->Arg(1), env, host_info)};
        if (!lhs || !rhs) {
            // Hogwarts Legacy: a texture handle loaded from a storage buffer (8-byte elements
            // indexed per instance) ORed with a sampler word from a constant buffer. The table is
            // bound as a descriptor array and the shader indexes it with the load's offset.
            if (const auto table = StorageTable(lhs ? inst->Arg(1) : inst->Arg(0), lhs ? lhs : rhs)) {
                return table;
            }
            // Other loads: bind by the word it can see, and mark the handle run-time only so its
            // type and format are not read from whatever descriptor that word alone names.
            if (lhs && !rhs && DependsOnMemoryLoad(inst->Arg(1))) {
                lhs->dynamic_handle = true;
                return lhs;
            }
            if (rhs && !lhs && DependsOnMemoryLoad(inst->Arg(0))) {
                rhs->dynamic_handle = true;
                return rhs;
            }
            return std::nullopt;
        }
        if (lhs->has_secondary || rhs->has_secondary) {
            // handle = Phi(table | word, ...) | word: ORing a word the tracked side already
            // includes changes nothing (Hogwarts Legacy, sampler word cbuf2 + 0x590..0x5b8).
            const auto includes = [](const ConstBufferAddr& with, const ConstBufferAddr& word) {
                const bool scalar = word.count == 1 && !word.has_secondary;
                const bool is_primary = with.index == word.index && with.offset == word.offset &&
                                        with.shift_left == word.shift_left;
                const bool is_secondary = with.secondary_index == word.index &&
                                          with.secondary_offset == word.offset &&
                                          with.secondary_shift_left == word.shift_left;
                return with.has_secondary && scalar && (is_primary || is_secondary);
            };
            if (includes(*lhs, *rhs)) {
                return lhs;
            }
            if (includes(*rhs, *lhs)) {
                return rhs;
            }
            return std::nullopt;
        }
        if (lhs->count > 1 && rhs->count > 1) {
            return std::nullopt;
        }
        // One side may be a handle table indexed at run time, ORed with one fixed word: Hogwarts
        // Legacy reads a per-instance texture handle (cbuf[attr * stride + field]) and ORs in a
        // separate sampler handle. The table is the primary array; the fixed word is the secondary
        // of every element. Bailing here let the search below settle on the sampler word alone:
        // texture 0, a null descriptor, zeros, and NaN normals drawn black.
        if (rhs->count > 1 || (lhs->count == 1 && (lhs->shift_left > 0 || lhs->index > rhs->index ||
                                                   lhs->offset > rhs->offset))) {
            std::swap(lhs, rhs);
        }
        return ConstBufferAddr{
            .index = lhs->index,
            .offset = lhs->offset,
            .shift_left = lhs->shift_left,
            .secondary_index = rhs->index,
            .secondary_offset = rhs->offset,
            .secondary_shift_left = rhs->shift_left,
            .dynamic_offset = lhs->dynamic_offset,
            .count = lhs->count,
            .has_secondary = true,
            .dynamic_handle = lhs->dynamic_handle || rhs->dynamic_handle,
        };
    }
    case IR::Opcode::ShiftLeftLogical32: {
        const IR::Value shift{inst->Arg(1)};
        if (!shift.IsImmediate()) {
            return std::nullopt;
        }
        std::optional lhs{TrackCached(inst->Arg(0), env, host_info)};
        if (lhs) {
            lhs->shift_left = shift.U32();
        }
        return lhs;
    }
    case IR::Opcode::BitwiseAnd32: {
        IR::Value op1{inst->Arg(0)};
        IR::Value op2{inst->Arg(1)};
        if (op1.IsImmediate()) {
            std::swap(op1, op2);
        }
        if (!op2.IsImmediate() && !op1.IsImmediate()) {
            do {
                auto try_index = TryGetConstant(op1, env);
                if (try_index) {
                    op1 = op2;
                    op2 = IR::Value{*try_index};
                    break;
                }
                auto try_index_2 = TryGetConstant(op2, env);
                if (try_index_2) {
                    op2 = IR::Value{*try_index_2};
                    break;
                }
                return std::nullopt;
            } while (false);
        }
        std::optional lhs{TrackCached(op1, env, host_info)};
        if (lhs) {
            lhs->shift_left = static_cast<u32>(std::countr_zero(op2.U32()));
        }
        return lhs;
        break;
    }
    case IR::Opcode::GetCbufU32x2:
    case IR::Opcode::GetCbufU32:
        break;
    }
    const IR::Value index{inst->Arg(0)};
    const IR::Value offset{inst->Arg(1)};
    if (!index.IsImmediate()) {
        // Reading a bindless texture from variable indices is valid
        // but not supported here at the moment
        return std::nullopt;
    }
    if (offset.IsImmediate()) {
        return ConstBufferAddr{
            .index = index.U32(),
            .offset = offset.U32(),
            .shift_left = 0,
            .secondary_index = 0,
            .secondary_offset = 0,
            .secondary_shift_left = 0,
            .dynamic_offset = {},
            .count = 1,
            .has_secondary = false,
        };
    }
    IR::Inst* const offset_inst{offset.InstRecursive()};
    u32 base_offset{};
    IR::U32 dynamic_offset;
    // A constant part is a base only when it lies inside a constant buffer (64 KiB): Hogwarts
    // Legacy reads c7[index * stride - 8], and taking -8 as the base made a one-element table
    // read at c7 - 8 (zero, texture 0, rocks drawn black).
    const auto is_base = [](const IR::Value& v) { return v.IsImmediate() && v.U32() < 0x10000; };
    if (offset_inst->GetOpcode() == IR::Opcode::IAdd32 && is_base(offset_inst->Arg(0))) {
        base_offset = offset_inst->Arg(0).U32();
        dynamic_offset = IR::U32{offset_inst->Arg(1)};
    } else if (offset_inst->GetOpcode() == IR::Opcode::IAdd32 && is_base(offset_inst->Arg(1))) {
        base_offset = offset_inst->Arg(1).U32();
        dynamic_offset = IR::U32{offset_inst->Arg(0)};
    } else {
        // A table read at a run-time offset with no top-level constant part: attr * stride, or
        // attr * stride + field built from 16-bit multiplies (XMAD), where the constant sits
        // inside. The whole offset indexes a table starting at 0.
        dynamic_offset = IR::U32{offset};
    }
    auto const size_shift = DynamicDescriptorSizeShift(dynamic_offset);
    auto const sampled_limit = (std::max)(1U, (std::min)(host_info.max_per_stage_descriptor_sampled_images,
                                                         host_info.max_descriptor_set_sampled_images));
    auto const resource_limit = (std::max)(1U, host_info.max_per_stage_resources);
    return ConstBufferAddr{
        .index = index.U32(),
        .offset = base_offset,
        .shift_left = 0,
        .secondary_index = 0,
        .secondary_offset = 0,
        .secondary_shift_left = 0,
        .dynamic_offset = dynamic_offset,
        .count = DynamicDescriptorCount(base_offset, size_shift, (std::min)({MaxTableDescriptors(), sampled_limit, resource_limit})),
        .has_secondary = false,
    };
}

TextureInst MakeInst(Environment& env, IR::Block* block, IR::Inst& inst, const HostTranslateInfo& host_info) {
    ConstBufferAddr addr;
    if (IsBindless(inst)) {
        const std::optional<ConstBufferAddr> track_addr{TrackCached(inst.Arg(0), env, host_info)};
        if (!track_addr) {
            throw NotImplementedException("Failed to track bindless texture constant buffer");
        } else {
            addr = *track_addr;
        }
    } else {
        addr = ConstBufferAddr{
            .index = env.TextureBoundBuffer(),
            .offset = inst.Arg(0).U32(),
            .shift_left = 0,
            .secondary_index = 0,
            .secondary_offset = 0,
            .secondary_shift_left = 0,
            .dynamic_offset = {},
            .count = 1,
            .has_secondary = false,
        };
    }
    return TextureInst{
        .cbuf = addr,
        .inst = &inst,
        .block = block,
    };
}

u32 GetTextureHandle(Environment& env, const ConstBufferAddr& cbuf) {
    const u32 secondary_index{cbuf.has_secondary ? cbuf.secondary_index : cbuf.index};
    const u32 secondary_offset{cbuf.has_secondary ? cbuf.secondary_offset : cbuf.offset};
    const u32 lhs_raw{env.ReadCbufValue(cbuf.index, cbuf.offset) << cbuf.shift_left};
    const u32 rhs_raw{env.ReadCbufValue(secondary_index, secondary_offset)
                      << cbuf.secondary_shift_left};
    return lhs_raw | rhs_raw;
}

[[maybe_unused]] TextureType ReadTextureType(Environment& env, const ConstBufferAddr& cbuf) {
    return env.ReadTextureType(GetTextureHandle(env, cbuf));
}

[[maybe_unused]] TexturePixelFormat ReadTexturePixelFormat(Environment& env, const ConstBufferAddr& cbuf) {
    return env.ReadTexturePixelFormat(GetTextureHandle(env, cbuf));
}

[[maybe_unused]] bool IsTexturePixelFormatInteger(Environment& env, const ConstBufferAddr& cbuf) {
    return env.IsTexturePixelFormatInteger(GetTextureHandle(env, cbuf));
}

class Descriptors {
public:
    explicit Descriptors(TextureBufferDescriptors& texture_buffer_descriptors_,
                         ImageBufferDescriptors& image_buffer_descriptors_,
                         TextureDescriptors& texture_descriptors_,
                         ImageDescriptors& image_descriptors_,
                         IndexPrograms& index_programs_)
        : texture_buffer_descriptors{texture_buffer_descriptors_},
          image_buffer_descriptors{image_buffer_descriptors_},
          texture_descriptors{texture_descriptors_}, image_descriptors{image_descriptors_},
          index_programs{index_programs_} {}

    u32 Add(const TextureBufferDescriptor& desc) {
        return Add(texture_buffer_descriptors, desc, [&desc](const auto& existing) {
            return desc.cbuf_index == existing.cbuf_index &&
                   desc.cbuf_offset == existing.cbuf_offset &&
                   desc.shift_left == existing.shift_left &&
                   desc.secondary_cbuf_index == existing.secondary_cbuf_index &&
                   desc.secondary_cbuf_offset == existing.secondary_cbuf_offset &&
                   desc.secondary_shift_left == existing.secondary_shift_left &&
                   desc.count == existing.count && desc.size_shift == existing.size_shift &&
                   desc.has_secondary == existing.has_secondary;
        });
    }

    u32 Add(const ImageBufferDescriptor& desc) {
        const u32 index{Add(image_buffer_descriptors, desc, [&desc](const auto& existing) {
            return desc.format == existing.format && desc.cbuf_index == existing.cbuf_index &&
                   desc.cbuf_offset == existing.cbuf_offset && desc.count == existing.count &&
                   desc.size_shift == existing.size_shift;
        })};
        image_buffer_descriptors[index].is_written |= desc.is_written;
        image_buffer_descriptors[index].is_read |= desc.is_read;
        image_buffer_descriptors[index].is_integer |= desc.is_integer;
        return index;
    }

    u32 Add(const TextureDescriptor& desc) {
        const u32 index{Add(texture_descriptors, desc, [&desc](const auto& existing) {
            return desc.type == existing.type && desc.is_depth == existing.is_depth &&
                   desc.has_secondary == existing.has_secondary &&
                   desc.cbuf_index == existing.cbuf_index &&
                   desc.cbuf_offset == existing.cbuf_offset &&
                   desc.shift_left == existing.shift_left &&
                   desc.secondary_cbuf_index == existing.secondary_cbuf_index &&
                   desc.secondary_cbuf_offset == existing.secondary_cbuf_offset &&
                   desc.secondary_shift_left == existing.secondary_shift_left &&
                   desc.index_program == existing.index_program &&
                   // One table, one array: a variant's cap applies to the first use (see
                   // HostTranslateInfo::table_buckets); later uses take its count.
                   (desc.count == existing.count ||
                    (IsHandleTable(desc) && IsHandleTable(existing))) &&
                   desc.size_shift == existing.size_shift &&
                   desc.live_stride == existing.live_stride &&
                   desc.live_phase == existing.live_phase;
        })};
        // TODO: Read this from TIC
        texture_descriptors[index].is_multisample |= desc.is_multisample;
        texture_descriptors[index].is_integer |= desc.is_integer;
        return index;
    }

    /// Handle tables so far: the slot of the next one in HostTranslateInfo::table_buckets
    u32 NumTables() const noexcept {
        return static_cast<u32>(
            std::count_if(texture_descriptors.begin(), texture_descriptors.end(), IsHandleTable));
    }

    /// 1-based index of the program (shared when equal)
    u32 AddIndexProgram(const IndexProgram& program) {
        const auto it{std::find(index_programs.begin(), index_programs.end(), program)};
        if (it != index_programs.end()) {
            return static_cast<u32>(std::distance(index_programs.begin(), it)) + 1;
        }
        index_programs.push_back(program);
        return static_cast<u32>(index_programs.size());
    }

    u32 TextureCount(u32 index) const noexcept {
        return texture_descriptors[index].count;
    }

    u32 Add(const ImageDescriptor& desc) {
        const u32 index{Add(image_descriptors, desc, [&desc](const auto& existing) {
            return desc.type == existing.type && desc.format == existing.format &&
                   desc.cbuf_index == existing.cbuf_index &&
                   desc.cbuf_offset == existing.cbuf_offset && desc.count == existing.count &&
                   desc.size_shift == existing.size_shift;
        })};
        image_descriptors[index].is_written |= desc.is_written;
        image_descriptors[index].is_read |= desc.is_read;
        image_descriptors[index].is_integer |= desc.is_integer;
        return index;
    }

private:
    template <typename Descriptors, typename Descriptor, typename Func>
    static u32 Add(Descriptors& descriptors, const Descriptor& desc, Func&& pred) {
        // TODO: Handle arrays
        const auto it{std::ranges::find_if(descriptors, pred)};
        if (it != descriptors.end()) {
            return static_cast<u32>(std::distance(descriptors.begin(), it));
        }
        descriptors.push_back(desc);
        return static_cast<u32>(descriptors.size()) - 1;
    }

    TextureBufferDescriptors& texture_buffer_descriptors;
    ImageBufferDescriptors& image_buffer_descriptors;
    TextureDescriptors& texture_descriptors;
    ImageDescriptors& image_descriptors;
    IndexPrograms& index_programs;
};

void PatchImageSampleImplicitLod(IR::Block& block, IR::Inst& inst) {
    IR::IREmitter ir{block, IR::Block::InstructionList::s_iterator_to(inst)};
    const auto info{inst.Flags<IR::TextureInstInfo>()};
    const IR::Value coord(inst.Arg(1));
    const IR::Value handle(ir.Imm32(0));
    const IR::U32 lod{ir.Imm32(0)};
    const IR::U1 skip_mips{ir.Imm1(true)};
    const IR::Value texture_size = ir.ImageQueryDimension(handle, lod, skip_mips, info);
    inst.SetArg(
        1, ir.CompositeConstruct(
               ir.FPMul(IR::F32(ir.CompositeExtract(coord, 0)),
                        ir.FPRecip(ir.ConvertUToF(32, 32, ir.CompositeExtract(texture_size, 0)))),
               ir.FPMul(IR::F32(ir.CompositeExtract(coord, 1)),
                        ir.FPRecip(ir.ConvertUToF(32, 32, ir.CompositeExtract(texture_size, 1))))));
}

bool IsPixelFormatSNorm(TexturePixelFormat pixel_format) {
    switch (pixel_format) {
    case TexturePixelFormat::A8B8G8R8_SNORM:
    case TexturePixelFormat::R8G8_SNORM:
    case TexturePixelFormat::R8_SNORM:
    case TexturePixelFormat::R16G16B16A16_SNORM:
    case TexturePixelFormat::R16G16_SNORM:
    case TexturePixelFormat::R16_SNORM:
        return true;
    default:
        return false;
    }
}

void PatchTexelFetch(IR::Block& block, IR::Inst& inst, TexturePixelFormat pixel_format) {
    const auto it{IR::Block::InstructionList::s_iterator_to(inst)};
    IR::IREmitter ir{block, IR::Block::InstructionList::s_iterator_to(inst)};
    auto get_max_value = [pixel_format]() -> float {
        switch (pixel_format) {
        case TexturePixelFormat::A8B8G8R8_SNORM:
        case TexturePixelFormat::R8G8_SNORM:
        case TexturePixelFormat::R8_SNORM:
            return 1.f / (std::numeric_limits<char>::max)();
        case TexturePixelFormat::R16G16B16A16_SNORM:
        case TexturePixelFormat::R16G16_SNORM:
        case TexturePixelFormat::R16_SNORM:
            return 1.f / (std::numeric_limits<short>::max)();
        default:
            throw InvalidArgument("Invalid texture pixel format");
        }
    };

    const IR::Value new_inst{&*block.PrependNewInst(it, inst)};
    const IR::F32 x(ir.CompositeExtract(new_inst, 0));
    const IR::F32 y(ir.CompositeExtract(new_inst, 1));
    const IR::F32 z(ir.CompositeExtract(new_inst, 2));
    const IR::F32 w(ir.CompositeExtract(new_inst, 3));
    const IR::F16F32F64 max_value(ir.Imm32(get_max_value()));
    const IR::Value converted =
        ir.CompositeConstruct(ir.FPMul(ir.ConvertSToF(32, 32, ir.BitCast<IR::U32>(x)), max_value),
                              ir.FPMul(ir.ConvertSToF(32, 32, ir.BitCast<IR::U32>(y)), max_value),
                              ir.FPMul(ir.ConvertSToF(32, 32, ir.BitCast<IR::U32>(z)), max_value),
                              ir.FPMul(ir.ConvertSToF(32, 32, ir.BitCast<IR::U32>(w)), max_value));
    inst.ReplaceUsesWith(converted);
}
} // Anonymous namespace

void TexturePass(Environment& env, IR::Program& program, const HostTranslateInfo& host_info) {
    // reset per-pass caches
    env.cbuf_word_cache.clear();
    env.handle_cache.clear();
    env.track_cache.clear();
    TextureInstVector to_replace;
    for (IR::Block* const block : program.post_order_blocks) {
        for (IR::Inst& inst : block->Instructions()) {
            if (!IsTextureInstruction(inst)) {
                continue;
            }
            to_replace.push_back(MakeInst(env, block, inst, host_info));
        }
    }
    // Sort instructions to visit textures by constant buffer index, then by offset
    std::ranges::sort(to_replace, [](const auto& a, const auto& b) {
        if (a.cbuf.index != b.cbuf.index) return a.cbuf.index < b.cbuf.index;
        return a.cbuf.offset < b.cbuf.offset;
    });
    Descriptors descriptors{
        program.info.texture_buffer_descriptors,
        program.info.image_buffer_descriptors,
        program.info.texture_descriptors,
        program.info.image_descriptors,
        program.info.index_programs,
    };
    const u32 sampled_dynamic_cap = DynamicSampledTextureCap(program.info, host_info, DynamicSampledTextureArrayCount(to_replace));
    bool has_last_is_integer{false};
    u32 last_cbuf_index{};
    u32 last_cbuf_offset{};
    bool last_is_integer{false};
    const auto is_texture_pixel_format_integer{[&](const ConstBufferAddr& cbuf_addr) {
        if (has_last_is_integer && last_cbuf_index == cbuf_addr.index &&
            last_cbuf_offset == cbuf_addr.offset) {
            return last_is_integer;
        }
        last_is_integer = IsTexturePixelFormatIntegerCached(env, cbuf_addr);
        last_cbuf_index = cbuf_addr.index;
        last_cbuf_offset = cbuf_addr.offset;
        has_last_is_integer = true;
        return last_is_integer;
    }};
    for (TextureInst& texture_inst : to_replace) {
        // TODO: Handle arrays
        IR::Inst* const inst{texture_inst.inst};
        inst->ReplaceOpcode(IndexedInstruction(*inst));

        const auto& cbuf{texture_inst.cbuf};
        auto flags{inst->Flags<IR::TextureInstInfo>()};
        bool is_multisample{false};
        switch (inst->GetOpcode()) {
        case IR::Opcode::ImageQueryDimensions:
            flags.type.Assign(ReadTextureTypeCached(env, cbuf));
            inst->SetFlags(flags);
            break;
        case IR::Opcode::ImageSampleImplicitLod:
            if (flags.type != TextureType::Color2D) {
                break;
            }
            if (ReadTextureTypeCached(env, cbuf) == TextureType::Color2DRect) {
                PatchImageSampleImplicitLod(*texture_inst.block, *texture_inst.inst);
            }
            break;
        case IR::Opcode::ImageFetch:
            if (flags.type == TextureType::Color2D || flags.type == TextureType::Color2DRect ||
                flags.type == TextureType::ColorArray2D) {
                is_multisample = !inst->Arg(4).IsEmpty();
            } else {
                inst->SetArg(4, IR::U32{});
            }
            if (flags.type != TextureType::Color1D) {
                break;
            }
            if (ReadTextureTypeCached(env, cbuf) == TextureType::Buffer) {
                // Replace with the bound texture type only when it's a texture buffer
                // If the instruction is 1D and the bound type is 2D, don't change the code and let
                // the rasterizer robustness handle it
                // This happens on Fire Emblem: Three Houses
                flags.type.Assign(TextureType::Buffer);
            }
            break;
        default:
            break;
        }
        u32 index;
        u32 size_shift = cbuf.count > 1 ? DynamicDescriptorSizeShift(cbuf.dynamic_offset) : DESCRIPTOR_SIZE_SHIFT;
        u32 count = cbuf.count;
        // A storage table's cbuf_index/cbuf_offset name the buffer's 64-bit pointer, which the
        // storage buffer descriptor already resolved; its elements are 8 bytes.
        u32 table_cbuf_index{cbuf.index};
        u32 table_cbuf_offset{cbuf.offset};
        if (cbuf.storage_table) {
            const auto& ssbo{program.info.storage_buffers_descriptors.at(cbuf.storage_index)};
            table_cbuf_index = ssbo.cbuf_index;
            table_cbuf_offset = ssbo.cbuf_offset;
            size_shift = 3;
        }
        // Hogwarts Legacy reads one material table at nine constant bases (c7[0x0 + i*8] ..
        // c7[0x48 + i*8]): nine descriptor arrays of the same table, each written in full per
        // draw. Rebase such reads onto the table's first element so they share one array; the
        // instruction adds its base to the index.
        u16 index_program{}; // a per-draw constant index, resolved by the renderer
        u32 base_elements{};
        if (count > 1 && !cbuf.storage_table) {
            const u32 element{1u << size_shift};
            const u32 phase{table_cbuf_offset % element};
            base_elements = (table_cbuf_offset - phase) / element;
            if (base_elements < count / 4) {
                table_cbuf_offset = phase;
            } else {
                base_elements = 0;
            }
        }
        switch (inst->GetOpcode()) {
        case IR::Opcode::ImageRead:
        case IR::Opcode::ImageAtomicIAdd32:
        case IR::Opcode::ImageAtomicSMin32:
        case IR::Opcode::ImageAtomicUMin32:
        case IR::Opcode::ImageAtomicSMax32:
        case IR::Opcode::ImageAtomicUMax32:
        case IR::Opcode::ImageAtomicInc32:
        case IR::Opcode::ImageAtomicDec32:
        case IR::Opcode::ImageAtomicAnd32:
        case IR::Opcode::ImageAtomicOr32:
        case IR::Opcode::ImageAtomicXor32:
        case IR::Opcode::ImageAtomicExchange32:
        case IR::Opcode::ImageWrite: {
            if (cbuf.has_secondary) {
                throw NotImplementedException("Unexpected separate sampler");
            }
            const bool is_written{inst->GetOpcode() != IR::Opcode::ImageRead};
            const bool is_read{inst->GetOpcode() != IR::Opcode::ImageWrite};
            const bool is_integer{is_texture_pixel_format_integer(cbuf)};
            if (flags.type == TextureType::Buffer) {
                index = descriptors.Add(ImageBufferDescriptor{
                    .format = flags.image_format,
                    .is_written = is_written,
                    .is_read = is_read,
                    .is_integer = is_integer,
                    .cbuf_index = cbuf.index,
                    .cbuf_offset = cbuf.offset,
                    .count = count,
                    .size_shift = size_shift,
                });
            } else {
                index = descriptors.Add(ImageDescriptor{
                    .type = flags.type,
                    .format = flags.image_format,
                    .is_written = is_written,
                    .is_read = is_read,
                    .is_integer = is_integer,
                    .cbuf_index = cbuf.index,
                    .cbuf_offset = cbuf.offset,
                    .count = count,
                    .size_shift = size_shift,
                });
            }
            break;
        }
        default:
            if (flags.type == TextureType::Buffer) {
                index = descriptors.Add(TextureBufferDescriptor{
                    .has_secondary = cbuf.has_secondary,
                    .cbuf_index = cbuf.index,
                    .cbuf_offset = cbuf.offset,
                    .shift_left = cbuf.shift_left,
                    .secondary_cbuf_index = cbuf.secondary_index,
                    .secondary_cbuf_offset = cbuf.secondary_offset,
                    .secondary_shift_left = cbuf.secondary_shift_left,
                    .count = count,
                    .size_shift = size_shift,
                });
            } else {
                count = std::min(count, sampled_dynamic_cap);
                const bool is_integer{is_texture_pixel_format_integer(cbuf)};
                u32 live_stride{};
                u32 live_phase{};
                if (count > 1) {
                    const OffsetLattice lattice{AnalyzeOffset(IR::Value{cbuf.dynamic_offset})};
                    const u64 element{u64{1} << size_shift};
                    if (lattice.stride > element && lattice.stride % element == 0 &&
                        lattice.stride <= 0x10000) {
                        const s64 stride{static_cast<s64>(lattice.stride)};
                        const s64 phase{((lattice.phase % stride) + stride) % stride};
                        // The element index truncates offset >> size_shift: only an aligned phase
                        // maps each reachable offset to exactly one element.
                        if (phase % static_cast<s64>(element) == 0) {
                            live_stride = static_cast<u32>(stride);
                            live_phase = static_cast<u32>(phase);
                        }
                    }
                }
                if (live_stride != 0) {
                    live_phase = (live_phase + (base_elements << size_shift)) % live_stride;
                }
                // An index the CPU can compute per draw (Hogwarts Legacy: a material slot read
                // from two constant buffers): bind that element alone, or one per Phi operand with
                // the shader's own control flow choosing among them.
                if (count > 1 && !cbuf.storage_table && program.stage != Stage::Compute) {
                    if (const auto compiled{CompileIndexProgram(IR::Value{cbuf.dynamic_offset},
                                                                size_shift, base_elements)}) {
                        const bool phi{compiled->num_candidates > 1};
                        if (!phi || BlockOf(program, cbuf.dynamic_offset.InstRecursive()) != nullptr) {
                            index_program = static_cast<u16>(descriptors.AddIndexProgram(*compiled));
                            count = compiled->num_candidates;
                            live_stride = 0;
                            live_phase = 0;
                        }
                    }
                }
                // A pipeline variant sized to the bound tables (GraphicsPipeline::TableBuckets)
                if (count > 1 && index_program == 0) {
                    const u32 slot{descriptors.NumTables()};
                    if (slot < host_info.table_buckets.size() && host_info.table_buckets[slot] != 0) {
                        count = (std::min)(count, 1u << host_info.table_buckets[slot]);
                    }
                }
                index = descriptors.Add(TextureDescriptor{
                    .type = flags.type,
                    .is_depth = flags.is_depth != 0,
                    .is_multisample = is_multisample,
                    .is_integer = is_integer,
                    .has_secondary = cbuf.has_secondary,
                    .cbuf_index = table_cbuf_index,
                    .cbuf_offset = table_cbuf_offset,
                    .shift_left = cbuf.shift_left,
                    .secondary_cbuf_index = cbuf.secondary_index,
                    .secondary_cbuf_offset = cbuf.secondary_offset,
                    .secondary_shift_left = cbuf.secondary_shift_left,
                    .count = count,
                    .size_shift = size_shift,
                    .live_stride = live_stride,
                    .live_phase = live_phase,
                    .storage_table = cbuf.storage_table,
                    .index_program = index_program,
                });
                count = descriptors.TextureCount(index);
                flags.is_integer.Assign(is_integer ? 1 : 0);
            }
            break;
        }
        flags.descriptor_index.Assign(index);
        inst->SetFlags(flags);

        if (index_program != 0 && count > 1) {
            // One descriptor per Phi operand: a Phi of immediates picks the one this path took
            IR::Inst* const phi{cbuf.dynamic_offset.InstRecursive()};
            IR::Block* const phi_block{BlockOf(program, phi)};
            const auto it{phi_block->PrependNewInst(IR::Block::InstructionList::s_iterator_to(*phi),
                                                    IR::Opcode::Phi, {},
                                                    static_cast<u32>(IR::Type::U32))};
            for (size_t j = 0; j < phi->NumArgs(); ++j) {
                it->AddPhiOperand(phi->PhiBlock(j), IR::Value{static_cast<u32>(j)});
            }
            inst->SetArg(0, IR::Value{&*it});
        } else if (index_program != 0) {
            inst->SetArg(0, IR::Value{});
        } else if (count > 1) {
            const auto insert_point{IR::Block::InstructionList::s_iterator_to(*inst)};
            IR::IREmitter ir{*texture_inst.block, insert_point};
            const IR::U32 shift{ir.Imm32(size_shift)};
            IR::U32 element_index{ir.ShiftRightLogical(cbuf.dynamic_offset, shift)};
            if (base_elements != 0) {
                element_index = ir.IAdd(element_index, ir.Imm32(base_elements));
            }
            inst->SetArg(0, ir.UMin(element_index, ir.Imm32(count - 1)));
        } else {
            inst->SetArg(0, IR::Value{});
        }

        if (!host_info.support_snorm_render_buffer && inst->GetOpcode() == IR::Opcode::ImageFetch &&
            flags.type == TextureType::Buffer) {
            const auto pixel_format = ReadTexturePixelFormatCached(env, cbuf);
            if (IsPixelFormatSNorm(pixel_format)) {
                PatchTexelFetch(*texture_inst.block, *texture_inst.inst, pixel_format);
            }
        }
    }
}

void JoinTextureInfo(Info& base, Info& source) {
    Descriptors descriptors{
        base.texture_buffer_descriptors,
        base.image_buffer_descriptors,
        base.texture_descriptors,
        base.image_descriptors,
        base.index_programs,
    };
    for (auto& desc : source.texture_buffer_descriptors) {
        descriptors.Add(desc);
    }
    for (auto& desc : source.image_buffer_descriptors) {
        descriptors.Add(desc);
    }
    for (TextureDescriptor desc : source.texture_descriptors) {
        if (desc.index_program != 0) {
            desc.index_program = static_cast<u16>(
                descriptors.AddIndexProgram(source.index_programs[desc.index_program - 1]));
        }
        descriptors.Add(desc);
    }
    for (auto& desc : source.image_descriptors) {
        descriptors.Add(desc);
    }
}

} // namespace Shader::Optimization
