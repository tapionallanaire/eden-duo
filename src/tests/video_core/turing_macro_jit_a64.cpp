// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// The ARM64 macro JIT (macro_turing_jit_a64.h) run in dynarmic's A64 emulator on x86-64 hosts,
// against the x86-64 JIT and the interpreter on random macro programs.

#include <array>
#include <cstdint>
#include <cstring>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <catch2/catch_test_macros.hpp>

#if defined(ARCHITECTURE_x86_64) && __has_include(<oaknut/oaknut.hpp>) &&                          \
    __has_include(<dynarmic/interface/A64/a64.h>)
#include <dynarmic/interface/A64/a64.h>
#include <dynarmic/interface/A64/config.h>

#include "video_core/macro_turing_jit.h"
#include "video_core/macro_turing_jit_a64.h"

namespace {
using Word = std::uint32_t;
using State = Tegra::TuringMacroJitTypes::State;
using A64Jit = Tegra::BasicTuringMacroJit<Tegra::TuringMacroProgramA64>;

// Guest memory is the host's (identity mapped). Calls to the State's emit/read hooks become an
// SVC at their address (the harness calls the hook and clobbers the caller-saved registers);
// the return address the code starts with halts.
class Emulator final : public Dynarmic::A64::UserCallbacks {
public:
    Emulator() {
        Dynarmic::A64::UserConfig config{};
        config.callbacks = this;
        config.enable_cycle_counting = false;
        config.code_cache_size = 16 * 1024 * 1024;
        jit.emplace(config);
    }

    Word Run(const std::uint32_t* code, State* state) {
        static constexpr std::uint64_t Saved = 0x5a5a000000000000ULL;
        emit_address = reinterpret_cast<std::uint64_t>(state->emit);
        read_address = reinterpret_cast<std::uint64_t>(state->read);
        jit->ClearCache();
        for (std::size_t i = 0; i < 31; ++i) {
            jit->SetRegister(i, Saved + i);
        }
        jit->SetRegister(0, reinterpret_cast<std::uint64_t>(state));
        jit->SetRegister(30, ExitAddress);
        const std::uint64_t stack_top = reinterpret_cast<std::uint64_t>(stack.data() + stack.size());
        jit->SetSP(stack_top);
        jit->SetPC(reinterpret_cast<std::uint64_t>(code));
        exception.reset();
        jit->Run();
        abi_ok = jit->GetSP() == stack_top;
        for (std::size_t i = 19; i < 30; ++i) {
            abi_ok &= jit->GetRegister(i) == Saved + i;
        }
        return static_cast<Word>(jit->GetRegister(0));
    }

    std::optional<Dynarmic::A64::Exception> exception;
    bool abi_ok{};

private:
    static constexpr std::uint64_t ExitAddress = 0x1000;
    static constexpr std::uint32_t Ret = 0xd65f03c0;
    static constexpr std::uint32_t Svc(std::uint32_t number) {
        return 0xd4000001 | (number << 5);
    }

    std::optional<std::uint32_t> MemoryReadCode(Dynarmic::A64::VAddr address) override {
        if (address == emit_address) return Svc(1);
        if (address == read_address) return Svc(2);
        if (address == ExitAddress) return Svc(3);
        if (address == emit_address + 4 || address == read_address + 4) return Ret;
        std::uint32_t word;
        std::memcpy(&word, reinterpret_cast<const void*>(address), sizeof(word));
        return word;
    }
    std::uint64_t MemoryRead(Dynarmic::A64::VAddr address, std::size_t size) override {
        std::uint64_t value{};
        std::memcpy(&value, reinterpret_cast<const void*>(address), size);
        return value;
    }
    Dynarmic::A64::Vector MemoryRead128(Dynarmic::A64::VAddr address) override {
        Dynarmic::A64::Vector value{};
        std::memcpy(value.data(), reinterpret_cast<const void*>(address), sizeof(value));
        return value;
    }
    void MemoryWrite(Dynarmic::A64::VAddr address, std::uint64_t value, std::size_t size) override {
        std::memcpy(reinterpret_cast<void*>(address), &value, size);
    }
    void MemoryWrite128(Dynarmic::A64::VAddr address, Dynarmic::A64::Vector value) override {
        std::memcpy(reinterpret_cast<void*>(address), value.data(), sizeof(value));
    }
    void CallSVC(std::uint32_t number) override {
        auto* const state = reinterpret_cast<State*>(jit->GetRegister(0));
        const Word x = static_cast<Word>(jit->GetRegister(1));
        const Word y = static_cast<Word>(jit->GetRegister(2));
        if (number == 3) {
            jit->HaltExecution();
            return;
        }
        const std::uint64_t result = number == 1 ? std::uint64_t{state->emit(state, x, y)}
                                                 : std::uint64_t{state->read(state, x)};
        // A call may change every caller-saved register.
        for (std::size_t i = 1; i < 18; ++i) {
            jit->SetRegister(i, 0xdead0000ULL + i);
        }
        jit->SetRegister(0, result | 0xbad0000000000000ULL);
    }
    void ExceptionRaised(Dynarmic::A64::VAddr, Dynarmic::A64::Exception kind) override {
        exception = kind;
        jit->HaltExecution();
    }
    void AddTicks(std::uint64_t) override {}
    std::uint64_t GetTicksRemaining() override {
        return 1ULL << 40;
    }
    std::uint64_t GetCNTPCT() override {
        return 0;
    }

    std::optional<Dynarmic::A64::Jit> jit;
    std::vector<std::uint64_t> stack = std::vector<std::uint64_t>(8192);
    std::uint64_t emit_address{}, read_address{};
};

Emulator* emulator;

Word Emulate(const std::uint32_t* code, State* state) {
    return emulator->Run(code, state);
}

struct Outcome {
    bool success{};
    std::string error;
    std::size_t consumed{};
    std::vector<std::pair<Word, Word>> emits;
    std::vector<Word> fifo;
    std::array<Word, 0xc00> dram{};

    bool operator==(const Outcome&) const = default;
};

std::string Describe(const Outcome& outcome) {
    std::string text = (outcome.success ? std::string{"ok"} : "error '" + outcome.error + "'") +
                       " consumed " + std::to_string(outcome.consumed) + " emits";
    for (const auto& [method, value] : outcome.emits) {
        text += " " + std::to_string(method) + "=" + std::to_string(value);
    }
    return text;
}

class Fuzzer {
public:
    explicit Fuzzer(std::uint32_t seed) : random{seed} {}

    /// Starts with Init instructions writing every register (the interpreter stops at reads of
    /// never-written ones).
    static constexpr Word Init = 12;

    std::vector<Word> Program(Word count) {
        std::vector<Word> code;
        for (Word pc = 0; pc < Init; ++pc) {
            std::array<Word, 3> bits{};
            const auto field = [&](Word offset, Word size, Word value) {
                for (Word bit = 0; bit < size; ++bit) {
                    bits[(offset + bit) / 32] |= ((value >> bit) & 1) << ((offset + bit) % 32);
                }
            };
            for (Word lane = 0; lane < 2; ++lane) {
                field(10 + lane * 36, 5, 0);                 // ADD
                field(15 + lane * 36, 5, pc * 2 + lane);     // dst
                field(20 + lane * 36, 5, 25);                // the lane's immediate
                field(25 + lane * 36, 5, 24);                // zero
                field(30 + lane * 36, 16, Word(random()) & 0xffff);
            }
            code.insert(code.end(), {bits[2], bits[1], bits[0]});
        }
        for (Word pc = 0; pc < count; ++pc) {
            std::array<Word, 3> bits{};
            const auto field = [&](Word offset, Word size, Word value) {
                for (Word bit = 0; bit < size; ++bit) {
                    bits[(offset + bit) / 32] |= ((value >> bit) & 1) << ((offset + bit) % 32);
                }
            };
            field(0, 1, Chance(10));
            if (Chance(40)) {
                field(1, 4, Below(16));
                field(5, 5, Source());
            }
            for (Word lane = 0; lane < 2; ++lane) {
                const Word op = Op();
                field(10 + lane * 36, 5, op);
                field(15 + lane * 36, 5, Chance(90) ? Below(24) : 24 + Below(Chance(80) ? 1 : 8));
                field(20 + lane * 36, 5, op == 23 && Chance(70) ? 28 + Below(2) : Source());
                field(25 + lane * 36, 5, Source());
                field(30 + lane * 36, 16, Immediate(op));
                field(82 + lane * 7, 3, Chance(50) ? 0 : Selector() & 7);
                field(85 + lane * 7, 4, Chance(40) ? 0 : Selector());
            }
            code.insert(code.end(), {bits[2], bits[1], bits[0]});
        }
        return code;
    }

    std::vector<Word> Parameters() {
        std::vector<Word> words(Below(12));
        for (Word& word : words) {
            word = Chance(70) ? Below(6) : Word(random());
        }
        return words;
    }

    void Fill(std::array<Word, 0xc00>& dram) {
        for (Word& word : dram) {
            word = Word(random());
        }
    }

    Word Below(Word bound) {
        return Word(random() % bound);
    }

private:
    bool Chance(Word percent) {
        return Below(100) < percent;
    }

    Word Op() {
        const Word kind = Below(100);
        if (kind < 65) {
            Word op;
            do op = Below(23); while (op == 7 && Below(4) != 0);
            return op;
        }
        if (kind < 85) return 23 + Below(7);
        return 30 + Below(2);
    }

    Word Source() {
        if (Chance(2)) return 30 + Below(2);
        return Chance(50) ? Below(24) : 24 + Below(6);
    }

    Word Selector() {
        Word selector;
        do selector = 1 + Below(10); while (selector == 7 && Below(4) != 0);
        return selector;
    }

    Word Immediate(Word op) {
        switch (op) {
        case 23: return Below(6);
        case 24: return 1 + Below(4);
        case 25: case 26: case 27: case 28: case 29: {
            const Word offset = Chance(3) ? 0x1000 : Word(std::int32_t(Below(13)) - 6) & 0x1fff;
            return offset | (Below(4) << 14);
        }
        default:
            if (Chance(5)) return 0x157 + Below(2);
            if (Chance(5)) return 0x1000;
            return Chance(50) ? Below(0x40) : Word(random()) & 0xffff;
        }
    }

    std::mt19937 random;
};

// `borrowed`: parameters the FIFO gets only when a DMA read appends to it.
template <typename Run>
Outcome Capture(const std::vector<Word>& parameters, std::span<const Word> borrowed,
                const std::array<Word, 0xc00>& dram, Run&& run) {
    Outcome outcome;
    outcome.dram = dram;
    outcome.fifo = parameters;
    const auto read = [](Word address) { return address * 2654435761U ^ 0x1234; };
    const auto emit = [&](Word method, Word value) {
        outcome.emits.emplace_back(method, value);
        if (method == 0x158) {
            if (outcome.fifo.empty()) {
                outcome.fifo.assign(borrowed.begin(), borrowed.end());
            }
            for (Word i = 0; i < (value & 3); ++i) {
                outcome.fifo.push_back(value * 7 + i);
            }
        }
        return method < 0xe00;
    };
    const auto result = run(outcome, read, emit);
    outcome.success = result.success;
    outcome.error = std::string{result.error};
    outcome.consumed = result.consumed;
    return outcome;
}
} // namespace

TEST_CASE("The ARM64 macro JIT matches the x86-64 JIT and the interpreter",
          "[video_core][native_macro]") {
    Emulator emulation;
    emulator = &emulation;
    Tegra::TuringMacroProgramA64::emulate = &Emulate;
    Fuzzer fuzzer{20261114};
    std::size_t compared{}, against_interpreter{}, failures{};
    for (Word program = 0; program < 1500; ++program) {
        const std::vector<Word> code = fuzzer.Program(4 + fuzzer.Below(40));
        Tegra::TuringMacroJit x64;
        A64Jit a64;
        A64Jit a64_batched;
        for (Word call = 0; call < 4; ++call) {
            const Word start = fuzzer.Below(4) == 0 ? Fuzzer::Init + fuzzer.Below(4) : 0;
            const std::vector<Word> parameters = fuzzer.Parameters();
            std::array<Word, 0xc00> dram;
            fuzzer.Fill(dram);
            const Outcome interpreted = Capture(parameters, {}, dram, [&](Outcome& o, auto read, auto emit) {
                Tegra::TuringMacroInterpreter interpreter;
                interpreter.active_dram = &o.dram;
                return interpreter.Execute(code, start, o.fifo, read, emit);
            });
            const Outcome x64_outcome = Capture(parameters, {}, dram, [&](Outcome& o, auto read, auto emit) {
                x64.Prepare(code, 1);
                return x64.Execute(start, o.fifo, o.dram, read, emit);
            });
            const Outcome a64_outcome = Capture(parameters, {}, dram, [&](Outcome& o, auto read, auto emit) {
                a64.Prepare(code, 1);
                return a64.Execute(start, o.fifo, o.dram, read, emit);
            });
            REQUIRE(!emulation.exception);
            REQUIRE(emulation.abi_ok);
            // Batched emits, a kept State and borrowed parameters.
            const Outcome batched = Capture({}, parameters, dram, [&](Outcome& o, auto read, auto emit) {
                a64_batched.Prepare(code, 1, true);
                return a64_batched.Execute(start, o.fifo, parameters, o.dram, read, emit,
                                           {.batch_emits = true, .keep_state = true});
            });
            REQUIRE(!emulation.exception);
            REQUIRE(emulation.abi_ok);
            ++compared;
            const bool same = a64_outcome == x64_outcome;
            // A borrowed FIFO is only filled when a DMA read appends to it.
            Outcome batched_view = batched;
            if (batched_view.fifo.empty()) batched_view.fifo = parameters;
            const bool same_batched = batched_view == a64_outcome;
            // The JITs read never-written registers as 0 where the interpreter stops, and count
            // the instruction limit in dispatches.
            const bool comparable = interpreted.error != "uninitialized register read" &&
                                    interpreted.error != "instruction limit exceeded" &&
                                    x64_outcome.error != "instruction limit exceeded";
            // On errors the JITs may stop at a different one (they run lane 0 before reading
            // lane 1's operands) after different side effects.
            const bool same_interpreted =
                !comparable || (interpreted.success ? a64_outcome == interpreted : !a64_outcome.success);
            against_interpreter += comparable;
            if ((!same || !same_batched || !same_interpreted) && ++failures <= 8) {
                UNSCOPED_INFO("program " << program << " call " << call << " start " << start);
                UNSCOPED_INFO("interpreter: " << Describe(interpreted));
                UNSCOPED_INFO("x86-64:      " << Describe(x64_outcome));
                UNSCOPED_INFO("arm64:       " << Describe(a64_outcome));
                UNSCOPED_INFO("batched:     " << Describe(batched));
                CHECK(false);
            }
        }
    }
    Tegra::TuringMacroProgramA64::emulate = nullptr;
    UNSCOPED_INFO(compared << " calls compared, " << against_interpreter << " with the interpreter");
    CHECK(failures == 0);
    CHECK(against_interpreter > compared / 4);
}
#endif
