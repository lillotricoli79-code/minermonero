#include "jit/x64_jit.hpp"
#include <configuration.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <bit>
#include <xmmintrin.h>
#include <vector>

namespace {

using BinaryFunction = std::uint64_t (*)(std::uint64_t, std::uint64_t);
using BranchFunction = std::uint64_t (*)(std::uint64_t);
using RegisterFunction = std::uint64_t (*)(std::uint64_t*);
using RoundingFunction = void (*)(std::uint64_t*);
using MultiLaneFunction = void (*)(unique_rx::jit::LaneState*);

std::uint64_t expected_unsigned_high(std::uint64_t left, std::uint64_t right) {
    return static_cast<std::uint64_t>((static_cast<unsigned __int128>(left) * right) >> 64);
}

std::uint64_t expected_signed_high(std::uint64_t left, std::uint64_t right) {
    const auto signed_left = static_cast<__int128>(static_cast<std::int64_t>(left));
    const auto signed_right = static_cast<__int128>(static_cast<std::int64_t>(right));
    return static_cast<std::uint64_t>((signed_left * signed_right) >> 64);
}

bool check(bool condition, const char* message) {
    if (!condition) std::cerr << message << '\n';
    return condition;
}

std::uint64_t apply_integer(unique_rx::jit::IntegerOpcode opcode,
                            std::uint64_t destination, std::uint64_t source) {
    switch (opcode) {
        case unique_rx::jit::IntegerOpcode::iadd_rs:
            return destination + (source << 0);
        case unique_rx::jit::IntegerOpcode::isub_r:
            return destination - source;
        case unique_rx::jit::IntegerOpcode::imul_r:
            return destination * source;
        case unique_rx::jit::IntegerOpcode::imulh_r:
            return expected_unsigned_high(destination, source);
        case unique_rx::jit::IntegerOpcode::ismulh_r:
            return expected_signed_high(destination, source);
        case unique_rx::jit::IntegerOpcode::ineg_r:
            return ~destination + 1;
        case unique_rx::jit::IntegerOpcode::ixor_r:
            return destination ^ source;
        case unique_rx::jit::IntegerOpcode::iror_r:
            return std::rotr(destination, static_cast<int>(source & 63));
        case unique_rx::jit::IntegerOpcode::irol_r:
            return std::rotl(destination, static_cast<int>(source & 63));
        case unique_rx::jit::IntegerOpcode::iswap_r:
            return source;
    }
    return 0;
}

void run_scalar(const unique_rx::jit::IntegerProgram& program,
                std::array<std::uint64_t, 8>& registers,
                const std::uint8_t* scratchpad, std::uint64_t scratchpad_mask) {
    for (const auto& instruction : program) {
        if (instruction.opcode == unique_rx::jit::IntegerOpcode::iadd_m ||
            instruction.opcode == unique_rx::jit::IntegerOpcode::isub_m ||
            instruction.opcode == unique_rx::jit::IntegerOpcode::imul_m ||
            instruction.opcode == unique_rx::jit::IntegerOpcode::imulh_m ||
            instruction.opcode == unique_rx::jit::IntegerOpcode::ismulh_m) {
            const auto offset = (registers[instruction.src] +
                                 static_cast<std::uint64_t>(static_cast<std::int64_t>(instruction.immediate))) &
                                scratchpad_mask;
            std::uint64_t memory_value = 0;
            std::memcpy(&memory_value, scratchpad + offset, sizeof(memory_value));
            switch (instruction.opcode) {
                case unique_rx::jit::IntegerOpcode::iadd_m:
                    registers[instruction.dst] += memory_value;
                    break;
                case unique_rx::jit::IntegerOpcode::isub_m:
                    registers[instruction.dst] -= memory_value;
                    break;
                case unique_rx::jit::IntegerOpcode::imul_m:
                    registers[instruction.dst] *= memory_value;
                    break;
                case unique_rx::jit::IntegerOpcode::imulh_m:
                    registers[instruction.dst] = expected_unsigned_high(
                        registers[instruction.dst], memory_value);
                    break;
                case unique_rx::jit::IntegerOpcode::ismulh_m:
                    registers[instruction.dst] = expected_signed_high(
                        registers[instruction.dst], memory_value);
                    break;
                default:
                    break;
            }
        } else if (instruction.opcode == unique_rx::jit::IntegerOpcode::iadd_rs) {
            registers[instruction.dst] +=
                (registers[instruction.src] << instruction.shift) +
                static_cast<std::uint64_t>(static_cast<std::int64_t>(instruction.immediate));
        } else if (instruction.opcode == unique_rx::jit::IntegerOpcode::iswap_r) {
            std::swap(registers[instruction.dst], registers[instruction.src]);
        } else {
            registers[instruction.dst] = apply_integer(
                instruction.opcode, registers[instruction.dst], registers[instruction.src]);
        }
    }
}

} // namespace

int main() {
#if !defined(__x86_64__) && !defined(_M_X64)
    std::cout << "x86-64 JIT test skipped on this architecture\n";
    return 0;
#else
    using namespace unique_rx::jit;

    CodeBuffer unsigned_mul;
    unsigned_mul.align(64);
    const auto unsigned_offset = unsigned_mul.code_size();
    unsigned_mul.move_arg0_to_rax();
    unsigned_mul.multiply_high_unsigned();
    unsigned_mul.ret();
    ExecutableCode unsigned_code(unsigned_mul);
    const auto unsigned_fn = unsigned_code.entry<BinaryFunction>(unsigned_offset);

    CodeBuffer signed_mul;
    signed_mul.move_arg0_to_rax();
    signed_mul.multiply_high_signed();
    signed_mul.ret();
    ExecutableCode signed_code(signed_mul);
    const auto signed_fn = signed_code.entry<BinaryFunction>();

    CodeBuffer subtraction;
    subtraction.move_arg0_to_rax();
    subtraction.subtract_arg1_from_rax();
    subtraction.ret();
    ExecutableCode subtraction_code(subtraction);
    const auto subtraction_fn = subtraction_code.entry<BinaryFunction>();

    CodeBuffer branch;
    branch.move_arg0_to_rax();
    branch.test_rax();
    const auto zero = branch.make_label();
    branch.jump_if_zero(zero);
    branch.move_eax_imm1();
    branch.ret();
    branch.align(16);
    const auto zero_offset = branch.code_size();
    branch.bind(zero);
    branch.zero_eax();
    branch.ret();
    ExecutableCode branch_code(branch);
    const auto branch_fn = branch_code.entry<BranchFunction>();

    constexpr std::uint32_t register_index = 3;
    constexpr std::uint32_t condition_shift = RANDOMX_JUMP_OFFSET;
    constexpr std::uint32_t condition_mask =
        ((1U << RANDOMX_JUMP_BITS) - 1U) << condition_shift;
    constexpr std::int32_t branch_imm = 0;
    std::uint64_t branch_increment =
        static_cast<std::uint64_t>(static_cast<std::int64_t>(branch_imm)) |
        (std::uint64_t{1} << condition_shift);
    if (condition_shift > 0) branch_increment &= ~(std::uint64_t{1} << (condition_shift - 1));
    CodeBuffer randomx_branch;
    randomx_branch.emit_randomx_cbranch(register_index, branch_increment,
                                        condition_mask, 17, 18);
    ExecutableCode randomx_branch_code(randomx_branch);
    const auto randomx_branch_fn = randomx_branch_code.entry<RegisterFunction>();

    CodeBuffer cfround;
    cfround.emit_randomx_cfround(register_index, 0, false);
    ExecutableCode cfround_code(cfround);
    const auto cfround_fn = cfround_code.entry<RoundingFunction>();

    CodeBuffer cfround_v2_skip;
    cfround_v2_skip.emit_randomx_cfround(register_index, 0, true);
    ExecutableCode cfround_v2_skip_code(cfround_v2_skip);
    const auto cfround_v2_skip_fn = cfround_v2_skip_code.entry<RoundingFunction>();

    bool ok = true;
    ok &= check(unsigned_offset % 64 == 0, "JIT entry is not 64-byte aligned");
    ok &= check(unsigned_fn(0x123456789abcdef0ULL, 0xfedcba9876543210ULL) ==
                    expected_unsigned_high(0x123456789abcdef0ULL, 0xfedcba9876543210ULL),
                "IMULH lowering differs from the reference");
    ok &= check(signed_fn(0xf23456789abcdef0ULL, 0x8edcba9876543210ULL) ==
                    expected_signed_high(0xf23456789abcdef0ULL, 0x8edcba9876543210ULL),
                "ISMULH lowering differs from the reference");
    ok &= check(subtraction_fn(0, 1) == std::numeric_limits<std::uint64_t>::max(),
                "ISUB_R lowering does not wrap modulo 2^64");
    ok &= check(branch_fn(0) == 0 && branch_fn(5) == 1,
                "conditional rel32 fixup produced the wrong control flow");
    ok &= check(zero_offset % 16 == 0, "branch target alignment is incorrect");

    std::array<std::uint64_t, 8> registers{};
    registers[register_index] = std::numeric_limits<std::uint64_t>::max();
    ok &= check(randomx_branch_fn(registers.data()) == 17,
                "RandomX CBRANCH taken path is incorrect");
    ok &= check(registers[register_index] == 255,
                "RandomX CBRANCH register update is incorrect");
    registers[register_index] = 0;
    ok &= check(randomx_branch_fn(registers.data()) == 18,
                "RandomX CBRANCH fallthrough is incorrect");

    const auto original_mxcsr = _mm_getcsr();
    registers[register_index] = 2;
    cfround_fn(registers.data());
    ok &= check(((_mm_getcsr() >> 13) & 3U) == 2U,
                "RandomX CFROUND did not select the expected rounding mode");
    _mm_setcsr(original_mxcsr);
    registers[register_index] = 4;
    cfround_v2_skip_fn(registers.data());
    ok &= check(_mm_getcsr() == original_mxcsr,
                "RandomX v2 CFROUND should skip this rounding update");
    _mm_setcsr(original_mxcsr);

    using namespace unique_rx::jit;
    const std::vector<IntegerProgram> programs{
        {
            {IntegerOpcode::imulh_r, 0, 1},
            {IntegerOpcode::isub_r, 2, 3},
            {IntegerOpcode::ismulh_r, 4, 5},
            {IntegerOpcode::imulh_r, 6, 7},
            {IntegerOpcode::iadd_rs, 0, 1, 2, -17},
            {IntegerOpcode::imul_r, 2, 3},
            {IntegerOpcode::ineg_r, 4, 0},
            {IntegerOpcode::ixor_r, 6, 7},
            {IntegerOpcode::iror_r, 1, 3},
            {IntegerOpcode::irol_r, 5, 2},
            {IntegerOpcode::iswap_r, 0, 7},
            {IntegerOpcode::iadd_m, 0, 1, 0, -13},
            {IntegerOpcode::isub_m, 2, 3, 0, 9},
            {IntegerOpcode::imul_m, 4, 5, 0, -1},
            {IntegerOpcode::imulh_m, 6, 7, 0, 24},
            {IntegerOpcode::ismulh_m, 1, 0, 0, -32},
        },
        {
            {IntegerOpcode::ismulh_r, 0, 1},
            {IntegerOpcode::imulh_r, 2, 3},
            {IntegerOpcode::isub_r, 4, 5},
            {IntegerOpcode::ismulh_r, 6, 7},
            {IntegerOpcode::iadd_rs, 1, 0, 3, 5},
            {IntegerOpcode::imul_r, 3, 2},
            {IntegerOpcode::ixor_r, 5, 4},
            {IntegerOpcode::ineg_r, 7, 6},
            {IntegerOpcode::irol_r, 0, 5},
            {IntegerOpcode::iror_r, 2, 7},
            {IntegerOpcode::iswap_r, 3, 3},
        },
        {
            {IntegerOpcode::isub_r, 0, 1},
            {IntegerOpcode::imulh_r, 2, 3},
            {IntegerOpcode::ismulh_r, 4, 5},
            {IntegerOpcode::isub_r, 6, 7},
            {IntegerOpcode::ixor_r, 0, 7},
            {IntegerOpcode::iadd_rs, 2, 1, 1, -3},
            {IntegerOpcode::imul_r, 4, 0},
            {IntegerOpcode::ineg_r, 6, 6},
        },
        {
            {IntegerOpcode::imulh_r, 0, 1},
            {IntegerOpcode::ismulh_r, 2, 3},
            {IntegerOpcode::isub_r, 4, 5},
            {IntegerOpcode::imulh_r, 6, 7},
            {IntegerOpcode::iror_r, 0, 1},
            {IntegerOpcode::irol_r, 2, 3},
            {IntegerOpcode::iswap_r, 5, 7},
            {IntegerOpcode::iadd_rs, 4, 2, 0, 9},
            {IntegerOpcode::isub_m, 0, 1, 0, 0},
            {IntegerOpcode::imulh_m, 2, 3, 0, 16},
            {IntegerOpcode::iadd_m, 4, 5, 0, -8},
            {IntegerOpcode::ismulh_m, 6, 7, 0, 40},
            {IntegerOpcode::imul_m, 1, 2, 0, -24},
        },
    };
    for (std::size_t lane_count = 1; lane_count <= programs.size(); ++lane_count) {
        std::vector<std::array<std::uint64_t, 8>> lane_registers(lane_count);
        std::vector<std::array<std::uint64_t, 8>> lane_scratchpads(lane_count);
        for (std::size_t lane = 0; lane < lane_count; ++lane) {
            for (std::size_t index = 0; index < lane_registers[lane].size(); ++index) {
                lane_registers[lane][index] =
                    (0x123456789abcdef0ULL * (lane + 1)) ^
                    (0x9e3779b97f4a7c15ULL * (index + 1));
                lane_scratchpads[lane][index] =
                    (0xd6e8feb86659fd93ULL * (lane + 3)) ^
                    (0xa5a3564e27f8862bULL * (index + 5));
            }
        }
        lane_registers[0][0] = 0xf23456789abcdef0ULL;
        lane_registers[0][1] = 0x8edcba9876543210ULL;
        lane_registers[0][2] = 0;
        lane_registers[0][3] = 0xffffffffffffffffULL;
        if (lane_count > 1) {
            lane_registers[1][0] = 0x8123456789abcdefULL;
            lane_registers[1][1] = 0xfedcba9876543210ULL;
        }

        auto expected_lanes = lane_registers;
        std::vector<IntegerProgram> selected_programs(
            programs.begin(), programs.begin() + static_cast<std::ptrdiff_t>(lane_count));
        for (std::size_t lane = 0; lane < lane_count; ++lane) {
            run_scalar(selected_programs[lane], expected_lanes[lane],
                       reinterpret_cast<const std::uint8_t*>(lane_scratchpads[lane].data()),
                       7 * sizeof(std::uint64_t));
        }

        std::vector<LaneState> lane_states;
        lane_states.reserve(lane_count);
        for (std::size_t lane = 0; lane < lane_count; ++lane) {
            lane_states.push_back({
                lane_registers[lane].data(),
                reinterpret_cast<const std::uint8_t*>(lane_scratchpads[lane].data()),
                7 * sizeof(std::uint64_t),
            });
        }

        auto multi_lane_code = compile_multi_lane_integer_programs(selected_programs);
        ExecutableCode multi_lane_executable(multi_lane_code);
        multi_lane_executable.entry<MultiLaneFunction>()(lane_states.data());
        ok &= check(lane_registers == expected_lanes,
                    "interleaved multi-lane JIT differs from scalar execution");
    }
    if (!ok) return 1;
    std::cout << "W^X x86-64 codegen, alignment, integer opcodes, CBRANCH, CFROUND and interleaved 1–4 lane code passed.\n";
    return 0;
#endif
}