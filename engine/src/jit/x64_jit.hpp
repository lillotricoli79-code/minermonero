#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <optional>
#include <stdexcept>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace unique_rx::jit {

struct LaneState {
    std::uint64_t* registers;
    const std::uint8_t* scratchpad;
    std::uint64_t scratchpad_mask;
};

static_assert(offsetof(LaneState, scratchpad) == sizeof(void*));
static_assert(offsetof(LaneState, scratchpad_mask) == 2 * sizeof(void*));

class CodeBuffer {
public:
    using Label = std::size_t;

    Label make_label() {
        labels_.emplace_back();
        return labels_.size() - 1;
    }

    void bind(Label label) {
        auto& offset = label_offset(label);
        if (offset.has_value()) throw std::logic_error("label already bound");
        offset = code_.size();
        for (auto& fixup : fixups_) {
            if (fixup.label == label && !fixup.resolved) {
                patch_rel32(fixup.displacement_offset, fixup.instruction_end, *offset);
                fixup.resolved = true;
            }
        }
    }

    void align(std::size_t boundary) {
        if (boundary == 0 || (boundary & (boundary - 1)) != 0) {
            throw std::invalid_argument("alignment must be a power of two");
        }
        while ((code_.size() & (boundary - 1)) != 0) code_.push_back(0x90);
    }

    void move_arg0_to_rax() {
#if defined(_WIN32)
        emit({0x48, 0x89, 0xC8});
#else
        emit({0x48, 0x89, 0xF8});
#endif
    }

    void multiply_high_unsigned() {
#if defined(_WIN32)
        emit({0x48, 0xF7, 0xE2});
#else
        emit({0x48, 0xF7, 0xE6});
#endif
        emit({0x48, 0x89, 0xD0});
    }

    void multiply_high_signed() {
#if defined(_WIN32)
        emit({0x48, 0xF7, 0xEA});
#else
        emit({0x48, 0xF7, 0xEE});
#endif
        emit({0x48, 0x89, 0xD0});
    }

    void subtract_arg1_from_rax() {
#if defined(_WIN32)
        emit({0x48, 0x29, 0xD0});
#else
        emit({0x48, 0x29, 0xF0});
#endif
    }

    void emit_lane_isub(std::uint32_t lane, std::uint8_t dst, std::uint8_t src) {
        const auto displacement_dst = static_cast<std::uint32_t>(dst) * sizeof(std::uint64_t);
        const auto displacement_src = static_cast<std::uint32_t>(src) * sizeof(std::uint64_t);
        load_lane_pointer(lane);
        load_rax_from_r8(displacement_dst);
        emit({0x49, 0x2B, 0x80});
        emit_u32(displacement_src);
        store_rax_to_r8(displacement_dst);
    }

    void emit_lane_iadd_rs(std::uint32_t lane, std::uint8_t dst, std::uint8_t src,
                           std::uint8_t shift, std::int32_t immediate) {
        if (shift > 3) throw std::out_of_range("IADD_RS shift must be in [0, 3]");
        load_lane_pointer(lane);
        load_rax_from_r8(static_cast<std::uint32_t>(dst) * sizeof(std::uint64_t));
        load_rcx_from_r8(static_cast<std::uint32_t>(src) * sizeof(std::uint64_t));
        if (shift != 0) emit({0x48, 0xC1, 0xE1, shift});
        if (immediate != 0) {
            emit({0x48, 0x81, 0xC1});
            emit_u32(static_cast<std::uint32_t>(immediate));
        }
        emit({0x48, 0x01, 0xC8});
        store_rax_to_r8(static_cast<std::uint32_t>(dst) * sizeof(std::uint64_t));
    }

    void emit_lane_imul(std::uint32_t lane, std::uint8_t dst, std::uint8_t src) {
        load_lane_pointer(lane);
        load_rax_from_r8(static_cast<std::uint32_t>(dst) * sizeof(std::uint64_t));
        emit({0x49, 0x0F, 0xAF, 0x80});
        emit_u32(static_cast<std::uint32_t>(src) * sizeof(std::uint64_t));
        store_rax_to_r8(static_cast<std::uint32_t>(dst) * sizeof(std::uint64_t));
    }

    void emit_lane_ineg(std::uint32_t lane, std::uint8_t dst) {
        load_lane_pointer(lane);
        load_rax_from_r8(static_cast<std::uint32_t>(dst) * sizeof(std::uint64_t));
        emit({0x48, 0xF7, 0xD8});
        store_rax_to_r8(static_cast<std::uint32_t>(dst) * sizeof(std::uint64_t));
    }

    void emit_lane_ixor(std::uint32_t lane, std::uint8_t dst, std::uint8_t src) {
        load_lane_pointer(lane);
        load_rax_from_r8(static_cast<std::uint32_t>(dst) * sizeof(std::uint64_t));
        emit({0x49, 0x33, 0x80});
        emit_u32(static_cast<std::uint32_t>(src) * sizeof(std::uint64_t));
        store_rax_to_r8(static_cast<std::uint32_t>(dst) * sizeof(std::uint64_t));
    }

    void emit_lane_rotate(std::uint32_t lane, std::uint8_t dst, std::uint8_t src,
                          bool rotate_left) {
        load_lane_pointer(lane);
        load_rax_from_r8(static_cast<std::uint32_t>(dst) * sizeof(std::uint64_t));
        load_rcx_from_r8(static_cast<std::uint32_t>(src) * sizeof(std::uint64_t));
        emit({0x83, 0xE1, 0x3F});
        emit(rotate_left ? std::initializer_list<std::uint8_t>{0x48, 0xD3, 0xC0}
                         : std::initializer_list<std::uint8_t>{0x48, 0xD3, 0xC8});
        store_rax_to_r8(static_cast<std::uint32_t>(dst) * sizeof(std::uint64_t));
    }

    void emit_lane_iswap(std::uint32_t lane, std::uint8_t dst, std::uint8_t src) {
        if (dst == src) return;
        load_lane_pointer(lane);
        load_rax_from_r8(static_cast<std::uint32_t>(dst) * sizeof(std::uint64_t));
        load_rcx_from_r8(static_cast<std::uint32_t>(src) * sizeof(std::uint64_t));
        store_rax_to_r8(static_cast<std::uint32_t>(src) * sizeof(std::uint64_t));
        emit({0x49, 0x89, 0x88});
        emit_u32(static_cast<std::uint32_t>(dst) * sizeof(std::uint64_t));
    }

    void emit_lane_imulh(std::uint32_t lane, std::uint8_t dst, std::uint8_t src,
                         bool is_signed) {
        const auto displacement_dst = static_cast<std::uint32_t>(dst) * sizeof(std::uint64_t);
        const auto displacement_src = static_cast<std::uint32_t>(src) * sizeof(std::uint64_t);
        load_lane_pointer(lane);
        load_rax_from_r8(displacement_dst);
        emit(is_signed ? std::initializer_list<std::uint8_t>{0x49, 0xF7, 0xA8}
                       : std::initializer_list<std::uint8_t>{0x49, 0xF7, 0xA0});
        emit_u32(displacement_src);
        emit({0x49, 0x89, 0x90});
        emit_u32(displacement_dst);
    }

    void emit_lane_memory_op(std::uint32_t lane, std::uint8_t dst, std::uint8_t src,
                             std::int32_t immediate, std::uint8_t operation) {
        prepare_scratchpad_value(lane, src, immediate);
        const auto displacement_dst = static_cast<std::uint32_t>(dst) * sizeof(std::uint64_t);
        switch (operation) {
            case 0: // IADD_M
                emit({0x49, 0x8B, 0x90});
                emit_u32(displacement_dst);
                emit({0x48, 0x01, 0xCA});
                emit({0x49, 0x89, 0x90});
                emit_u32(displacement_dst);
                break;
            case 1: // ISUB_M
                emit({0x49, 0x8B, 0x90});
                emit_u32(displacement_dst);
                emit({0x48, 0x29, 0xCA});
                emit({0x49, 0x89, 0x90});
                emit_u32(displacement_dst);
                break;
            case 2: // IMUL_M
                emit({0x49, 0x8B, 0x90});
                emit_u32(displacement_dst);
                emit({0x48, 0x0F, 0xAF, 0xD1});
                emit({0x49, 0x89, 0x90});
                emit_u32(displacement_dst);
                break;
            case 3: // IMULH_M
            case 4: // ISMULH_M
                load_rax_from_r8(displacement_dst);
                emit(operation == 3 ? std::initializer_list<std::uint8_t>{0x48, 0xF7, 0xE1}
                                    : std::initializer_list<std::uint8_t>{0x48, 0xF7, 0xE9});
                emit({0x49, 0x89, 0x90});
                emit_u32(displacement_dst);
                break;
            default:
                throw std::invalid_argument("unsupported RandomX memory operation");
        }
    }

    void test_rax() { emit({0x48, 0x85, 0xC0}); }

    void jump_if_zero(Label label) {
        emit({0x0F, 0x84});
        const auto displacement_offset = code_.size();
        emit_u32(0);
        const auto instruction_end = code_.size();
        auto& target = label_offset(label);
        if (target.has_value()) {
            patch_rel32(displacement_offset, instruction_end, *target);
        } else {
            fixups_.push_back({label, displacement_offset, instruction_end, false});
        }
    }

    void move_eax_imm1() { emit({0xB8, 0x01, 0x00, 0x00, 0x00}); }
    void zero_eax() { emit({0x31, 0xC0}); }
    void ret() { emit({0xC3}); }

    void begin_lane_table_function() { move_arg0_to_r10(); }

    void emit_randomx_cbranch(std::uint32_t register_index,
                              std::uint64_t increment,
                              std::uint64_t condition_mask,
                              std::uint32_t target_pc,
                              std::uint32_t fallthrough_pc) {
        move_arg0_to_r8();
        load_rax_from_r8(register_index * sizeof(std::uint64_t));
        move_rcx_imm64(increment);
        emit({0x48, 0x01, 0xC8});
        store_rax_to_r8(register_index * sizeof(std::uint64_t));
        move_rcx_imm64(condition_mask);
        emit({0x48, 0x85, 0xC8});

        const auto taken = make_label();
        jump_if_zero(taken);
        move_eax_imm32(fallthrough_pc);
        ret();
        bind(taken);
        move_eax_imm32(target_pc);
        ret();
    }

    void emit_randomx_cfround(std::uint32_t register_index,
                              std::uint8_t rotate_right,
                              bool randomx_v2) {
        move_arg0_to_r8();
        load_rax_from_r8(register_index * sizeof(std::uint64_t));
        emit({0x48, 0xC1, 0xC8, rotate_right});

        if (randomx_v2) {
            emit({0xA9});
            emit_u32(60);
            const auto update_rounding = make_label();
            jump_if_zero(update_rounding);
            ret();
            bind(update_rounding);
        }

        emit({0x83, 0xE0, 0x03});
        emit({0xC1, 0xE0, 0x0D});
        emit({0x41, 0x89, 0xC1});
        emit({0x48, 0x83, 0xEC, 0x08});
        emit({0x0F, 0xAE, 0x1C, 0x24});
        emit({0x8B, 0x04, 0x24});
        emit({0x25, 0xFF, 0x9F, 0xFF, 0xFF});
        emit({0x44, 0x09, 0xC8});
        emit({0x89, 0x04, 0x24});
        emit({0x0F, 0xAE, 0x14, 0x24});
        emit({0x48, 0x83, 0xC4, 0x08});
        ret();
    }

    const std::vector<std::uint8_t>& bytes() const { return code_; }

    std::size_t code_size() const { return code_.size(); }

    void validate() const {
        for (const auto& fixup : fixups_) {
            if (!fixup.resolved) throw std::logic_error("unbound branch label");
        }
    }

private:
    struct Fixup {
        Label label;
        std::size_t displacement_offset;
        std::size_t instruction_end;
        bool resolved;
    };

    std::vector<std::uint8_t> code_;
    std::vector<std::optional<std::size_t>> labels_;
    std::vector<Fixup> fixups_;

    std::optional<std::size_t>& label_offset(Label label) {
        if (label >= labels_.size()) throw std::out_of_range("invalid label");
        return labels_[label];
    }

    void emit(std::initializer_list<std::uint8_t> bytes) {
        code_.insert(code_.end(), bytes.begin(), bytes.end());
    }

    void emit_u32(std::uint32_t value) {
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(&value);
        code_.insert(code_.end(), bytes, bytes + sizeof(value));
    }

    void emit_u64(std::uint64_t value) {
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(&value);
        code_.insert(code_.end(), bytes, bytes + sizeof(value));
    }

    void move_arg0_to_r8() {
#if defined(_WIN32)
        emit({0x4C, 0x8B, 0xC1});
#else
        emit({0x4C, 0x8B, 0xC7});
#endif
    }

    void load_rax_from_r8(std::uint32_t offset) {
        emit({0x49, 0x8B, 0x80});
        emit_u32(offset);
    }

    void load_rcx_from_r8(std::uint32_t offset) {
        emit({0x49, 0x8B, 0x88});
        emit_u32(offset);
    }

    void store_rax_to_r8(std::uint32_t offset) {
        emit({0x49, 0x89, 0x80});
        emit_u32(offset);
    }

    void move_rcx_imm64(std::uint64_t value) {
        emit({0x48, 0xB9});
        emit_u64(value);
    }

    void move_arg0_to_r10() {
#if defined(_WIN32)
        emit({0x4C, 0x8B, 0xD1});
#else
        emit({0x4C, 0x8B, 0xD7});
#endif
    }

    void load_lane_pointer(std::uint32_t lane) {
        emit({0x4D, 0x8B, 0x82});
        emit_u32(lane * sizeof(LaneState) + offsetof(LaneState, registers));
    }

    void prepare_scratchpad_value(std::uint32_t lane, std::uint8_t src,
                                  std::int32_t immediate) {
        const auto base = lane * sizeof(LaneState);
        emit({0x4D, 0x8B, 0x82});
        emit_u32(base + offsetof(LaneState, registers));
        emit({0x4D, 0x8B, 0x8A});
        emit_u32(base + offsetof(LaneState, scratchpad));
        emit({0x4D, 0x8B, 0x9A});
        emit_u32(base + offsetof(LaneState, scratchpad_mask));
        load_rax_from_r8(static_cast<std::uint32_t>(src) * sizeof(std::uint64_t));
        if (immediate != 0) {
            emit({0x48, 0x05});
            emit_u32(static_cast<std::uint32_t>(immediate));
        }
        emit({0x4C, 0x21, 0xD8});
        emit({0x4C, 0x01, 0xC8});
        emit({0x48, 0x8B, 0x08});
    }

    void move_eax_imm32(std::uint32_t value) {
        emit({0xB8});
        emit_u32(value);
    }

    void patch_rel32(std::size_t displacement_offset, std::size_t instruction_end,
                     std::size_t target_offset) {
        const auto displacement = static_cast<std::int64_t>(target_offset) -
                                  static_cast<std::int64_t>(instruction_end);
        if (displacement < std::numeric_limits<std::int32_t>::min() ||
            displacement > std::numeric_limits<std::int32_t>::max()) {
            throw std::out_of_range("branch displacement exceeds rel32");
        }
        const auto relative = static_cast<std::int32_t>(displacement);
        std::memcpy(code_.data() + displacement_offset, &relative, sizeof(relative));
    }
};

class ExecutableCode {
public:
    explicit ExecutableCode(const CodeBuffer& buffer) {
        buffer.validate();
        size_ = buffer.code_size();
        if (size_ == 0) throw std::invalid_argument("cannot allocate an empty JIT buffer");
#if defined(_WIN32)
        SYSTEM_INFO info{};
        GetSystemInfo(&info);
        if (info.dwPageSize == 0 ||
            size_ > std::numeric_limits<std::size_t>::max() - info.dwPageSize + 1) {
            throw std::runtime_error("invalid Windows page size for JIT allocation");
        }
        allocation_size_ = ((size_ + info.dwPageSize - 1) / info.dwPageSize) * info.dwPageSize;
        memory_ = VirtualAlloc(nullptr, allocation_size_, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (memory_ == nullptr) throw std::runtime_error("VirtualAlloc failed");
        std::memcpy(memory_, buffer.bytes().data(), size_);
        DWORD old_protection = 0;
        if (!VirtualProtect(memory_, allocation_size_, PAGE_EXECUTE_READ, &old_protection)) {
            VirtualFree(memory_, 0, MEM_RELEASE);
            memory_ = nullptr;
            throw std::runtime_error("VirtualProtect failed");
        }
        FlushInstructionCache(GetCurrentProcess(), memory_, size_);
#else
        const long system_page_size = sysconf(_SC_PAGESIZE);
        if (system_page_size <= 0) {
            throw std::runtime_error("sysconf(_SC_PAGESIZE) failed");
        }
        const auto page_size = static_cast<std::size_t>(system_page_size);
        if (size_ > std::numeric_limits<std::size_t>::max() - page_size + 1) {
            throw std::runtime_error("JIT allocation size overflow");
        }
        allocation_size_ = ((size_ + page_size - 1) / page_size) * page_size;
        memory_ = mmap(nullptr, allocation_size_, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (memory_ == MAP_FAILED) {
            memory_ = nullptr;
            throw std::runtime_error("mmap failed");
        }
        std::memcpy(memory_, buffer.bytes().data(), size_);
        if (mprotect(memory_, allocation_size_, PROT_READ | PROT_EXEC) != 0) {
            munmap(memory_, allocation_size_);
            memory_ = nullptr;
            throw std::runtime_error("mprotect failed");
        }
        __builtin___clear_cache(static_cast<char*>(memory_),
                                static_cast<char*>(memory_) + size_);
#endif
    }

    ~ExecutableCode() {
        if (memory_ == nullptr) return;
#if defined(_WIN32)
        VirtualFree(memory_, 0, MEM_RELEASE);
#else
        munmap(memory_, allocation_size_);
#endif
    }

    ExecutableCode(const ExecutableCode&) = delete;
    ExecutableCode& operator=(const ExecutableCode&) = delete;

    template<typename Function>
    Function entry(std::size_t offset = 0) const {
        if (offset >= size_) throw std::out_of_range("entry offset outside code");
        return reinterpret_cast<Function>(static_cast<std::uint8_t*>(memory_) + offset);
    }

private:
    void* memory_ = nullptr;
    std::size_t size_ = 0;
    std::size_t allocation_size_ = 0;
};

enum class IntegerOpcode {
    iadd_rs,
    isub_r,
    iadd_m,
    isub_m,
    imul_r,
    imul_m,
    imulh_r,
    imulh_m,
    ismulh_r,
    ismulh_m,
    ineg_r,
    ixor_r,
    iror_r,
    irol_r,
    iswap_r,
};

struct IntegerInstruction {
    IntegerOpcode opcode;
    std::uint8_t dst;
    std::uint8_t src;
    std::uint8_t shift = 0;
    std::int32_t immediate = 0;
    std::uint8_t memory_mode = 0;
};

using IntegerProgram = std::vector<IntegerInstruction>;

inline CodeBuffer compile_multi_lane_integer_programs(
    const std::vector<IntegerProgram>& programs) {
    if (programs.empty() || programs.size() > 4) {
        throw std::invalid_argument("multi-lane JIT supports one to four lanes");
    }
    CodeBuffer code;
    code.begin_lane_table_function();
    std::size_t instruction_count = 0;
    for (const auto& program : programs) {
        if (program.size() > instruction_count) instruction_count = program.size();
    }
    for (std::size_t index = 0; index < instruction_count; ++index) {
        for (std::size_t lane = 0; lane < programs.size(); ++lane) {
            if (index >= programs[lane].size()) continue;
            const auto& instruction = programs[lane][index];
            if (instruction.dst >= 8 || instruction.src >= 8) {
                throw std::out_of_range("RandomX integer register index must be below 8");
            }
            switch (instruction.opcode) {
                case IntegerOpcode::isub_r:
                    code.emit_lane_isub(static_cast<std::uint32_t>(lane),
                                        instruction.dst, instruction.src);
                    break;
                case IntegerOpcode::iadd_rs:
                    code.emit_lane_iadd_rs(static_cast<std::uint32_t>(lane),
                                           instruction.dst, instruction.src,
                                           instruction.shift, instruction.immediate);
                    break;
                case IntegerOpcode::iadd_m:
                    code.emit_lane_memory_op(static_cast<std::uint32_t>(lane),
                                             instruction.dst, instruction.src,
                                             instruction.immediate, 0);
                    break;
                case IntegerOpcode::isub_m:
                    code.emit_lane_memory_op(static_cast<std::uint32_t>(lane),
                                             instruction.dst, instruction.src,
                                             instruction.immediate, 1);
                    break;
                case IntegerOpcode::imul_r:
                    code.emit_lane_imul(static_cast<std::uint32_t>(lane),
                                        instruction.dst, instruction.src);
                    break;
                case IntegerOpcode::imul_m:
                    code.emit_lane_memory_op(static_cast<std::uint32_t>(lane),
                                             instruction.dst, instruction.src,
                                             instruction.immediate, 2);
                    break;
                case IntegerOpcode::imulh_r:
                    code.emit_lane_imulh(static_cast<std::uint32_t>(lane),
                                         instruction.dst, instruction.src, false);
                    break;
                case IntegerOpcode::imulh_m:
                    code.emit_lane_memory_op(static_cast<std::uint32_t>(lane),
                                             instruction.dst, instruction.src,
                                             instruction.immediate, 3);
                    break;
                case IntegerOpcode::ismulh_r:
                    code.emit_lane_imulh(static_cast<std::uint32_t>(lane),
                                         instruction.dst, instruction.src, true);
                    break;
                case IntegerOpcode::ismulh_m:
                    code.emit_lane_memory_op(static_cast<std::uint32_t>(lane),
                                             instruction.dst, instruction.src,
                                             instruction.immediate, 4);
                    break;
                case IntegerOpcode::ineg_r:
                    code.emit_lane_ineg(static_cast<std::uint32_t>(lane), instruction.dst);
                    break;
                case IntegerOpcode::ixor_r:
                    code.emit_lane_ixor(static_cast<std::uint32_t>(lane),
                                        instruction.dst, instruction.src);
                    break;
                case IntegerOpcode::iror_r:
                    code.emit_lane_rotate(static_cast<std::uint32_t>(lane),
                                          instruction.dst, instruction.src, false);
                    break;
                case IntegerOpcode::irol_r:
                    code.emit_lane_rotate(static_cast<std::uint32_t>(lane),
                                          instruction.dst, instruction.src, true);
                    break;
                case IntegerOpcode::iswap_r:
                    code.emit_lane_iswap(static_cast<std::uint32_t>(lane),
                                         instruction.dst, instruction.src);
                    break;
            }
        }
    }
    code.ret();
    return code;
}

} // namespace unique_rx::jit