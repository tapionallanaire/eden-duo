// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <vector>

#include <oaknut/oaknut.hpp>
#ifdef ARCHITECTURE_arm64
#include <oaknut/code_block.hpp>
#endif

#include "video_core/macro_turing_jit.h"

namespace Tegra {

namespace TuringMacroA64 {
using namespace oaknut::util;
using Word = TuringMacroJitTypes::Word;
using State = TuringMacroJitTypes::State;
using Decoded = TuringMacroInterpreter::Decoded;

// TuringMacroProgramX64 (macro_turing_jit.h) block for block: x19 holds the State (rbx), w20/w21
// the instruction's FIFO loads (r12d/r13d), w22/w23 its ALU results (r14d/r15d); w0/w1/w2 are
// eax/ecx/edx (value, x, y), w3 and x16/x17 scratch. Conditional branches reach +-1 MiB and the
// whole macro RAM compiles to more than that, so conditional jumps out of an instruction (to
// the failure exits, the end or the dispatcher) go to a `B` placed after the instruction's block.
class Generator : public oaknut::VectorCodeGenerator, TuringMacroJitTypes {
public:
    Generator(std::vector<std::uint32_t>& words_, bool batch_emits_)
        : oaknut::VectorCodeGenerator(words_), words{words_}, batch_emits{batch_emits_} {}

    void Generate(std::span<const Word> code) {
        const Word count = static_cast<Word>(code.size() / 3);
        labels.resize(count);
        const std::vector<Decoded> decoded = DecodeAll(code);
        Analyze(decoded, loop_end, after_end);
        STP(X29, X30, SP, PRE_INDEXED, -64);
        MOV(X29, SP);
        STP(X19, X20, SP, 16);
        STP(X21, X22, SP, 32);
        STP(X23, X24, SP, 48);
        MOV(X19, X0);
        LDR(W0, X19, offsetof(State, start));
        B(dispatch_entry);

        for (Word pc = 0; pc < count; ++pc) {
            l(labels[pc]);
            Instruction(pc, count, decoded[pc]);
        }

        // Dispatch on w0 (the next pc). Every backward edge comes through here, so the
        // instruction limit is counted here.
        l(dispatch);
        LDR(W16, X19, offsetof(State, steps));
        SUBS(W16, W16, 1);
        STR(W16, X19, offsetof(State, steps));
        B(EQ, fail_steps);
        l(dispatch_entry);
        CmpImm(W0, count);
        B(HS, fail_outside);
        ADR(X16, table);
        LDRSW(X17, X16, X0, oaknut::util::LSL, 2);
        ADD(X16, X16, X17);
        BR(X16);

        l(success);
        MovImm(W0, 0);
        l(leave);
        LDP(X23, X24, SP, 48);
        LDP(X21, X22, SP, 32);
        LDP(X19, X20, SP, 16);
        LDP(X29, X30, SP, POST_INDEXED, 64);
        RET();
        const auto fail_with = [&](oaknut::Label& label, Word error) {
            l(label);
            MovImm(W0, error);
            B(leave);
        };
        fail_with(fail_underflow, FifoUnderflow);
        fail_with(fail_source, InvalidSource);
        fail_with(fail_destination, InvalidDestination);
        fail_with(fail_extended, UnsupportedExtended);
        fail_with(fail_merge, InvalidMerge);
        fail_with(fail_loop, InvalidLoop);
        fail_with(fail_dread, DataRamReadOutOfBounds);
        fail_with(fail_dwrite, DataRamWriteOutOfBounds);
        fail_with(fail_output, InvalidOutput);
        fail_with(fail_emit, UnsupportedEmit);
        fail_with(fail_outside, OutsideCode);
        fail_with(fail_steps, StepLimit);

        // Each pc's block as a byte offset from the table.
        l(table);
        const std::size_t table_index = words.size();
        for (Word pc = 0; pc < count; ++pc) {
            dw(0);
        }
        const std::ptrdiff_t table_offset = static_cast<std::ptrdiff_t>(table_index * 4);
        for (Word pc = 0; pc < count; ++pc) {
            words[table_index + pc] =
                static_cast<std::uint32_t>(static_cast<std::int32_t>(labels[pc].offset() - table_offset));
        }
    }

private:
    static constexpr std::size_t RegOffset(Word reg) {
        return offsetof(State, regs) + reg * sizeof(Word);
    }

    void MovImm(oaknut::WReg dst, Word value) {
        MOV(dst, value);
    }

    void CmpImm(oaknut::WReg reg, Word value) {
        if (value < 4096) {
            CMP(reg, value);
        } else {
            MOV(W17, value);
            CMP(reg, W17);
        }
    }

    // A label in reach of a conditional branch that continues at `target`.
    oaknut::Label& Far(oaknut::Label& target) {
        for (auto& stub : stubs) {
            if (stub.target == &target) return *stub.label;
        }
        stubs.push_back({std::make_unique<oaknut::Label>(), &target});
        return *stubs.back().label;
    }

    // Loads MME register `reg` (as an operand of `lane`) into `dst`.
    void Get(oaknut::WReg dst, const Decoded& d, Word reg, Word lane) {
        if (reg < 24) {
            LDR(dst, X19, RegOffset(reg));
            return;
        }
        switch (reg) {
        case 24: MovImm(dst, 0); return;
        case 25: MovImm(dst, Word(std::int32_t(std::int16_t(d.imm[lane])))); return;
        case 26: MovImm(dst, Word(std::int32_t(std::int16_t(d.imm[1 - lane])))); return;
        case 27: MovImm(dst, (d.imm[0] << 16) | d.imm[1]); return;
        case 28: MOV(dst, W20); return;
        case 29: MOV(dst, W21); return;
        default: B(fail_source); return;
        }
    }

    void Instruction(Word pc, Word count, const Decoded& d) {
        stubs.clear();
        // FIFO loads, in lane order.
        for (Word i = 0; i < 2; ++i) {
            const oaknut::WReg load = i == 0 ? W20 : W21;
            if (!d.load[i]) {
                MovImm(load, 0);
                continue;
            }
            LDR(W0, X19, offsetof(State, consumed));
            LDR(W16, X19, offsetof(State, params_size));
            CMP(W0, W16);
            B(HS, Far(fail_underflow));
            LDR(X17, X19, offsetof(State, params));
            LDR(load, X17, X0, oaknut::util::LSL, 2);
            ADD(W0, W0, 1);
            STR(W0, X19, offsetof(State, consumed));
        }
        if (!d.pred_mode) {
            Body(pc, count, d, 15, true);
        } else {
            oaknut::Label when_false;
            Get(W0, d, d.pred_reg, 0);
            CBZ(W0, when_false);
            Body(pc, count, d, d.mask_true, false);
            l(when_false);
            Body(pc, count, d, d.mask_false, true);
        }
        for (auto& stub : stubs) {
            l(*stub.label);
            B(*stub.target);
        }
    }

    void Body(Word pc, Word count, const Decoded& d, Word mask, bool last) {
        // An inactive lane's result reads as 0 when an output selects it.
        for (Word lane = 0; lane < 2; ++lane) {
            if (mask & (1U << lane)) continue;
            bool selected = false;
            for (Word out = 0; out < 2; ++out) {
                if (!(mask & (4U << out))) continue;
                selected |= d.out_method[out] == lane + 1 || d.out_emit[out] == lane + 1;
            }
            if (selected) {
                MovImm(lane == 0 ? W22 : W23, 0);
            }
        }
        const bool flow = ((mask & 1) && IsFlow(d.op[0])) || ((mask & 2) && IsFlow(d.op[1]));
        if (flow) {
            MovImm(W16, pc + 1);
            STR(W16, X19, offsetof(State, next_pc));
            STRB(WZR, X19, offsetof(State, cancel_end));
        }
        // Lane 1's operands are read before lane 0 commits its destination (or calls out).
        const bool lane0_active = (mask & 1) != 0;
        const bool early_operands =
            (mask & 2) && lane0_active &&
            (d.op[0] == 22 || (d.dst[0] < 24 && (d.dst[0] == d.src_a[1] || d.dst[0] == d.src_b[1])));
        if (early_operands) {
            Get(W0, d, d.src_a[1], 1);
            STR(W0, X19, offsetof(State, scratch_x));
            Get(W0, d, d.src_b[1], 1);
            STR(W0, X19, offsetof(State, scratch_y));
        }
        for (Word lane = 0; lane < 2; ++lane) {
            if (!(mask & (1U << lane))) {
                continue;
            }
            if (lane == 1 && early_operands) {
                LDR(W1, X19, offsetof(State, scratch_x));
                LDR(W2, X19, offsetof(State, scratch_y));
            } else {
                Get(W1, d, d.src_a[lane], lane);
                Get(W2, d, d.src_b[lane], lane);
            }
            Lane(pc, d, lane);
            // w0 holds the lane's value.
            MOV(lane == 0 ? W22 : W23, W0);
            if (d.dst[lane] < 24) {
                STR(W0, X19, RegOffset(d.dst[lane]));
            } else if (d.dst[lane] != 24) {
                B(fail_destination);
            }
        }
        for (Word lane = 0; lane < 2; ++lane) {
            if (!(mask & (4U << lane))) {
                continue;
            }
            if (d.out_method[lane]) {
                Output(W0, d, d.out_method[lane]);
                AND(W1, W0, 0xfff);
                STR(W1, X19, offsetof(State, method));
                UBFX(W0, W0, 12, 4);
                STR(W0, X19, offsetof(State, increment));
            }
            if (d.out_emit[lane]) {
                Output(W0, d, d.out_emit[lane]);
                oaknut::Label call_emit, emitted;
                if (batch_emits) {
                    // Into the buffer unless it is full or the method is one the macro sees
                    // the effect of (DMA reads) or one `emit` may refuse.
                    LDR(W1, X19, offsetof(State, method));
                    LDR(W2, X19, offsetof(State, emit_count));
                    LDR(W3, X19, offsetof(State, emit_capacity));
                    CMP(W2, W3);
                    B(HS, call_emit);
                    CMP(W1, 0xe00);
                    B(HS, call_emit);
                    SUB(W3, W1, 0x157);
                    CMP(W3, 1);
                    B(LS, call_emit);
                    ADD(X16, X19, X2, oaknut::util::LSL, 3);
                    STR(W1, X16, offsetof(State, emit_buffer));
                    STR(W0, X16, offsetof(State, emit_buffer) + 4);
                    ADD(W2, W2, 1);
                    STR(W2, X19, offsetof(State, emit_count));
                    B(emitted);
                    l(call_emit);
                }
                MOV(W2, W0);
                LDR(W1, X19, offsetof(State, method));
                MOV(X0, X19);
                LDR(X16, X19, offsetof(State, emit));
                BLR(X16);
                TST(W0, 0xff);
                B(EQ, Far(fail_emit));
                if (batch_emits) {
                    l(emitted);
                }
                LDR(W0, X19, offsetof(State, increment));
                LDR(W1, X19, offsetof(State, method));
                ADD(W1, W1, W0);
                STR(W1, X19, offsetof(State, method));
            }
        }
        // An END in the previous instruction ends the macro after this one.
        if (after_end[pc]) {
            LDRB(W16, X19, offsetof(State, end_next));
            CBNZ(W16, Far(success));
        }
        if (flow) {
            LDRB(W16, X19, offsetof(State, stopped));
            CBNZ(W16, Far(success));
        }
        if (d.end) {
            if (flow) {
                LDRB(W16, X19, offsetof(State, cancel_end));
                EOR(W16, W16, 1);
                STRB(W16, X19, offsetof(State, end_next));
            } else {
                MovImm(W16, 1);
                STRB(W16, X19, offsetof(State, end_next));
            }
        }
        if (loop_end[pc]) {
            oaknut::Label no_loop;
            LDR(W0, X19, offsetof(State, loop_count));
            CBZ(W0, no_loop);
            LDR(W16, X19, offsetof(State, loop_end));
            CmpImm(W16, pc);
            B(NE, no_loop);
            SUB(W0, W0, 1);
            STR(W0, X19, offsetof(State, loop_count));
            LDR(W0, X19, offsetof(State, loop_start));
            ADD(W0, W0, 1);
            B(dispatch);
            l(no_loop);
        }
        if (flow) {
            LDR(W0, X19, offsetof(State, next_pc));
            if (pc + 1 < count) {
                CmpImm(W0, pc + 1);
                B(EQ, labels[pc + 1]);
            }
            B(dispatch);
        } else if (pc + 1 < count) {
            // The last body falls through to the next instruction when nothing follows it.
            if (!last || !stubs.empty()) {
                B(labels[pc + 1]);
            }
        } else {
            B(fail_outside);
        }
    }

    void Output(oaknut::WReg dst, const Decoded& d, Word selector) {
        switch (selector) {
        case 1: MOV(dst, W22); return;
        case 2: MOV(dst, W23); return;
        case 3: MOV(dst, W20); return;
        case 4: MOV(dst, W21); return;
        case 5: MovImm(dst, d.imm[0]); return;
        case 6: MovImm(dst, d.imm[1]); return;
        case 8: MovImm(dst, d.imm[0] >> 12); return;
        case 9: MovImm(dst, d.imm[1] >> 12); return;
        case 10: MovImm(dst, (d.imm[0] << 16) | d.imm[1]); return;
        default: B(fail_output); return;
        }
    }

    // The condition of comparison `op` (x < y signed/unsigned, x <= y, x == y).
    static oaknut::Cond Condition(Word op) {
        switch (op) {
        case 17: case 25: return LT;
        case 18: case 26: return LO;
        case 19: case 27: return LE;
        case 20: case 28: return LS;
        default: return EQ;
        }
    }

    static oaknut::Cond Inverse(oaknut::Cond cond) {
        return static_cast<oaknut::Cond>(static_cast<int>(cond) ^ 1);
    }

    // w1 = x, w2 = y; leaves the value in w0.
    void Lane(Word pc, const Decoded& d, Word lane) {
        const Word imm = d.imm[lane];
        const Word op = d.op[lane];
        switch (op) {
        case 0:
            ADDS(W0, W1, W2);
            CSET(W3, CS);
            STR(W3, X19, offsetof(State, carry));
            return;
        case 1:
            ADD(W0, W1, W2);
            LDR(W3, X19, offsetof(State, carry));
            ADD(W0, W0, W3);
            return;
        case 2:
            // The carry is the borrow (x < y): ARM64 sets C when there is none.
            SUBS(W0, W1, W2);
            CSET(W3, LO);
            STR(W3, X19, offsetof(State, carry));
            return;
        case 3:
            SUB(W0, W1, W2);
            LDR(W3, X19, offsetof(State, carry));
            SUB(W0, W0, W3);
            return;
        case 4:
        case 6:
            if (op == 4) {
                SMULL(X0, W1, W2);
            } else {
                UMULL(X0, W1, W2);
            }
            LSR(X3, X0, 32);
            STR(W3, X19, offsetof(State, carry));
            return;
        case 5:
            LDR(W0, X19, offsetof(State, carry));
            return;
        case 7:
            CmpImm(W1, 0x1000);
            B(NE, Far(fail_extended));
            MovImm(W0, 0);
            return;
        case 8:
            CLZ(W0, W1);
            return;
        case 9: LSLV(W0, W1, W2); return;
        case 10: LSRV(W0, W1, W2); return;
        case 11: ASRV(W0, W1, W2); return;
        case 12: AND(W0, W1, W2); return;
        case 13: AND(W0, W1, W2); MVN(W0, W0); return;
        case 14: ORR(W0, W1, W2); return;
        case 15: EOR(W0, W1, W2); return;
        case 16: {
            const Word dest = (imm >> 10) & 63, bit_count = (imm >> 5) & 31;
            const Word source = imm & 31;
            if (dest >= 32) {
                B(fail_merge);
                return;
            }
            const Word bits_mask = (1U << bit_count) - 1;
            MovImm(W3, ~(bits_mask << dest));
            AND(W0, W1, W3);
            LSR(W2, W2, source);
            MovImm(W3, bits_mask);
            AND(W2, W2, W3);
            LSL(W2, W2, dest);
            ORR(W0, W0, W2);
            return;
        }
        case 17: case 18: case 19: case 20: case 21:
            CMP(W1, W2);
            CSETM(W0, Condition(op));
            return;
        case 22:
            ADD(W1, W1, W2);
            AND(W1, W1, 0xffff);
            MOV(X0, X19);
            LDR(X16, X19, offsetof(State, read));
            BLR(X16);
            return;
        case 23: {
            if (imm < 2) {
                B(fail_loop);
                return;
            }
            oaknut::Label skip, done;
            LDR(W16, X19, offsetof(State, loop_count));
            CBNZ(W16, Far(fail_loop));
            CBZ(W1, skip);
            SUB(W1, W1, 1);
            STR(W1, X19, offsetof(State, loop_count));
            MovImm(W16, pc);
            STR(W16, X19, offsetof(State, loop_start));
            MovImm(W16, pc + imm - 1);
            STR(W16, X19, offsetof(State, loop_end));
            B(done);
            l(skip);
            MovImm(W16, pc + imm);
            STR(W16, X19, offsetof(State, next_pc));
            l(done);
            MovImm(W0, 0);
            return;
        }
        case 24:
            MovImm(W16, pc + (imm & 0x7fff));
            STR(W16, X19, offsetof(State, next_pc));
            MovImm(W0, 0);
            return;
        case 25: case 26: case 27: case 28: case 29: {
            oaknut::Label not_taken;
            const oaknut::Cond cond = Condition(op);
            CMP(W1, W2);
            // Taken when the condition equals bit 15.
            B((imm & 0x8000) ? Inverse(cond) : cond, not_taken);
            const auto offset = std::int32_t(imm << 19) >> 19;
            if (offset == -4096) {
                MovImm(W16, 1);
                STRB(W16, X19, offsetof(State, stopped));
            } else {
                MovImm(W16, Word(std::int32_t(pc) + offset));
                STR(W16, X19, offsetof(State, next_pc));
                MovImm(W16, (imm & 0x4000) ? 1 : 0);
                STRB(W16, X19, offsetof(State, cancel_end));
            }
            l(not_taken);
            MovImm(W0, 0);
            return;
        }
        case 30:
            CmpImm(W1, 0xc00);
            B(HS, Far(fail_dread));
            LDR(X16, X19, offsetof(State, dram));
            LDR(W0, X16, X1, oaknut::util::LSL, 2);
            return;
        case 31:
            CmpImm(W1, 0xc00);
            B(HS, Far(fail_dwrite));
            LDR(X16, X19, offsetof(State, dram));
            STR(W2, X16, X1, oaknut::util::LSL, 2);
            MovImm(W0, 0);
            return;
        default:
            MovImm(W0, 0);
            return;
        }
    }

    struct Stub {
        std::unique_ptr<oaknut::Label> label;
        oaknut::Label* target;
    };

    std::vector<std::uint32_t>& words;
    bool batch_emits{};
    std::vector<oaknut::Label> labels;
    std::vector<bool> loop_end, after_end;
    std::vector<Stub> stubs; ///< The current instruction's far branches
    oaknut::Label table, dispatch, dispatch_entry, success, leave;
    oaknut::Label fail_underflow, fail_source, fail_destination, fail_extended, fail_merge,
        fail_loop, fail_dread, fail_dwrite, fail_output, fail_emit, fail_outside, fail_steps;
};
} // namespace TuringMacroA64

// The ARM64 code of a macro RAM (TuringMacroA64::Generator), in an executable block.
class TuringMacroProgramA64 : public TuringMacroJitTypes {
public:
    explicit TuringMacroProgramA64(std::span<const Word> code, bool batch_emits) {
        TuringMacroA64::Generator{words, batch_emits}.Generate(code);
#ifdef ARCHITECTURE_arm64
        const std::size_t bytes = words.size() * sizeof(std::uint32_t);
        block = std::make_unique<oaknut::CodeBlock>(bytes);
        block->unprotect();
        std::memcpy(block->ptr(), words.data(), bytes);
        block->protect();
        block->invalidate(block->ptr(), bytes);
        entry = reinterpret_cast<Word (*)(State*)>(block->ptr());
        words = {};
#endif
    }

    Word Run(State* state) const {
#ifdef ARCHITECTURE_arm64
        return entry(state);
#else
        return emulate(words.data(), state);
#endif
    }

#ifndef ARCHITECTURE_arm64
    /// Runs the code on other hosts: tests set it to an A64 emulator.
    static inline Word (*emulate)(const std::uint32_t* code, State* state) = nullptr;
#endif

private:
    std::vector<std::uint32_t> words;
#ifdef ARCHITECTURE_arm64
    std::unique_ptr<oaknut::CodeBlock> block;
    Word (*entry)(State*) = nullptr;
#endif
};

#ifdef ARCHITECTURE_arm64
class TuringMacroJit final : public BasicTuringMacroJit<TuringMacroProgramA64> {};
#endif

} // namespace Tegra
