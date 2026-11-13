#include "code_discovery.hpp"
#include "mem_translator.hpp"
#include "thumb_decoder.hpp"

#include <algorithm>
#include <cstddef>
#include <iomanip>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace arm11 {
namespace {

constexpr std::uint64_t kAddressSpace = std::uint64_t{1} << 32;

std::string HexAddress(std::uint64_t address) {
    std::ostringstream text;
    text << "0x" << std::hex << std::uppercase << std::setfill('0') << std::setw(8) << address;
    return text.str();
}

// A non-empty detail is appended in parentheses after the guest address.
[[noreturn]] void Reject(const std::string& what, std::uint64_t address,
                         const std::string& detail = {}) {
    std::string message = what + " at guest address " + HexAddress(address);
    if (!detail.empty()) message += " (" + detail + ")";
    throw std::invalid_argument(message);
}

// Successors of one instruction: at most one direct target, plus fall-through.
struct Flow {
    std::optional<CodeLocation> target;
    bool fall_through{};
};

// ARM encodings ---------------------------------------------------------------

constexpr bool IsArmAlways(std::uint32_t op) noexcept { return (op >> 28) == 0xEu; }

constexpr bool IsArmBlxImmediate(std::uint32_t op) noexcept {
    return (op & 0xFE000000u) == 0xFA000000u;
}

constexpr bool IsArmBranchImmediate(std::uint32_t op) noexcept {
    return ((op >> 25) & 7u) == 5u && (op >> 28) != 0xFu;
}

constexpr bool IsArmBx(std::uint32_t op) noexcept {
    return ((op >> 4) & 0x00FFFFFFu) == 0x0012FFF1u;
}

constexpr bool IsArmBlxRegister(std::uint32_t op) noexcept {
    return ((op >> 4) & 0x00FFFFFFu) == 0x0012FFF3u;
}

// Signed 24-bit word offset scaled to bytes; the caller adds the pipeline bias.
constexpr std::uint32_t ArmBranchOffset(std::uint32_t op) noexcept {
    std::uint32_t offset = op & 0x00FFFFFFu;
    if ((offset & 0x00800000u) != 0) offset |= 0xFF000000u;
    return offset << 2;
}

// Data-processing, single-word load and halfword load writes to Rd=PC are dynamic.
bool ArmWritesPc(std::uint32_t op) noexcept {
    const auto cls = (op >> 25) & 7u;
    const auto rd = (op >> 12) & 15u;
    const bool load = (op & (1u << 20)) != 0;
    if (cls == 0u && (op & 0x90u) == 0x90u) {  // multiply, swap, halfword transfers
        return load && (op & 0x60u) != 0 && rd == 15u;
    }
    if (cls <= 1u) {
        const auto alu = (op >> 21) & 15u;
        return (alu < 8u || alu > 11u) && rd == 15u;  // TST..CMN do not write Rd
    }
    return (cls == 2u || cls == 3u) && load && rd == 15u;
}

Flow ArmBranchFlow(std::uint32_t op, std::uint32_t pc) {
    if (IsArmBlxImmediate(op)) {
        // BLX imm enters Thumb; H (bit 24) adds a halfword, and the call returns.
        const auto target = pc + 8u + ArmBranchOffset(op) + ((op >> 23) & 2u);
        return {CodeLocation{target, InstructionSet::Thumb}, true};
    }
    // Only unconditional B ends flow; BL and conditional branches also fall through.
    const bool links = (op & (1u << 24)) != 0;
    return {CodeLocation{pc + 8u + ArmBranchOffset(op), InstructionSet::ARM},
            links || !IsArmAlways(op)};
}

Flow ArmFlow(const Instruction& inst) {
    const auto op = inst.opcode;
    if (IsArmBlxImmediate(op) || IsArmBranchImmediate(op)) return ArmBranchFlow(op, inst.address);
    if (IsArmBlxRegister(op)) return {std::nullopt, true};
    // Dynamic exits end flow only when unconditional; a predicated one may fall through.
    if (IsArmBx(op) || ArmWritesPc(op) || MemoryTranslator::IsLoadMultipleToPC(inst)) {
        return {std::nullopt, !IsArmAlways(op)};
    }
    return {std::nullopt, true};
}

// PC-relative loads: the word read is data, never code.
std::optional<std::uint32_t> ArmLiteral(const Instruction& inst) {
    const auto op = inst.opcode;
    const auto cls = (op >> 25) & 7u;
    if ((op & (1u << 20)) == 0 || ((op >> 16) & 15u) != 15u) return std::nullopt;
    std::uint32_t offset = 0;
    if (cls == 2u) {  // LDR, LDRB
        offset = op & 0xFFFu;
    } else if (cls == 0u && (op & 0x90u) == 0x90u && (op & (1u << 22)) != 0 && (op & 0x60u) != 0) {
        offset = ((op >> 4) & 0xF0u) | (op & 0xFu);  // LDRH, LDRSB, LDRSH immediate form
    } else if (cls == 6u) {  // VLDR
        offset = (op & 0xFFu) << 2;
    } else {
        return std::nullopt;
    }
    const std::uint32_t pc = inst.address + 8u;
    return (op & (1u << 23)) != 0 ? pc + offset : pc - offset;
}

// Thumb encodings -------------------------------------------------------------

bool IsWideConditionalBranch(std::uint32_t op) noexcept {
    return op > 0xFFFFu && ((op >> 16) & 0xF800u) == 0xF000u && (op & 0xD000u) == 0x8000u &&
           ((op >> 22) & 15u) < 14u;
}

// T3 conditional B.W: S:J2:J1:imm6:imm11:'0', signed 21 bits, no XOR on J1/J2.
CodeLocation WideConditionalTarget(std::uint32_t op, std::uint32_t pc) {
    const auto first = op >> 16, second = op & 0xFFFFu;
    const auto sign = (first >> 10) & 1u, j1 = (second >> 13) & 1u, j2 = (second >> 11) & 1u;
    std::uint32_t offset = (sign << 20) | (j2 << 19) | (j1 << 18) | ((first & 0x3Fu) << 12) |
                           ((second & 0x7FFu) << 1);
    if (sign != 0) offset |= 0xFFE00000u;
    return CodeLocation{pc + 4u + offset, InstructionSet::Thumb};
}

// Thumb-2 load literal with Rn=PC, U (bit 7) cleared: LDRB F81F, LDRH F83F, LDR F85F,
// LDRSB F91F, LDRSH F93F. The word at Align(PC,4) +- imm12 is data.
constexpr bool IsWideLiteralLoad(std::uint32_t first) noexcept {
    const auto masked = first & 0xFF7Fu;
    return masked == 0xF81Fu || masked == 0xF83Fu || masked == 0xF85Fu || masked == 0xF91Fu ||
           masked == 0xF93Fu;
}

std::optional<std::uint32_t> WideLiteral(std::uint32_t op, std::uint32_t pc) {
    const auto first = op >> 16;
    if (op <= 0xFFFFu || !IsWideLiteralLoad(first)) return std::nullopt;
    const std::uint32_t base = (pc + 4u) & ~3u;
    const std::uint32_t offset = op & 0xFFFu;
    return (op & (1u << 23)) != 0 ? base + offset : base - offset;
}

std::optional<std::uint32_t> NarrowLiteral(std::uint32_t op, std::uint32_t pc) {
    if ((op & 0xF800u) != 0x4800u) return std::nullopt;
    return ((pc + 4u) & ~3u) + (op & 0xFFu) * 4u;
}

std::optional<std::uint32_t> ThumbLiteral(const Instruction& inst) {
    if (inst.opcode > 0xFFFFu) return WideLiteral(inst.opcode, inst.address);
    return NarrowLiteral(inst.opcode, inst.address);
}

std::optional<CodeLocation> ThumbTargetOf(std::uint32_t op, std::uint32_t pc) {
    const auto target = ThumbTranslator::BranchTarget(op, pc);
    if (!target) return std::nullopt;
    return CodeLocation{*target, ThumbTranslator::IsBLXImmediate(op) ? InstructionSet::ARM
                                                                    : InstructionSet::Thumb};
}

// ADD/MOV with Rd = PC (DN:Rm field 15) are dynamic; CMP does not write Rd.
bool ThumbWritesPc(std::uint32_t op) noexcept {
    const auto rd = ((op >> 4) & 8u) | (op & 7u);
    const auto group = (op >> 8) & 3u;
    return (op & 0xFC00u) == 0x4400u && (group == 0u || group == 2u) && rd == 15u;
}

Flow WideThumbFlow(std::uint32_t op, std::uint32_t pc) {
    if (ThumbTranslator::IsWideBranch(op)) {
        // BL and BLX set bit 14 and return to the next instruction; B.W does not.
        return {ThumbTargetOf(op, pc), (op & 0x4000u) != 0};
    }
    if (IsWideConditionalBranch(op)) return {WideConditionalTarget(op, pc), true};
    return {std::nullopt, !ThumbTranslator::IsWideLoadPC(op)};
}

Flow NarrowThumbFlow(std::uint32_t op, std::uint32_t pc) {
    if (ThumbTranslator::IsConditionalBranch(op)) return {ThumbTargetOf(op, pc), true};
    if ((op & 0xF800u) == 0xE000u) return {ThumbTargetOf(op, pc), false};
    if (ThumbTranslator::IsExchange(op)) return {std::nullopt, (op & 0x80u) != 0};  // BLX links
    if (ThumbTranslator::IsPopPC(op) || ThumbWritesPc(op)) return {std::nullopt, false};
    return {std::nullopt, true};
}

Flow ThumbFlow(const Instruction& inst) {
    if (inst.opcode > 0xFFFFu) return WideThumbFlow(inst.opcode, inst.address);
    return NarrowThumbFlow(inst.opcode, inst.address);
}

Flow FlowOf(const Instruction& inst) {
    return inst.state == InstructionSet::Thumb ? ThumbFlow(inst) : ArmFlow(inst);
}

std::optional<std::uint32_t> LiteralOf(const Instruction& inst) {
    return inst.state == InstructionSet::Thumb ? ThumbLiteral(inst) : ArmLiteral(inst);
}

// A location waiting to be visited. `from` is the instruction that reached it (none for the entry);
// `fall_through` is true only for the sequential successor of `from`, false for branch targets.
struct Pending {
    CodeLocation location;
    std::optional<std::uint32_t> from;
    bool fall_through{};
};

// Worklist walker. Each address is decoded at most once, so work is bounded by .text size.
class Discoverer {
public:
    Discoverer(std::span<const std::uint8_t> text, std::uint32_t base, const DiscoveryOptions& options)
        : text_{text}, base_{base}, end_{std::uint64_t{base} + text.size()}, options_{options} {
        if (end_ > kAddressSpace) throw std::out_of_range("Text exceeds the 32-bit guest address space");
    }

    [[nodiscard]] DiscoveryResult Run(CodeLocation entry) {
        if (!InText(entry.address)) Reject("Entry point is outside .text", entry.address);
        Enqueue(entry, std::nullopt, false);
        while (!pending_.empty()) {
            const auto pending = pending_.back();
            pending_.pop_back();
            Visit(pending);
        }
        return Collect();
    }

private:
    [[nodiscard]] bool InText(std::uint64_t address) const noexcept {
        return address >= base_ && address < end_;
    }

    [[nodiscard]] bool InForcedRange(std::uint32_t address) const {
        return std::any_of(options_.thumb_ranges.begin(), options_.thumb_ranges.end(),
                           [address](const auto& range) {
                               return address >= range.first &&
                                      std::uint64_t{address} < std::uint64_t{range.first} + range.second;
                           });
    }

    [[nodiscard]] std::uint32_t ReadLE(std::uint64_t address, unsigned bytes) const {
        const auto offset = static_cast<std::size_t>(address - base_);
        std::uint32_t value = 0;
        for (unsigned i = 0; i < bytes; ++i) value |= std::uint32_t{text_[offset + i]} << (8 * i);
        return value;
    }

    [[nodiscard]] Instruction Fetch(CodeLocation location) const {
        const std::uint64_t address = location.address;
        if (location.state == InstructionSet::ARM) {
            if (end_ - address < 4) Reject("Execution falls off the end of .text", address);
            return {ReadLE(address, 4), location.address, InstructionSet::ARM};
        }
        if (end_ - address < 2) Reject("Execution falls off the end of .text", address);
        const auto first = ReadLE(address, 2);
        if (!ThumbTranslator::IsWidePrefix(first)) return {first, location.address, InstructionSet::Thumb};
        if (end_ - address < 4) Reject("Truncated T32 instruction", address);
        return {(first << 16) | ReadLE(address + 2, 2), location.address, InstructionSet::Thumb};
    }

    // True if any decoded instruction intersects [address, address + width).
    [[nodiscard]] bool CodeOverlaps(std::uint64_t address, std::uint32_t width) const {
        const std::uint64_t stop = address + width;
        const std::uint64_t first = address >= std::uint64_t{base_} + 2u ? address - 2u : address;
        for (std::uint64_t start = first; start < stop; start += 2) {
            const auto found = decoded_.find(static_cast<std::uint32_t>(start));
            if (found != decoded_.end() && start + found->second.Width() > address) return true;
        }
        return false;
    }

    // The pool word overlapping [address, address + width), if any.
    [[nodiscard]] std::optional<std::uint32_t> FindPool(std::uint64_t address, std::uint32_t width) const {
        for (std::uint64_t word = address & ~std::uint64_t{3}; word < address + width; word += 4) {
            if (pools_.contains(static_cast<std::uint32_t>(word))) return static_cast<std::uint32_t>(word);
        }
        return std::nullopt;
    }

    // "reached from 0x........", or empty when the location has no origin.
    [[nodiscard]] std::string ReachedFrom(const Pending& pending) const {
        return pending.from ? "reached from " + HexAddress(*pending.from) : std::string{};
    }

    // Names the first load that referenced the pool word (later loads are not tracked).
    [[nodiscard]] std::string PoolDetail(std::uint32_t word, const Pending& pending) const {
        std::string detail = "literal word; first referencing load reached at " +
                             HexAddress(pool_source_.at(word));
        if (pending.from) {
            detail += "; reached from " + HexAddress(*pending.from) + " by " +
                      (pending.fall_through ? "fall-through" : "branch");
        }
        return detail;
    }

    // Literals outside .text are external data and are not recorded.
    void RecordLiteral(std::uint32_t literal, std::uint32_t load) {
        const std::uint32_t word = literal & ~3u;
        if (word < base_ || std::uint64_t{word} + 4 > end_) return;
        if (CodeOverlaps(word, 4)) {
            Reject("Literal pool overlaps decoded code", word,
                   "literal referenced by the load at " + HexAddress(load));
        }
        pools_.insert(word);
        pool_source_.try_emplace(word, load);  // The first load to reference a word is its source.
    }

    void Enqueue(CodeLocation location, std::optional<std::uint32_t> from, bool fall_through) {
        const std::uint32_t alignment = location.state == InstructionSet::Thumb ? 2u : 4u;
        if (location.address % alignment != 0) Reject("Misaligned branch target", location.address);
        if (!InText(location.address)) return;  // External targets are not followed.
        if (location.state == InstructionSet::ARM &&
            (options_.all_thumb || InForcedRange(location.address))) {
            Reject("ARM state where Thumb is forced by --thumb or --thumb-range", location.address);
        }
        pending_.push_back(Pending{location, from, fall_through});
    }

    void Visit(const Pending& pending) {
        const auto location = pending.location;
        const auto known = decoded_.find(location.address);
        if (known != decoded_.end()) {
            if (known->second.state != location.state) {
                Reject("Address is decoded as both ARM and Thumb", location.address);
            }
            return;
        }
        const auto inst = Fetch(location);
        if (CodeOverlaps(inst.address, inst.Width())) {
            Reject("Decoded instruction overlaps another decoded instruction", inst.address,
                   ReachedFrom(pending));
        }
        if (const auto word = FindPool(inst.address, inst.Width())) {
            Reject("Decoded instruction overlaps a literal pool", inst.address, PoolDetail(*word, pending));
        }
        decoded_.emplace(inst.address, inst);
        if (const auto literal = LiteralOf(inst)) RecordLiteral(*literal, inst.address);
        const auto flow = FlowOf(inst);
        if (flow.target) Enqueue(*flow.target, inst.address, false);
        if (flow.fall_through) {
            const std::uint64_t next = std::uint64_t{inst.address} + inst.Width();
            if (next >= end_) Reject("Execution falls off the end of .text", next);
            Enqueue(CodeLocation{static_cast<std::uint32_t>(next), inst.state}, inst.address, true);
        }
    }

    [[nodiscard]] DiscoveryResult Collect() const {
        DiscoveryResult result;
        result.instructions.reserve(decoded_.size());
        for (const auto& entry : decoded_) result.instructions.push_back(entry.second);
        std::sort(result.instructions.begin(), result.instructions.end(),
                  [](const Instruction& a, const Instruction& b) { return a.address < b.address; });
        for (const auto word : pools_) result.literal_pools.push_back(DataRegion{word, 4u});
        return result;
    }

    std::span<const std::uint8_t> text_;
    std::uint32_t base_;
    std::uint64_t end_;
    const DiscoveryOptions& options_;
    std::unordered_map<std::uint32_t, Instruction> decoded_;
    std::set<std::uint32_t> pools_;
    std::unordered_map<std::uint32_t, std::uint32_t> pool_source_;  // pool word -> first referencing load
    std::vector<Pending> pending_;
};

} // namespace

DiscoveryResult DiscoverCode(std::span<const std::uint8_t> text, std::uint32_t base_address,
                             CodeLocation entry, const DiscoveryOptions& options) {
    return Discoverer{text, base_address, options}.Run(entry);
}

} // namespace arm11
