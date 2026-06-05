#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace sh4xe::sdk::binary
{

inline constexpr size_t kRel32InstructionSize = 5;
inline constexpr uint8_t kNop = 0x90;

enum class Rel32Opcode : uint8_t
{
    Call = 0xE8,
    Jump = 0xE9,
};

uintptr_t ResolveRel32Target(uintptr_t instruction, int32_t displacement, size_t instructionSize = kRel32InstructionSize);
bool TryMakeRel32Displacement(uintptr_t instruction,
                              uintptr_t target,
                              int32_t* out,
                              size_t instructionSize = kRel32InstructionSize);
bool MakeRel32Instruction(Rel32Opcode opcode,
                          uintptr_t instruction,
                          uintptr_t target,
                          std::array<uint8_t, kRel32InstructionSize>* out);

bool WriteRel32Instruction(Rel32Opcode opcode, uintptr_t instruction, uintptr_t target);
bool WriteCall(uintptr_t instruction, uintptr_t target);
bool WriteJump(uintptr_t instruction, uintptr_t target);
bool WriteNops(uintptr_t address, size_t size);

} // namespace sh4xe::sdk::binary
