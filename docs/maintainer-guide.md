# Maintainer handoff

## Purpose and current state

3DS2U extracts supported 3DS containers, translates a subset of guest instructions
to C++20, and compiles them into a Wii U RPX. This is a prototype, not a complete
3DS emulator. An RPX building successfully does not prove a game boots. There is
no interpreter/JIT fallback for missing AOT blocks or dynamically loaded CRO code.

The graphics, audio, HID and service bridges include synthetic project protocols.
They are useful for authored demos but do not reproduce all retail OS/GPU traffic.
The README feature table and subsystem documents describe the supported subset.

## Repository map

| Path | Responsibility |
| --- | --- |
| `extractor/include`, `extractor/src` | Host CLI, 3DSX relocation, CIA/NCSD adapters, shared NCCH decryption, BLZ, key parsing |
| `extractor/tests` | Synthetic container fixtures, AES standard vectors, parser/crypto boundary checks |
| `translator/include`, `translator/src` | Binary layout, instruction/mode representation, CFG, translators and source emitter |
| `translator/tests` | Host execution of emitted arithmetic/memory/VFP/T32 code and decoder tests |
| `wiiu_runtime/include`, `wiiu_runtime/src` | PPC memory, dispatcher, GX2/AX/VPAD, kernel and HLE components |
| `wiiu_runtime/tests` | Host mocks and synthetic Wii U demos; platform stubs are not production APIs |
| `scripts` | Checked artifact cleanup and redacted privacy audit; tests under `scripts/tests` |
| `.githooks` | Optional staged-file privacy check; explicitly enable per clone |
| `docs` | Format decisions, implementation notes, subsystem limitations and workflows |
| `build` | Ignored tools, objects, test output, diagnostics and packages; never a source dependency |

The root demo generators are authored fixtures. Small checked-in `dummy_*.bin`
files are legacy synthetic fixtures, not game dumps. Root generated C++ is
disposable; only `wiiu_runtime/src/*.cpp` enters the runtime Makefile.

## Pipeline and generated interfaces

1. Build `extractor.exe` and `3ds2u_translator.exe` using README's MSVC commands.
   Use a developer command prompt; no personal Visual Studio installation path
   belongs in a checked-in command. Other C++20 compilers may work but need their
   own build invocation. SDK paths are configured through environment variables.
2. `build_3ds2u_app.cmd input.3dsx output.rpx` also accepts supported CIA/NCSD.
   It validates tools, cleans old generated artifacts, extracts, translates,
   installs generated sources, invokes make and publishes the selected output.
3. The extractor writes `extracted.code`, `extracted.exheader`, `extracted.romfs`
   and `extracted_romfs.cpp`. ExeFS `.code` alone has no section table. NCCH uses
   a separate decrypted ExHeader; 3DSX extraction synthesizes the required layout.
4. `CppEmitter` creates block functions and a global
   `InitDispatcher(wiiu::WiiUEngine&)`. Block arguments are CPUContext and
   WiiUMemory. Generated memory statements use the block's `memory` reference.
5. The dispatcher maps image sections, reserves/zeros backing including BSS,
   loads initialized bytes, registers block functions and sets the entry PC/mode
   from the translated block at the parsed ExHeader entry. Emission fails before
   writing the output if that entry has no block, or both ARM and Thumb blocks.
   Runtime `main` must not overwrite that parsed entry point; it passes `cpu.r[15]`
   to `Run`. Regenerate sources after a runtime or translator update, because old
   generated files leave the PC at 0.
6. The generated RomFS source supplies `GetPackagedRomFS()`; `packaged_romfs.cpp`
   provides a weak empty fallback. Main mounts embedded Level-3 RomFS. A new
   missing-RomFS build must replace old assets with an empty package.

Do not run unified builds concurrently: extraction names, generated sources and
object paths are shared. Cleanup preserves host executables, so rebuilding a
runtime never updates an old host tool. Close Cemu before replacing an RPX.

## Architectural invariants and decisions

- Guest integer/file bytes are little-endian. PPC is big-endian. Binary parsers
  read integers explicitly; WiiUMemory converts numeric 16/32-bit accesses while
  raw byte buffers retain their guest order. MMIO passes numeric values directly.
- O3DS/N3DS backing is 128/256 MiB, aligned to 4 KiB, with a canonical window at
  `0x08000000`. Generated low image VAs map to compact backing via MapImageRange;
  this is not a page table with permissions or faults (invariants in
  [guest-memory-mapping.md](guest-memory-mapping.md)). Heap reservations protect
  image/BSS backing.
- ARM instructions are four bytes; Thumb can be two or four. Dispatch keys encode
  Thumb in bit zero, while stored instruction addresses are canonical. ExHeader
  metadata does not identify mode boundaries or literal pools. The default CLI
  scans ARM linearly; use explicit Thumb ranges when known. Opt-in `--discover`
  follows reachable code instead; see [Reachability discovery](#reachability-discovery).
- Literal LDR bases use the individual instruction PC+8, not the block-entry PC.
  Other unsupported PC/shift/register-overlap cases need architectural review.
- VFP double registers are D0-D15; single registers S0-S31 alias their words.
  Decode operands only after identifying their format/precision. An unhandled
  ARM VFP word logs a warning and emits a runtime trap (a thrown
  `std::runtime_error` naming the guest address) under the instruction's own
  condition, never a no-op. Translation still completes, so data decoded as code
  does not abort a build; do not treat successful translation as support. This
  is not a complete guest Undefined Instruction exception implementation; see
  [issue #11](https://github.com/Wiirizon/3DS2U/issues/11).
  Unhandled non-VFP ARM words are the same: they emit a runtime trap under their
  condition and throw only when executed; unhandled Thumb words already fail at
  translation time. The cache hint PLD (condition 0xF) is skipped, as it has no
  architectural effect.
- CIA outer content encryption is CBC; NCCH region encryption is CTR. Plaintext
  NCCH content magic bypasses stale CIA encryption flags. Raw ExHeader hashes,
  validated ExeFS headers/hashes and bounded IVFC headers detect plaintext NCCH
  regions. Plaintext decisions do not relax bounds or hash checks.
- BLZ displacement is measured before decrementing the output cursor. Repeated
  identical-byte fixtures can hide off-by-one corruption; retain nonconstant
  pattern tests. Compressed-file hashes alone cannot validate decoded output.
- Kernel threading uses native workers plus a shared execution mutex. GX2 access
  remains on the main rendering context; host synchronization mocks are limited.

## Setup, testing and maintenance

Dependencies: Windows cmd/PowerShell for the master workflow, Python 3 plus Git
for privacy tooling, C++20 host compiler, devkitPPC, wut, GNU make, MSYS sh and
elf2rpl. Toolchain binaries/libraries are external dependencies; do not vendor a
private SDK. The history-cleanup tool, when needed, is external git-filter-repo.

Host test source files are maintained in each component's `tests` directory.
Build/run them from the repository root. Container tests link their adapter,
`ncch_extractor.cpp`, `key_store.cpp`, `crypto_engine.cpp`, and the runtime RomFS
reader; CIA tests additionally link translator `binary_loader.cpp`. VFP/T32 and
memory tests generate C++ execution sources which must then be compiled and run.
See the compiler commands in the subsystem docs and `scripts/run_host_tests.cmd`.

```powershell
powershell -NoProfile -File scripts/tests/clean_generated_tests.ps1
python -m unittest discover -s scripts/tests -p "test_*.py"
python scripts/audit_repository.py --scope working
python scripts/audit_repository.py --scope history
```

### Split translated units

`3ds2u_translator ... --split-instructions N` bounds each generated translation
unit to about `N` instructions (a block is never split; one larger block gets its
own unit; at most 10000 units). The unified build uses `N=4000`. Output:

- `translated_game.cpp`: image data and `InitDispatcher`, which calls
  `arm11::RegisterTranslatedBlocksNNNN(engine)` for each unit in order, then sets
  the entry PC/mode exactly as the single-file output does.
- `translated_game_blocks_NNNN.cpp`: block functions (internal linkage) and one
  registration function. `svc_handler.hpp` is included only where an SVC exists.

Without the option the translator writes one file exactly as before. Block order
and keys are deterministic, so generating twice gives identical files. The
translator streams every file to `<name>.3ds2u-tmp`, re-reads them to check that
each block key is registered exactly once, and only then renames them into place
(files whose bytes are unchanged keep their timestamp). Staging files are created
exclusively (`O_EXCL|O_NOFOLLOW` on POSIX, `_O_EXCL` on Windows), so a link or
directory planted at a `.3ds2u-tmp` name is refused and never followed; only a
leftover regular staging file of an earlier run is replaced. Every final and stale
match is checked before the first staged file is written, and again (as regular
file or absent) right before each rename. It removes stale
`<stem>_blocks_<digits>.cpp` regular files in the output directory; a directory
or link with such a name makes the run fail. The output directory itself must be
trusted: an attacker who can already rename files inside it can still race the
recheck. Progress is printed to stdout as
plain lines (`[3DS2U] progress phase=emit-units 3/12 units, ...`) with no terminal
control sequences; a failure prints `progress FAILED phase=...` and exits 1, and
`phase=complete` is printed only after every unit is published. The Makefile
compiles all `src/*.cpp`, so chunks need no Makefile change; `-O2` still applies
to every source (the older `TRANSLATED_OPT` variable does not exist).

RomFS is packaged by `extractor/include/romfs_source.hpp` as `"\xNN"` string
literals instead of a `0xNN,` list: about 20% less source and faster to compile,
but more peak compiler memory (see the pull request for measured numbers).
The unified workflow cleans runtime objects, so incremental speed is sacrificed
for reproducible fresh image builds. Manual builds can retain stale assets.
Tests: `translator/tests/split_units_tests.cpp`, `split_exec_driver.cpp` (split
vs unsplit execution compare) and `extractor/tests/romfs_source_tests.cpp`.

### Reachability discovery

`3ds2u_translator ... --discover` (opt-in) replaces the linear scan with
`DiscoverCode` in `translator/src/code_discovery.cpp`. Starting from the entry point
it follows B/BL, conditional branches, BLX immediate (into Thumb), BX/BLX register,
LDM/LDR/high-register writes to PC (dynamic: no successor), and Thumb 16/32-bit
forms, tracking ARM/Thumb state. Each address is decoded once; work is bounded by
`.text` size and output is address-sorted and deterministic.
PC-relative loads (ARM LDR/LDRB/LDRH/VLDR; Thumb LDR, LDRB, LDRH, LDRSB, LDRSH literal)
record the 4-byte word containing the address (aligned down) that is never decoded as code.

Limits:

- Only the entry point seeds discovery. Functions reached only through BX Rm,
  function pointers or jump tables are not found. The CLI prints the number of
  undecoded `.text` bytes so this is visible.
- `--thumb-range` does not seed discovery; with `--discover` it only constrains
  ARM targets. `--thumb` sets the entry state to Thumb and forbids ARM targets.
- Direct targets outside `.text` are external: not followed, not an error.
- Encodings that cannot be translated are not judged here; translation rejects them later.
- Dynamic PC writes are recognised by pattern; unrecognised encodings that write PC
  fall through.
- Wide conditional B.W (T3) is followed, but the Thumb translator does not support it,
  so emission fails explicitly.

Rejected inputs fail with `std::invalid_argument` naming the guest address:

- the same address decoded as both ARM and Thumb;
- a decoded instruction overlapping another instruction (including a T32 second
  halfword) or a literal-pool word, and a literal pool overlapping decoded code;
- a misaligned branch target, or an ARM target inside a forced Thumb range;
- execution falling off `.text` (or a truncated T32 instruction), and an entry point
  outside `.text` or misaligned.

Rejection messages keep the sentence and guest address above and add provenance in
parentheses: the first load reached that referenced the literal-pool word (later loads
are not recorded), and the instruction that reached the conflicting address, marked as
fall-through or branch. The provenance is a hint for classifying the address as code or
data, not a rule: check the flow that reaches it, such as a call that may not return or a
missing terminator. Provenance is diagnostic only; accepted inputs and flow rules are
unchanged.

Tests: `translator/tests/code_discovery_tests.cpp`, built by `scripts/run_host_tests.cmd`.

## Troubleshooting

| Symptom | Check next |
| --- | --- |
| Missing NCCH magic | Outer CIA CBC/key/content selection or corruption; inner CTR does not encrypt the magic |
| ExHeader/ExeFS hash mismatch | Exact crypto layer, requested key slot, plaintext detection, metadata integrity |
| Nonsensical decoded branches | BLZ output, mode selection, embedded data, PC semantics; do not hide missing blocks |
| Address below FCRAM | Image VA mapping emitted/installed, current host-tool version, actual faulting address |
| No translated block | Compare the translator `Entry point` line with the runtime `Fatal: no translated block` PC, mode and lookup key; a stale generated source may lack the entry. Then check CFG boundaries, unsupported control flow, CRO/dynamic code |
| Unsupported VFPv2 warning | Check the emitted code and whether the instruction is reachable; the emitted block now throws at runtime if the instruction executes (message names the guest address); the word may be an invalid encoding, a valid unsupported form or data decoded as code. See [issue #11](https://github.com/Wiirizon/3DS2U/issues/11) |
| Long compile | Source size, compiler CPU/RAM; the translator prints unit progress lines, so a stall inside one `translated_game_blocks_NNNN.cpp` points at that unit; lower `--split-instructions` to shrink units |
| Black screen | Read redacted runtime termination/PC logs before attributing failure to graphics; see the [runtime-validation record](runtime-validation.md) for a Cemu missing-block failure with unknown cause |
| Device Guard/Defender blocks a test | Record the blocked check; obtain approved execution, do not evade controls |

## Priorities and handoff boundaries

Prioritize correct reachable code/mode discovery, explicit unsupported instruction
handling, PC semantics, real IPC/SVC integration, and genuine PICA200 protocols.
ALU/register-shifter/PC-destination handling is partial; some unsupported ARM
operations still emit comments. CRO mapping does not create executable host code.
Full MMU, startup state, services and GPU/DSP behavior are incomplete. Do not
claim commercial compatibility from synthetic demos or a successful link.

When handing off, state which tools were rebuilt, which generated artifacts were
regenerated, exactly what was tested and any blocked checks. Use redacted synthetic
reproducers. Do not require an assistant or contributor to reconstruct decisions
from chat history. For privacy/history work, follow SECURITY.md and preserve
intentional authorship and third-party attribution.
