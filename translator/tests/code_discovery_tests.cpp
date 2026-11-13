#include "cfg_builder.hpp"
#include "code_discovery.hpp"
#include "decoder.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <initializer_list>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

// Synthetic reachability regressions: mixed ARM/Thumb flow, literal pools and
// rejected ambiguous inputs. Fixtures are raw encodings built in memory.
namespace {

using Bytes = std::vector<std::uint8_t>;
using arm11::CodeLocation;
using arm11::DiscoveryOptions;
using arm11::DiscoveryResult;
using arm11::InstructionSet;

constexpr std::uint32_t kBase = 0x00100000u;
constexpr CodeLocation kArmEntry{kBase, InstructionSet::ARM};
constexpr CodeLocation kThumbEntry{kBase, InstructionSet::Thumb};

void Check(bool value) {
    if (!value) throw std::runtime_error("Code discovery regression");
}

void Append16(Bytes& bytes, std::uint32_t value) {
    bytes.push_back(static_cast<std::uint8_t>(value));
    bytes.push_back(static_cast<std::uint8_t>(value >> 8));
}

void Append32(Bytes& bytes, std::uint32_t value) {
    Append16(bytes, value & 0xFFFFu);
    Append16(bytes, value >> 16);
}

Bytes Arm(std::initializer_list<std::uint32_t> words) {
    Bytes bytes;
    for (const auto word : words) Append32(bytes, word);
    return bytes;
}

// T32 instructions are written as two consecutive halfwords.
Bytes Thumb(std::initializer_list<std::uint32_t> halfwords) {
    Bytes bytes;
    for (const auto half : halfwords) Append16(bytes, half);
    return bytes;
}

Bytes Zeros(std::size_t count) { return Bytes(count, 0); }

Bytes Concat(std::initializer_list<Bytes> parts) {
    Bytes bytes;
    for (const auto& part : parts) bytes.insert(bytes.end(), part.begin(), part.end());
    return bytes;
}

arm11::DiscoveryResult Run(const Bytes& code, CodeLocation entry,
                           const DiscoveryOptions& options = {}) {
    return arm11::DiscoverCode(code, kBase, entry, options);
}

// Returns the rejection text; an accepted input is a failed expectation.
template <typename Action>
std::string Rejection(Action&& action) {
    try {
        action();
    } catch (const std::exception& error) {
        return error.what();
    }
    throw std::runtime_error("Expected rejection");
}

bool Contains(const std::string& text, const std::string& needle) {
    return text.find(needle) != std::string::npos;
}

bool StartsWith(const std::string& text, const std::string& prefix) {
    return text.compare(0, prefix.size(), prefix) == 0;
}

bool HasInstruction(const DiscoveryResult& result, std::uint32_t address, InstructionSet state) {
    for (const auto& inst : result.instructions) {
        if (inst.address == address && inst.state == state) return true;
    }
    return false;
}

bool Decoded(const DiscoveryResult& result, std::uint32_t address) {
    for (const auto& inst : result.instructions) {
        if (inst.address == address) return true;
    }
    return false;
}

// Output contract: sorted, non-overlapping, aligned per state, pools disjoint from code.
void CheckWellFormed(const DiscoveryResult& result) {
    std::uint64_t cursor = 0;
    for (const auto& inst : result.instructions) {
        Check(inst.address >= cursor);
        Check(inst.address % (inst.state == InstructionSet::Thumb ? 2u : 4u) == 0);
        cursor = std::uint64_t{inst.address} + inst.Width();
    }
    for (const auto& pool : result.literal_pools) {
        for (const auto& inst : result.instructions) {
            const std::uint64_t end = std::uint64_t{inst.address} + inst.Width();
            Check(end <= pool.address || inst.address >= std::uint64_t{pool.address} + pool.size);
        }
    }
}

// ARM LDR r0,[pc,#4] loads from 0x0C; the word there looks like B . and must stay data.
void TestArmLiteralPool() {
    const auto code = Arm({0xE59F0004u, 0xE12FFF1Eu, 0xE1A01001u, 0xEAFFFFFEu});
    const auto result = Run(code, kArmEntry);
    CheckWellFormed(result);
    Check(result.instructions.size() == 2);
    Check(HasInstruction(result, kBase, InstructionSet::ARM));
    Check(HasInstruction(result, kBase + 4, InstructionSet::ARM));
    Check(!Decoded(result, kBase + 12));
    Check(result.literal_pools.size() == 1);
    Check(result.literal_pools[0].address == kBase + 12 && result.literal_pools[0].size == 4);
}

// B skips the data word at +4; that word must never be decoded.
void TestArmSkipsData() {
    const auto code = Arm({0xEA000000u, 0x12345678u, 0xE12FFF1Eu});
    const auto result = Run(code, kArmEntry);
    CheckWellFormed(result);
    Check(result.instructions.size() == 2);
    Check(HasInstruction(result, kBase, InstructionSet::ARM));
    Check(HasInstruction(result, kBase + 8, InstructionSet::ARM));
    Check(!Decoded(result, kBase + 4));
    Check(result.literal_pools.empty());
}

// BLX immediate enters Thumb at +0x10; the Thumb callee returns with BX lr and
// execution resumes in ARM at +4.
void TestArmToThumbBlx() {
    const auto code = Concat({Arm({0xFA000002u, 0xE12FFF1Eu}), Zeros(8), Thumb({0x2000u, 0x4770u})});
    const auto result = Run(code, kArmEntry);
    CheckWellFormed(result);
    Check(result.instructions.size() == 4);
    Check(HasInstruction(result, kBase, InstructionSet::ARM));
    Check(HasInstruction(result, kBase + 4, InstructionSet::ARM));
    Check(HasInstruction(result, kBase + 0x10, InstructionSet::Thumb));
    Check(HasInstruction(result, kBase + 0x12, InstructionSet::Thumb));
    const auto blocks = arm11::CFGBuilder{}.BuildBlocks(result.instructions);
    Check(blocks.contains(arm11::DispatchKey({kBase + 0x10, InstructionSet::Thumb})));
}

// Wide BLX immediate at the Thumb entry returns to ARM code at +0x10.
void TestThumbToArmBlx() {
    const auto code = Concat({Thumb({0xF000u, 0xE806u, 0x4770u}), Zeros(10), Arm({0xE12FFF1Eu})});
    const auto result = Run(code, kThumbEntry);
    CheckWellFormed(result);
    Check(result.instructions.size() == 3);
    Check(HasInstruction(result, kBase, InstructionSet::Thumb));
    Check(result.instructions[0].opcode == 0xF000E806u);
    Check(HasInstruction(result, kBase + 4, InstructionSet::Thumb));
    Check(HasInstruction(result, kBase + 0x10, InstructionSet::ARM));
    const auto blocks = arm11::CFGBuilder{}.BuildBlocks(result.instructions);
    Check(blocks.contains(arm11::DispatchKey({kBase + 0x10, InstructionSet::ARM})));
}

// LDR r0,[pc,#4] (Thumb) reads the word at +8; it is a pool, not code.
void TestThumbLiteralPool() {
    const auto code = Concat({Thumb({0x4801u, 0x4770u}), Zeros(4), Arm({0xE7FEE7FEu})});
    const auto result = Run(code, kThumbEntry);
    CheckWellFormed(result);
    Check(result.instructions.size() == 2);
    Check(HasInstruction(result, kBase, InstructionSet::Thumb));
    Check(HasInstruction(result, kBase + 2, InstructionSet::Thumb));
    Check(result.literal_pools.size() == 1);
    Check(result.literal_pools[0].address == kBase + 8 && result.literal_pools[0].size == 4);
}

// Places a byte string at an offset inside a fixture image.
void Place(Bytes& image, std::size_t offset, const Bytes& part) {
    std::copy(part.begin(), part.end(), image.begin() + static_cast<std::ptrdiff_t>(offset));
}

// Wide load r0,[pc,#8] at +0x10 (PC aligned). U=0 reads +0x0C, U=1 reads +0x1C; both are data.
void CheckWideLiteral(std::uint32_t first, bool up) {
    Bytes image = Zeros(0x20);
    Place(image, 0x00, Thumb({0xE006u}));  // B +0x10
    Place(image, 0x10, Thumb({first | (up ? 0x80u : 0u), 0x0008u}));
    Place(image, 0x14, Thumb({0x4770u}));  // BX lr
    const std::uint32_t pool = up ? 0x1Cu : 0x0Cu;
    Place(image, pool, Arm({0xE7FEE7FEu}));
    const auto result = Run(image, kThumbEntry);
    CheckWellFormed(result);
    Check(result.instructions.size() == 3);
    Check(HasInstruction(result, kBase, InstructionSet::Thumb));
    Check(HasInstruction(result, kBase + 0x10, InstructionSet::Thumb));
    Check(HasInstruction(result, kBase + 0x14, InstructionSet::Thumb));
    Check(!Decoded(result, kBase + pool));
    Check(result.literal_pools.size() == 1);
    Check(result.literal_pools[0].address == kBase + pool && result.literal_pools[0].size == 4);
}

// LDRB F81F, LDRH F83F, LDR F85F, LDRSB F91F, LDRSH F93F, each with U=0 and U=1.
void TestWideLiteralLoads() {
    for (const std::uint32_t first : {0xF81Fu, 0xF83Fu, 0xF85Fu, 0xF91Fu, 0xF93Fu}) {
        CheckWideLiteral(first, false);
        CheckWideLiteral(first, true);
    }
}

// The load sits at +2, so PC = +6 and Align(PC,4) = +4. The pool must be computed from the
// aligned base: an unaligned base would move the word to +0x10 for offset 10.
void CheckLoadAtTwoModFour(std::initializer_list<std::uint32_t> load, std::uint32_t bx_offset) {
    Bytes image = Zeros(0x10);
    Place(image, 0x02, Thumb(load));
    Place(image, bx_offset, Thumb({0x4770u}));  // BX lr
    Place(image, 0x0C, Arm({0xE7FEE7FEu}));
    const auto result = Run(image, CodeLocation{kBase + 2, InstructionSet::Thumb});
    CheckWellFormed(result);
    Check(result.instructions.size() == 2);
    Check(HasInstruction(result, kBase + 2, InstructionSet::Thumb));
    Check(HasInstruction(result, kBase + bx_offset, InstructionSet::Thumb));
    Check(result.literal_pools.size() == 1);
    Check(result.literal_pools[0].address == kBase + 0x0Cu && result.literal_pools[0].size == 4);
}

void TestUnalignedPcLiteral() {
    CheckLoadAtTwoModFour({0x4802u}, 0x04);           // LDR r0,[pc,#8]
    CheckLoadAtTwoModFour({0xF8DFu, 0x000Au}, 0x06);  // LDR.W r0,[pc,#10]
    CheckLoadAtTwoModFour({0xF89Fu, 0x000Au}, 0x06);  // LDRB.W r0,[pc,#10]
}

// A 32-bit BL is one instruction; no instruction starts at the second halfword.
void TestWideBl() {
    const auto code = Concat({Thumb({0xF000u, 0xF806u, 0x4770u}), Zeros(10), Thumb({0x4770u})});
    const auto result = Run(code, kThumbEntry);
    CheckWellFormed(result);
    Check(result.instructions.size() == 3);
    Check(result.instructions[0].opcode == 0xF000F806u);
    Check(result.instructions[0].Width() == 4);
    Check(!Decoded(result, kBase + 2));
    Check(HasInstruction(result, kBase + 4, InstructionSet::Thumb));
    Check(HasInstruction(result, kBase + 0x10, InstructionSet::Thumb));
}

// BEQ explores both the taken target and the fall-through path.
void TestConditionalBranch() {
    const auto code = Arm({0xE3500000u, 0x0A000001u, 0xE3A00001u, 0xE12FFF1Eu,
                           0xE3A00002u, 0xE12FFF1Eu});
    const auto result = Run(code, kArmEntry);
    CheckWellFormed(result);
    Check(result.instructions.size() == 6);
    Check(HasInstruction(result, kBase + 8, InstructionSet::ARM));
    Check(HasInstruction(result, kBase + 0x10, InstructionSet::ARM));
}

// BL returns to the next instruction, which must be explored.
void TestCallFallsThrough() {
    const auto code = Arm({0xEB000002u, 0xE3A01005u, 0xE12FFF1Eu, 0u, 0xE3A00000u, 0xE12FFF1Eu});
    const auto result = Run(code, kArmEntry);
    CheckWellFormed(result);
    Check(result.instructions.size() == 5);
    Check(HasInstruction(result, kBase + 4, InstructionSet::ARM));
    Check(!Decoded(result, kBase + 0xC));
    Check(HasInstruction(result, kBase + 0x10, InstructionSet::ARM));
}

// The same address reached as ARM and as Thumb is ambiguous.
void TestSameAddressBothModes() {
    const auto code = Arm({0xFA000002u, 0xEA000001u, 0xE1A00000u, 0xE1A00000u, 0xE12FFF1Eu});
    const auto message = Rejection([&] { Run(code, kArmEntry); });
    Check(Contains(message, "0x00100010"));
}

// LDR literal points at +8, and the B at +4 then executes that pool word.
// B at +4 (0xEAFFFFFF) targets the next address, +8, so the edge is a branch, not a fall-through.
void TestCodeInLiteralPool() {
    const auto code = Arm({0xE59F0000u, 0xEAFFFFFFu, 0xE12FFF1Eu});
    const auto message = Rejection([&] { Run(code, kArmEntry); });
    Check(Contains(message, "0x00100008"));
    Check(Contains(message, "first referencing load reached at 0x00100000"));
    Check(Contains(message, "reached from 0x00100004 by branch"));
    Check(!Contains(message, "fall-through"));
}

void TestMisalignedEntry() {
    const auto code = Arm({0xE12FFF1Eu});
    const auto message = Rejection([&] { Run(code, CodeLocation{kBase + 2, InstructionSet::ARM}); });
    Check(Contains(message, "0x00100002"));
}

void TestFallOffEnd() {
    const auto code = Arm({0xE3A00000u});
    const auto message = Rejection([&] { Run(code, kArmEntry); });
    Check(Contains(message, "0x00100004"));
}

// B lands on an ARM address inside a forced Thumb range.
void TestForcedThumbRange() {
    const auto code = Arm({0xEA000002u, 0xE12FFF1Eu, 0u, 0u, 0xE12FFF1Eu});
    DiscoveryOptions options;
    options.thumb_ranges.push_back({kBase + 0x10, 4});
    const auto message = Rejection([&] { Run(code, kArmEntry, options); });
    Check(Contains(message, "0x00100010"));
}

// --thumb: an ARM entry is rejected, a Thumb entry is accepted.
void TestAllThumbEntryState() {
    DiscoveryOptions options;
    options.all_thumb = true;
    const auto arm_code = Arm({0xE12FFF1Eu});
    Check(!Rejection([&] { Run(arm_code, kArmEntry, options); }).empty());
    const auto thumb_code = Thumb({0x2000u, 0x4770u});
    const auto result = Run(thumb_code, kThumbEntry, options);
    CheckWellFormed(result);
    Check(result.instructions.size() == 2);
}

// B to 0x00200000 leaves .text: it is external, not followed, and not an error.
void TestExternalTargetIgnored() {
    const auto code = Arm({0xEA03FFFEu});
    const auto result = Run(code, kArmEntry);
    CheckWellFormed(result);
    Check(result.instructions.size() == 1);
    Check(result.literal_pools.empty());
}

// B . is a loop that must terminate with a single instruction.
void TestInfiniteLoop() {
    const auto code = Arm({0xEAFFFFFEu});
    const auto result = Run(code, kArmEntry);
    Check(result.instructions.size() == 1);
    Check(HasInstruction(result, kBase, InstructionSet::ARM));
}

// BLX imm (H=1) enters Thumb at +0x12, inside the ARM word at +0x10 that B reaches.
void TestArmThumbOverlap() {
    const auto code = Arm({0xFB000002u, 0xEA000001u, 0u, 0u, 0xE12FFF1Eu});
    const auto message = Rejection([&] { Run(code, kArmEntry); });
    Check(Contains(message, "0x00100012"));
}

// BL at +0 occupies +0..+3; B at +4 targets +2, inside that instruction.
void TestOverlapInsideT32() {
    const auto code = Thumb({0xF000u, 0xF806u, 0xE7FDu, 0x4770u});
    const auto message = Rejection([&] { Run(code, kThumbEntry); });
    Check(Contains(message, "0x00100002"));
}

// Existing rejection sentences stay at the start of the message; provenance is appended after.
void TestRejectionPrefixes() {
    const auto pool_code = Arm({0xE59F0000u, 0xEAFFFFFFu, 0xE12FFF1Eu});
    const auto pool_message = Rejection([&] { Run(pool_code, kArmEntry); });
    Check(StartsWith(pool_message, "Decoded instruction overlaps a literal pool at guest address 0x00100008"));
    const auto mode_code = Arm({0xFA000002u, 0xEA000001u, 0xE1A00000u, 0xE1A00000u, 0xE12FFF1Eu});
    const auto mode_message = Rejection([&] { Run(mode_code, kArmEntry); });
    Check(StartsWith(mode_message, "Address is decoded as both ARM and Thumb at guest address 0x00100010"));
}

// LDR r0,[pc,#4] at +0 reads the word at +0x0C. Flow falls through +4 and +8 into that word.
void TestPoolFallThroughProvenance() {
    const auto code = Arm({0xE59F0004u, 0xE1A00000u, 0xE1A00000u, 0xE12FFF1Eu});
    const auto message = Rejection([&] { Run(code, kArmEntry); });
    Check(StartsWith(message, "Decoded instruction overlaps a literal pool at guest address 0x0010000C"));
    Check(Contains(message, "first referencing load reached at 0x00100000"));
    Check(Contains(message, "reached from 0x00100008 by fall-through"));
}

// The same load, but B at +4 branches straight into the pool word at +0x0C.
void TestPoolBranchProvenance() {
    const auto code = Arm({0xE59F0004u, 0xEA000000u, 0xE1A00000u, 0xE12FFF1Eu});
    const auto message = Rejection([&] { Run(code, kArmEntry); });
    Check(StartsWith(message, "Decoded instruction overlaps a literal pool at guest address 0x0010000C"));
    Check(Contains(message, "first referencing load reached at 0x00100000"));
    Check(Contains(message, "reached from 0x00100004 by branch"));
}

// BL at +0 occupies +0..+3; B at +4 targets +2, inside that instruction.
void TestOverlapProvenance() {
    const auto code = Thumb({0xF000u, 0xF806u, 0xE7FDu, 0x4770u});
    const auto message = Rejection([&] { Run(code, kThumbEntry); });
    Check(StartsWith(message, "Decoded instruction overlaps another decoded instruction at guest address 0x00100002"));
    Check(Contains(message, "(reached from 0x00100004)"));
}

// B +0 reaches MOV at +8, which falls through to LDR at +0x0C; that load reads the code word at +8.
void TestLiteralOverlapsCodeProvenance() {
    const auto code = Arm({0xEA000000u, 0xE12FFF1Eu, 0xE1A00000u, 0xE51F000Cu});
    const auto message = Rejection([&] { Run(code, kArmEntry); });
    Check(StartsWith(message, "Literal pool overlaps decoded code at guest address 0x00100008"));
    Check(Contains(message, "(literal referenced by the load at 0x0010000C)"));
}

void TestDeterministic() {
    const auto code = Concat({Arm({0xFA000002u, 0xE12FFF1Eu}), Zeros(8), Thumb({0x2000u, 0x4770u})});
    const auto first = Run(code, kArmEntry);
    const auto second = Run(code, kArmEntry);
    Check(first.instructions.size() == second.instructions.size());
    for (std::size_t i = 0; i < first.instructions.size(); ++i) {
        Check(first.instructions[i].address == second.instructions[i].address);
        Check(first.instructions[i].opcode == second.instructions[i].opcode);
        Check(first.instructions[i].state == second.instructions[i].state);
    }
    Check(first.literal_pools.size() == second.literal_pools.size());
}

} // namespace

int main() {
    TestArmLiteralPool();
    TestArmSkipsData();
    TestArmToThumbBlx();
    TestThumbToArmBlx();
    TestThumbLiteralPool();
    TestWideLiteralLoads();
    TestUnalignedPcLiteral();
    TestWideBl();
    TestConditionalBranch();
    TestCallFallsThrough();
    TestSameAddressBothModes();
    TestCodeInLiteralPool();
    TestMisalignedEntry();
    TestFallOffEnd();
    TestForcedThumbRange();
    TestAllThumbEntryState();
    TestExternalTargetIgnored();
    TestInfiniteLoop();
    TestArmThumbOverlap();
    TestOverlapInsideT32();
    TestRejectionPrefixes();
    TestPoolFallThroughProvenance();
    TestPoolBranchProvenance();
    TestOverlapProvenance();
    TestLiteralOverlapsCodeProvenance();
    TestDeterministic();
    std::cout << "Code discovery regression checks passed (26 tests).\n";
    return 0;
}
