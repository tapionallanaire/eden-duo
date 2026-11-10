// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <compare>

#include "common/common_types.h"

namespace Shader {

/// A texture handle table index the shader computes from constant buffers and immediates alone
/// (Hogwarts Legacy: a material slot number read from two constant buffers). The texture pass
/// serializes it as a postfix program; the renderer evaluates it per draw and binds that one
/// table element instead of the whole table.
enum class IndexOp : u8 {
    Imm,     ///< push imm
    CbufU32, ///< pop byte offset, push word imm (cbuf index) at that offset; 0 past the buffer
    IAdd,
    ISub,
    IMul,
    Shl,
    Shr,
    Sar,
    And,
    Or,
    Xor,
    UMin,
    UMax,
    SMin,
    SMax,
    Pack64, ///< pop hi, pop lo, push 64-bit
    Shl64,
    Shr64,
    Add64,
    Word, ///< pop 64-bit, push its low (imm 0) or high (imm 1) word
    FTrunc,
    FFloor,
    FCeil,
    FAbs,
    FNeg,
    FAdd,
    FMul,
    FMin,
    FMax,
    FClamp, ///< pop hi, lo, x
    FSat,
    U32F32,
    S32F32,
    F32U32,
    F32S32,
    BfeU, ///< pop count, offset, base
    BfeS,
};

struct IndexInsn {
    IndexOp op;
    u32 imm;

    auto operator<=>(const IndexInsn&) const = default;
};

struct IndexProgram {
    static constexpr size_t MAX_INSNS = 64;
    static constexpr size_t MAX_CANDIDATES = 4;

    std::array<IndexInsn, MAX_INSNS> insns{};
    /// Candidate j (one per Phi operand, or just one) is insns[starts[j], starts[j + 1]).
    std::array<u8, MAX_CANDIDATES + 1> starts{};
    u8 num_candidates{};

    auto operator<=>(const IndexProgram&) const = default;
};

/// Runs one candidate; the result is the table element index. read_cbuf(cbuf index, byte offset)
/// returns the word there, 0 past the buffer. A malformed program yields ~0u (out of range).
template <typename ReadCbuf>
u32 EvaluateIndexProgram(const IndexProgram& program, u32 candidate, ReadCbuf&& read_cbuf) {
    std::array<u64, 16> stack{};
    size_t sp{};
    bool bad{};
    const auto pop{[&]() -> u64 {
        if (sp == 0) {
            bad = true;
            return 0;
        }
        return stack[--sp];
    }};
    const auto push{[&](u64 value) {
        if (sp >= stack.size()) {
            bad = true;
            return;
        }
        stack[sp++] = value;
    }};
    const auto f{[](u64 bits) { return std::bit_cast<f32>(static_cast<u32>(bits)); }};
    const auto fb{[](f32 value) -> u64 { return std::bit_cast<u32>(value); }};
    const auto s{[](u64 bits) { return static_cast<s32>(static_cast<u32>(bits)); }};
    const auto ub{[](u64 value) -> u64 { return static_cast<u32>(value); }};
    const auto to_u32{[](f32 value) -> u64 {
        if (!(value > 0.0f)) {
            return 0;
        }
        return value >= 4294967296.0f ? 0xffffffffu : static_cast<u32>(value);
    }};
    const auto to_s32{[](f32 value) -> u64 {
        if (std::isnan(value)) {
            return 0;
        }
        const f32 clamped{std::clamp(value, -2147483648.0f, 2147483520.0f)};
        return static_cast<u32>(static_cast<s32>(clamped));
    }};
    for (size_t i = program.starts[candidate]; i < program.starts[candidate + 1] && !bad; ++i) {
        const auto [op, imm]{program.insns[i]};
        switch (op) {
        case IndexOp::Imm:
            push(imm);
            break;
        case IndexOp::CbufU32: {
            const u64 offset{pop()};
            push(read_cbuf(imm, static_cast<u32>(offset)));
            break;
        }
        case IndexOp::Word: {
            const u64 value{pop()};
            push(ub(imm != 0 ? value >> 32 : value));
            break;
        }
        case IndexOp::FTrunc:
            push(fb(std::trunc(f(pop()))));
            break;
        case IndexOp::FFloor:
            push(fb(std::floor(f(pop()))));
            break;
        case IndexOp::FCeil:
            push(fb(std::ceil(f(pop()))));
            break;
        case IndexOp::FAbs:
            push(fb(std::fabs(f(pop()))));
            break;
        case IndexOp::FNeg:
            push(fb(-f(pop())));
            break;
        case IndexOp::FSat:
            push(fb(std::clamp(f(pop()), 0.0f, 1.0f)));
            break;
        case IndexOp::U32F32:
            push(to_u32(f(pop())));
            break;
        case IndexOp::S32F32:
            push(to_s32(f(pop())));
            break;
        case IndexOp::F32U32:
            push(fb(static_cast<f32>(static_cast<u32>(pop()))));
            break;
        case IndexOp::F32S32:
            push(fb(static_cast<f32>(s(pop()))));
            break;
        case IndexOp::FClamp: {
            const f32 hi{f(pop())};
            const f32 lo{f(pop())};
            const f32 x{f(pop())};
            push(fb(std::min(std::max(x, lo), hi)));
            break;
        }
        case IndexOp::BfeU:
        case IndexOp::BfeS: {
            const u32 count{static_cast<u32>(pop()) & 31u};
            const u32 offset{static_cast<u32>(pop()) & 31u};
            const u32 base{static_cast<u32>(pop())};
            if (count == 0) {
                push(0);
                break;
            }
            const u32 mask{count >= 32 ? 0xffffffffu : (1u << count) - 1u};
            const u32 field{(base >> offset) & mask};
            if (op == IndexOp::BfeS && (field & (1u << (count - 1))) != 0) {
                push(ub(field | ~mask));
            } else {
                push(field);
            }
            break;
        }
        default: {
            const u64 b{pop()};
            const u64 a{pop()};
            switch (op) {
            case IndexOp::IAdd:
                push(ub(a + b));
                break;
            case IndexOp::ISub:
                push(ub(a - b));
                break;
            case IndexOp::IMul:
                push(ub(a * b));
                break;
            case IndexOp::Shl:
                push(ub(static_cast<u32>(a) << (b & 31u)));
                break;
            case IndexOp::Shr:
                push(static_cast<u32>(a) >> (b & 31u));
                break;
            case IndexOp::Sar:
                push(ub(static_cast<u32>(s(a) >> (b & 31u))));
                break;
            case IndexOp::And:
                push(ub(a & b));
                break;
            case IndexOp::Or:
                push(ub(a | b));
                break;
            case IndexOp::Xor:
                push(ub(a ^ b));
                break;
            case IndexOp::UMin:
                push(std::min(ub(a), ub(b)));
                break;
            case IndexOp::UMax:
                push(std::max(ub(a), ub(b)));
                break;
            case IndexOp::SMin:
                push(ub(static_cast<u32>(std::min(s(a), s(b)))));
                break;
            case IndexOp::SMax:
                push(ub(static_cast<u32>(std::max(s(a), s(b)))));
                break;
            case IndexOp::Pack64:
                push((b << 32) | ub(a));
                break;
            case IndexOp::Shl64:
                push(a << (b & 63u));
                break;
            case IndexOp::Shr64:
                push(a >> (b & 63u));
                break;
            case IndexOp::Add64:
                push(a + b);
                break;
            case IndexOp::FAdd:
                push(fb(f(a) + f(b)));
                break;
            case IndexOp::FMul:
                push(fb(f(a) * f(b)));
                break;
            case IndexOp::FMin:
                push(fb(std::min(f(a), f(b))));
                break;
            case IndexOp::FMax:
                push(fb(std::max(f(a), f(b))));
                break;
            default:
                bad = true;
                break;
            }
            break;
        }
        }
    }
    if (bad || sp != 1) {
        return ~0u;
    }
    return static_cast<u32>(stack[0]);
}

} // namespace Shader
