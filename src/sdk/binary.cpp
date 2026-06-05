#include "sdk/binary.h"

#include "sdk/memory.h"

#include <cstring>
#include <limits>

namespace sh4xe::sdk::binary
{

uintptr_t ResolveRel32Target(uintptr_t instruction, int32_t displacement, size_t instructionSize)
{
    return instruction + instructionSize + displacement;
}

bool TryMakeRel32Displacement(uintptr_t instruction, uintptr_t target, int32_t* out, size_t instructionSize)
{
    if (!out)
        return false;

    const auto delta = static_cast<int64_t>(target) - static_cast<int64_t>(instruction + instructionSize);
    if (delta < std::numeric_limits<int32_t>::min() || delta > std::numeric_limits<int32_t>::max())
        return false;

    *out = static_cast<int32_t>(delta);
    return true;
}

bool MakeRel32Instruction(Rel32Opcode opcode,
                          uintptr_t instruction,
                          uintptr_t target,
                          std::array<uint8_t, kRel32InstructionSize>* out)
{
    if (!out)
        return false;

    int32_t displacement = 0;
    if (!TryMakeRel32Displacement(instruction, target, &displacement))
        return false;

    (*out)[0] = static_cast<uint8_t>(opcode);
    std::memcpy(out->data() + 1, &displacement, sizeof(displacement));
    return true;
}

bool WriteRel32Instruction(Rel32Opcode opcode, uintptr_t instruction, uintptr_t target)
{
    std::array<uint8_t, kRel32InstructionSize> bytes = {};
    if (!MakeRel32Instruction(opcode, instruction, target, &bytes))
        return false;

    return WriteBytes(instruction, bytes.data(), bytes.size());
}

bool WriteCall(uintptr_t instruction, uintptr_t target)
{
    return WriteRel32Instruction(Rel32Opcode::Call, instruction, target);
}

bool WriteJump(uintptr_t instruction, uintptr_t target)
{
    return WriteRel32Instruction(Rel32Opcode::Jump, instruction, target);
}

bool WriteNops(uintptr_t address, size_t size)
{
    return FillBytes(address, kNop, size);
}

} // namespace sh4xe::sdk::binary
