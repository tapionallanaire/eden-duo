// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>

#include "common/common_types.h"

// Pointer authentication (ARMv8.3) as the identity, for guest code that runs natively.
//
// Switch 2 programs sign and authenticate pointers. Run natively they would use the host
// process's keys, and an authentication that fails is a fault (ESR class 0x1C on a CPU with
// FEAT_FPAC); every form outside the hint space is also undefined on a CPU without ARMv8.3.
// Rewriting each instruction to what it does when nothing is ever signed gives the same
// semantics as the JIT (dynarmic's pointer_authentication.cpp): PAC* add no code, AUT*/XPAC* have
// nothing to strip, the combined branches and loads are their plain forms, and PACGA is
// (Xn ^ Xm) & 0xFFFFFFFF00000000.
//
// Encodings are the decoder patterns of src/dynarmic/src/dynarmic/frontend/A64/decoder/a64.inc:
//   HINT        11010101000000110010MMMMooo11111   (CRm:op2 select the PAC hints)
//   PACAUT_reg  110110101100000100Zooonnnnnddddd
//   XPAC_1      110110101100000101000D11111ddddd
//   PACGA       10011010110mmmmm001100nnnnnddddd
//   RETA        110101100101111100001M1111111111
//   BRA_BLRA    1101011Z00L1111100001Mnnnnnmmmmm
//   LDRA        11111000MS1iiiiiiiiiW1nnnnnttttt

namespace Core::NCE {

enum class PacForm : u8 {
    None,           ///< Not a pointer authentication instruction
    Hint,           ///< PACIASP, AUTIB1716, XPACLRI, ...: NOP
    Register,       ///< PACIA Xd, Xn ... AUTDZB Xd, XPACI/XPACD Xd: NOP
    Return,         ///< RETAA, RETAB: RET
    Branch,         ///< BRAA/BRAB/BLRAA/BLRAB and their Z forms: BR/BLR Xn
    Load,           ///< LDRAA/LDRAB that one plain load replaces
    LoadTrampoline, ///< LDRAA/LDRAB that needs two instructions
    Generic,        ///< PACGA with a result that one NOP replaces (Xd is XZR)
    GenericTrampoline, ///< PACGA
    ExceptionReturn,   ///< ERETAA/ERETAB: not for user code, left alone
};

inline constexpr size_t NumPacForms = static_cast<size_t>(PacForm::ExceptionReturn) + 1;

struct PacRewrite {
    PacForm form{PacForm::None};
    u32 replacement{}; ///< For the forms rewritten in place
};

namespace PacEncoding {
inline constexpr u32 Nop = 0xD503201F;
inline constexpr u32 Ret = 0xD65F03C0; // RET X30

constexpr u32 Br(u32 rn) {
    return 0xD61F0000 | (rn << 5);
}
constexpr u32 Blr(u32 rn) {
    return 0xD63F0000 | (rn << 5);
}
/// LDR Xt, [Xn|SP, #offset], offset a non-negative multiple of 8 up to 32760
constexpr u32 LdrUnsigned(u32 rt, u32 rn, u32 offset) {
    return 0xF9400000 | ((offset / 8) << 10) | (rn << 5) | rt;
}
/// LDUR Xt, [Xn|SP, #offset], offset in -256..255
constexpr u32 Ldur(u32 rt, u32 rn, s32 offset) {
    return 0xF8400000 | ((static_cast<u32>(offset) & 0x1FF) << 12) | (rn << 5) | rt;
}
/// LDR Xt, [Xn|SP, #offset]!, offset in -256..255
constexpr u32 LdrPreIndex(u32 rt, u32 rn, s32 offset) {
    return 0xF8400C00 | ((static_cast<u32>(offset) & 0x1FF) << 12) | (rn << 5) | rt;
}
/// ADD or SUB Xd|SP, Xn|SP, #amount for amount in 0..4096 (4096 as 1 << 12)
constexpr u32 AddSubImmediate(bool subtract, u32 rd, u32 rn, u32 amount) {
    const u32 base = subtract ? 0xD1000000 : 0x91000000;
    return amount < 4096 ? base | (amount << 10) | (rn << 5) | rd
                         : base | (1U << 22) | ((amount >> 12) << 10) | (rn << 5) | rd;
}
/// EOR Xd, Xn, Xm (register 31 is XZR)
constexpr u32 Eor(u32 rd, u32 rn, u32 rm) {
    return 0xCA000000 | (rm << 16) | (rn << 5) | rd;
}
/// AND Xd, Xn, #0xFFFFFFFF00000000
constexpr u32 AndHighHalf(u32 rd, u32 rn) {
    return 0x92607C00 | (rn << 5) | rd;
}
/// STR Xt, [SP, #-16]!
constexpr u32 PushRegister(u32 rt) {
    return 0xF81F0FE0 | rt;
}
/// LDR Xt, [SP], #16
constexpr u32 PopRegister(u32 rt) {
    return 0xF84107E0 | rt;
}
} // namespace PacEncoding

/// The signed byte offset of an LDRAA/LDRAB (S:imm9, scaled by 8).
constexpr s32 PacLoadOffset(u32 inst) {
    const u32 imm10 = (((inst >> 22) & 1) << 9) | ((inst >> 12) & 0x1FF);
    return (static_cast<s32>(imm10) - (((inst >> 22) & 1) != 0 ? 1024 : 0)) * 8;
}

/// What a guest instruction becomes when pointer authentication is the identity.
constexpr PacRewrite ClassifyPac(u32 inst) {
    using namespace PacEncoding;
    // HINT: CRm 0b0001 with op2 0, 2, 4, 6 = PACIA1716, PACIB1716, AUTIA1716, AUTIB1716;
    // CRm 0b0011 with op2 0..7 = PACIAZ, PACIASP, PACIBZ, PACIBSP, AUTIAZ, AUTIASP, AUTIBZ,
    // AUTIBSP; CRm 0b0000 with op2 7 = XPACLRI (a NOP only because nothing is ever signed).
    if ((inst & 0xFFFFF01F) == 0xD503201F) {
        const u32 crm = (inst >> 8) & 0xF;
        const u32 op2 = (inst >> 5) & 0x7;
        if ((crm == 0b0001 && (op2 & 1) == 0) || crm == 0b0011 || (crm == 0b0000 && op2 == 7)) {
            return {PacForm::Hint, Nop};
        }
        return {};
    }
    // PACIA/PACIB/PACDA/PACDB/AUTIA/AUTIB/AUTDA/AUTDB Xd, Xn|SP; with Z the modifier is zero and
    // the Rn field must be 31.
    if ((inst & 0xFFFFC000) == 0xDAC10000) {
        const bool z = ((inst >> 13) & 1) != 0;
        if (z && ((inst >> 5) & 0x1F) != 31) {
            return {};
        }
        return {PacForm::Register, Nop};
    }
    // XPACI/XPACD Xd
    if ((inst & 0xFFFFFBE0) == 0xDAC143E0) {
        return {PacForm::Register, Nop};
    }
    // PACGA Xd, Xn, Xm|SP
    if ((inst & 0xFFE0FC00) == 0x9AC03000) {
        if ((inst & 0x1F) == 31) {
            return {PacForm::Generic, Nop};
        }
        return {PacForm::GenericTrampoline, 0};
    }
    // RETAA/RETAB
    if ((inst & 0xFFFFFBFF) == 0xD65F0BFF) {
        return {PacForm::Return, Ret};
    }
    // ERETAA/ERETAB
    if ((inst & 0xFFFFFBFF) == 0xD69F0BFF) {
        return {PacForm::ExceptionReturn, 0};
    }
    // BRAA/BRAB/BLRAA/BLRAB Xn, Xm|SP (Z set); BRAAZ/BRABZ/BLRAAZ/BLRABZ Xn (Z clear, Rm 31)
    if ((inst & 0xFEDFF800) == 0xD61F0800) {
        const bool z = ((inst >> 24) & 1) != 0;
        if (!z && (inst & 0x1F) != 31) {
            return {};
        }
        const u32 rn = (inst >> 5) & 0x1F;
        return {PacForm::Branch, ((inst >> 21) & 1) != 0 ? Blr(rn) : Br(rn)};
    }
    // LDRAA/LDRAB Xt, [Xn|SP, #offset]{!}
    if ((inst & 0xFF200400) == 0xF8200400) {
        const u32 rt = inst & 0x1F;
        const u32 rn = (inst >> 5) & 0x1F;
        const bool writeback = ((inst >> 11) & 1) != 0;
        const s32 offset = PacLoadOffset(inst);
        if (writeback) {
            if (offset >= -256 && offset <= 255) {
                return {PacForm::Load, LdrPreIndex(rt, rn, offset)};
            }
            return {PacForm::LoadTrampoline, 0};
        }
        if (offset >= 0) {
            return {PacForm::Load, LdrUnsigned(rt, rn, static_cast<u32>(offset))};
        }
        if (offset >= -256) {
            return {PacForm::Load, Ldur(rt, rn, offset)};
        }
        // The trampoline computes the address in Xt; a load into XZR has no register for it.
        // Its only effect would be the memory access, which is dropped.
        return rt == 31 ? PacRewrite{PacForm::Load, Nop} : PacRewrite{PacForm::LoadTrampoline, 0};
    }
    return {};
}

/// The instructions that replace a PacForm::LoadTrampoline or PacForm::GenericTrampoline
/// instruction; `count` of `words` are used. The trampoline branches back after them.
struct PacSequence {
    std::array<u32, 5> words{};
    u32 count{};
};

constexpr PacSequence PacTrampoline(u32 inst) {
    using namespace PacEncoding;
    PacSequence out{};
    const auto emit = [&out](u32 word) { out.words[out.count++] = word; };
    const u32 rd = inst & 0x1F;
    const u32 rn = (inst >> 5) & 0x1F;
    if ((inst & 0xFFE0FC00) == 0x9AC03000) {
        // PACGA Xd, Xn, Xm|SP -> Xd = (Xn ^ Xm) & 0xFFFFFFFF00000000
        const u32 rm = (inst >> 16) & 0x1F;
        if (rm != 31) {
            emit(Eor(rd, rn, rm));
        } else if (rd != rn) {
            emit(AddSubImmediate(false, rd, 31, 0)); // MOV Xd, SP
            emit(Eor(rd, rd, rn));
        } else {
            // Xd is also Xn: the stack pointer goes through a register saved on the guest stack,
            // as the TPIDR_EL0 write handler does.
            const u32 scratch = rd == 16 ? 17 : 16;
            emit(PushRegister(scratch));
            emit(AddSubImmediate(false, scratch, 31, 16)); // the guest's SP
            emit(Eor(rd, rd, scratch));
            emit(PopRegister(scratch));
        }
        emit(AndHighHalf(rd, rd));
        return out;
    }
    // LDRAA/LDRAB
    const s32 offset = PacLoadOffset(inst);
    const u32 amount = static_cast<u32>(offset < 0 ? -offset : offset);
    if (((inst >> 11) & 1) != 0) {
        emit(AddSubImmediate(offset < 0, rn, rn, amount));
        emit(LdrUnsigned(rd, rn, 0));
    } else {
        emit(AddSubImmediate(offset < 0, rd, rn, amount));
        emit(LdrUnsigned(rd, rd, 0));
    }
    return out;
}

// Self-check: one encoding of each form (each checked with a disassembler) against the
// classification and its replacement, and plain instructions that must be left alone.
namespace PacSelfCheck {
constexpr bool Is(u32 inst, PacForm form, u32 replacement) {
    const PacRewrite rewrite = ClassifyPac(inst);
    return rewrite.form == form && rewrite.replacement == replacement;
}
constexpr bool Untouched(u32 inst) {
    return ClassifyPac(inst).form == PacForm::None;
}
using namespace PacEncoding;
static_assert(Is(0xD503233F, PacForm::Hint, Nop)); // paciasp
static_assert(Is(0xD50323BF, PacForm::Hint, Nop)); // autiasp
static_assert(Is(0xD503237F, PacForm::Hint, Nop)); // pacibsp
static_assert(Is(0xD50323FF, PacForm::Hint, Nop)); // autibsp
static_assert(Is(0xD503231F, PacForm::Hint, Nop)); // paciaz
static_assert(Is(0xD50323DF, PacForm::Hint, Nop)); // autibz
static_assert(Is(0xD503211F, PacForm::Hint, Nop)); // pacia1716
static_assert(Is(0xD503219F, PacForm::Hint, Nop)); // autia1716
static_assert(Is(0xD50321DF, PacForm::Hint, Nop)); // autib1716
static_assert(Is(0xD50320FF, PacForm::Hint, Nop)); // xpaclri
static_assert(Untouched(0xD503201F));              // nop
static_assert(Untouched(0xD503203F));              // yield
static_assert(Untouched(0xD503245F));              // bti c
static_assert(Untouched(0xD503221F));              // esb
static_assert(Untouched(0xD503213F));              // hint #9 (unallocated in the 1716 group)
static_assert(Is(0xDAC10130, PacForm::Register, Nop)); // pacia x16, x9
static_assert(Is(0xDAC117FE, PacForm::Register, Nop)); // autib x30, sp
static_assert(Is(0xDAC123E5, PacForm::Register, Nop)); // paciza x5
static_assert(Is(0xDAC143E0, PacForm::Register, Nop)); // xpaci x0
static_assert(Is(0xDAC147E0, PacForm::Register, Nop)); // xpacd x0
static_assert(Untouched(0xDAC12025));                  // Z form with Rn != 31: unallocated
static_assert(Untouched(0xDAC01000));                  // clz x0, x0
static_assert(Is(0xD65F0BFF, PacForm::Return, Ret));   // retaa
static_assert(Is(0xD65F0FFF, PacForm::Return, Ret));   // retab
static_assert(Untouched(0xD65F03C0));                  // ret
static_assert(Is(0xD69F0BFF, PacForm::ExceptionReturn, 0)); // eretaa
static_assert(Is(0xD61F0A1F, PacForm::Branch, 0xD61F0200)); // braaz x16 -> br x16
static_assert(Is(0xD73F0909, PacForm::Branch, 0xD63F0100)); // blraa x8, x9 -> blr x8
static_assert(Is(0xD63F0C1F, PacForm::Branch, 0xD63F0000)); // blrabz x0 -> blr x0
static_assert(Is(0xD71F0E30, PacForm::Branch, 0xD61F0220)); // brab x17, x16 -> br x17
static_assert(Untouched(0xD63F0100));                       // blr x8
static_assert(Untouched(0xD61F0200));                       // br x16
static_assert(Is(0xF8200420, PacForm::Load, 0xF9400020)); // ldraa x0, [x1] -> ldr x0, [x1]
static_assert(Is(0xF8FFFC62, PacForm::Load, 0xF85F8C62)); // ldrab x2, [x3, #-8]! -> ldr x2, [x3, #-8]!
static_assert(Is(0xF87FF420, PacForm::Load, 0xF85F8020)); // ldraa x0, [x1, #-8] -> ldur x0, [x1, #-8]
static_assert(Is(0xF83FF7E4, PacForm::Load, 0xF947FFE4)); // ldraa x4, [sp, #4088] -> ldr x4, [sp, #4088]
static_assert(Is(0xF8600420, PacForm::LoadTrampoline, 0)); // ldraa x0, [x1, #-4096]
static_assert(Is(0xF8220C20, PacForm::LoadTrampoline, 0)); // ldraa x0, [x1, #256]!
static_assert(Untouched(0xF9400020));                      // ldr x0, [x1]
static_assert(Untouched(0xF8626820));                      // ldr x0, [x1, x2]
static_assert(Untouched(0xF8200020));                      // ldadd x0, x0, [x1]
static_assert(Is(0x9AC23020, PacForm::GenericTrampoline, 0)); // pacga x0, x1, x2
static_assert(Is(0x9AC2303F, PacForm::Generic, Nop));         // pacga xzr, x1, x2
static_assert(Untouched(0x9AC20C20));                         // sdiv x0, x1, x2
static_assert(Untouched(0xD4000001));                         // svc #0
static_assert(PacLoadOffset(0xF8600420) == -4096);
static_assert(PacLoadOffset(0xF83FF7E4) == 4088);
// pacga x2, x8, x2 -> eor x2, x8, x2; and x2, x2, #0xffffffff00000000
static_assert(PacTrampoline(0x9AC23102).count == 2 &&
              PacTrampoline(0x9AC23102).words[0] == 0xCA020102 &&
              PacTrampoline(0x9AC23102).words[1] == 0x92607C42);
// pacga x0, x1, sp -> mov x0, sp; eor x0, x0, x1; and
static_assert(PacTrampoline(0x9ADF3020).count == 3 &&
              PacTrampoline(0x9ADF3020).words[0] == 0x910003E0 &&
              PacTrampoline(0x9ADF3020).words[1] == 0xCA010000);
// pacga x1, x1, sp -> str x16, [sp, #-16]!; add x16, sp, #16; eor x1, x1, x16; ldr x16, [sp], #16; and
static_assert(PacTrampoline(0x9ADF3021).count == 5 &&
              PacTrampoline(0x9ADF3021).words[0] == 0xF81F0FF0 &&
              PacTrampoline(0x9ADF3021).words[1] == 0x910043F0 &&
              PacTrampoline(0x9ADF3021).words[2] == 0xCA100021 &&
              PacTrampoline(0x9ADF3021).words[3] == 0xF84107F0 &&
              PacTrampoline(0x9ADF3021).words[4] == 0x92607C21);
// ldraa x0, [x1, #-4096] -> sub x0, x1, #1, lsl #12; ldr x0, [x0]
static_assert(PacTrampoline(0xF8600420).count == 2 &&
              PacTrampoline(0xF8600420).words[0] == 0xD1400420 &&
              PacTrampoline(0xF8600420).words[1] == 0xF9400000);
// ldraa x0, [x1, #256]! -> add x1, x1, #256; ldr x0, [x1]
static_assert(PacTrampoline(0xF8220C20).count == 2 &&
              PacTrampoline(0xF8220C20).words[0] == 0x91040021 &&
              PacTrampoline(0xF8220C20).words[1] == 0xF9400020);
} // namespace PacSelfCheck

} // namespace Core::NCE
