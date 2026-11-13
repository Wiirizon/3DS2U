# CRO module translation design

Status: proposal for issue #22. No code changes are made by this document. Paths are
relative to the repository root. The issue names `runtime/` and `translator/`; the
runtime directory is `wiiu_runtime/` and the translator is `translator/`.

## Current state

**Loader.** `CROLoader` maps an unfixed CRO0 image into FCRAM, applies relocations and
resolves imports (`wiiu_runtime/src/cro_loader.cpp:175`, declared in
`wiiu_runtime/include/cro_loader.hpp:37`). It rejects linked images, static CRS
relocations and unknown relocation types (`cro_loader.cpp:46`, `:56`, `:143`). It
does not extract CRS, call module init/exit, or create PowerPC code
(`docs/cro-modules.md:76-79`).

**Code policy.** `CROLoader::SetCodePolicy` is an opt-in callback asked once per load,
after relocation and before the module is visible (`cro_loader.hpp:31-34`,
`cro_loader.cpp:155-163`, `cro_loader.cpp:305`). A `false` result or a throw rejects the
load with `CRO module code is not translated`, and nothing is committed
(`cro_loader.cpp:305-311`).

**Policy is not wired.** Outside the loader and its test, nothing calls
`SetCodePolicy` (checked with grep). `main.cpp` only sets a module-name lookup for
diagnostics, under `DS2U_MISSING_BLOCK_DETAIL` (`wiiu_runtime/src/main.cpp:40-42`,
`:88-89`). `docs/svc-runtime.md:129` says the same.

**Engine.** Translated blocks live in `block_table`, keyed by absolute guest address
with the Thumb bit as the mode (`wiiu_runtime/include/wiiu_engine.hpp:23`, `:65`).
`RegisterBlock` throws once execution has started (`wiiu_runtime/src/wiiu_engine.cpp:22`).
Blocks therefore have to be registered at startup. A missing block halts the main CPU
(`wiiu_engine.cpp:59-70`); there is no interpreter fallback (`wiiu_engine.hpp:44-47`).

**Policy check is weak.** `HasBlockInRange` (`wiiu_engine.cpp:29-37`) only proves that
some block starts inside the segment range. `docs/svc-runtime.md:129-131` says this
explicitly. Blocks are not tied to a module identity, and `UnloadModule` does not remove
engine blocks (`cro_loader.cpp:326-335`). A later module loaded at the same address
would match blocks translated for an earlier one. This is not covered by any test.

**Translator.** The translator has no CRO awareness. A grep for `cro` in
`translator/` finds nothing relevant. Its input is an uncompressed `.code` file plus an
optional decrypted ExHeader (`translator/include/binary_loader.hpp:26-37`). The entry
point must be in `.text` (`translator/src/code_discovery.cpp:228`). The output is C++
with `RegisterBlock(address, function)` calls (`translator/src/cpp_emitter.cpp:285-311`).

**Absolute addresses are baked in.** Branch targets are emitted as constants:
`cpu.r[15] = 0x...` (`translator/src/branch_translator.cpp:39`). Image sections are
copied from file bytes at their virtual addresses (`cpp_emitter.cpp:127-132`,
`:267-275`). Literal pools outside `.text` are not recorded
(`code_discovery.cpp:306-307`, `RecordLiteral`). Code translated for one address and bytes is only
correct for that address and those bytes.

**Existing tests.** `wiiu_runtime/tests/cro_tests.cpp` builds synthetic CRO images in
memory (`Image()`, `:28`) and covers the policy (`:186-200`). Nothing in `translator/tests/`
covers CRO.

## What a CRO0 module contains

Field offsets below are from the loader code, not re-checked against the 3dbrew layout
that `docs/cro-modules.md:55-56` cites. Treat them as **unverified** until compared with
that reference.

| Part | Where the loader reads it | Notes |
| --- | --- | --- |
| Hashes | file offset `0x00`-`0x7F` (four SHA-256) | Coverage not verified. `cro-modules.md:3-4` |
| Header | `0x80`-`0x137`, magic `CRO0` at `0x80` | `cro_loader.cpp:42`. Header words `h[]` |
| Code/rodata/data | file offsets from segment table | Copied in place, offsets preserved (`cro-modules.md:9-14`) |
| BSS | after page-rounded file length | Zeroed; size from header (`cro_loader.cpp:189`) |
| Segment table | `p.Entry(3, i)`, 12 bytes each | `offset, length, type` with type 0 code, 1 rodata, 2 data, 3 BSS (`cro_loader.cpp:199`) |
| Name | string table, `p.h[16]` | Must be unique among loaded modules (`cro_loader.cpp:186`) |
| Exports (named) | export table, `p.h[]` index | Duplicate names fail (`cro_loader.cpp:229`) |
| Exports (indexed) | index table | Used by indexed imports |
| Imports (named) | import table | Resolved against the shared symbol table (`cro_loader.cpp:265`) |
| Imports (module slices) | module import table, 8-byte entries | Indexed or anonymous; provider must be loaded (`cro_loader.cpp:273-290`) |
| Relocation batches | 12-byte records with type, flags, addend | Explicit signed addends (`cro_loader.cpp:84-86`, `:241-253`) |
| Internal relocations | count in `h[43]`, record at `p.Entry(15, i)` | Target is a segment base (`cro_loader.cpp:298-303`) |

Supported relocation types are listed in `docs/cro-modules.md:37-47`.

The important property: **the bytes a module runs with are not the bytes in the file.**
Relocations write the load address and resolved import addresses into code and data.
The same CRO file produces different final bytes at a different base or with a
different provider at a given import.

## Options

**A. Translate at build time from known module files, pinned to a load address.**
The build reads the CRO, applies the same relocations the runtime applies, and translates
the result at the pinned base. Output is registered like main-image blocks.

- Pros: fits the AOT design. No translator at runtime, no JIT, no interpreter. Fails
  closed when addresses or bytes differ.
- Cons: needs the exact load address, load order and import bindings in advance. Needs
  a manifest. Needs the relocation logic on the host side (see Required changes).

**B. Translate at runtime on the console.** Run the translator (or a copy) when a
module loads.

- Pros: works for any address and any module, with no manifest.
- Cons: the translator is a host tool that emits C++ source. Running it on the console
  means compiling generated code on device, or building a second execution path. This
  contradicts the stated no-JIT stance (`docs/maintainer-guide.md:8`,
  `docs/cro-modules.md:76-79`). **Not recommended.**

**C. Interpreter fallback for CRO code.** Execute guest instructions directly when no
block exists.

- Pros: simplest way to run arbitrary code.
- Cons: adds a second execution engine with its own correctness and timing story. The
  engine comments and docs say it is a fail-closed stop, not a fallback
  (`wiiu_engine.hpp:44-47`, `docs/maintainer-guide.md:8`). **Reject unless the
  maintainer changes that rule.**

**D. Position-independent translation.** Translate once from the unrelocated image.
Emit branch targets and data reads relative to a base register or runtime variable.
Read relocated data from memory at runtime instead of folding it.

- Pros: one translation for any load address.
- Cons: the largest translator change. Needs runtime-variable branch targets and no
  constant folding of relocated words. Import-dependent targets still need a stable
  binding. Whether the translator folds literal loads was **not verified**
  (`translator/src/mem_translator.cpp` was not read). Defer until A proves insufficient.

## Recommendation

Use **A now**, with these constraints:

1. **Relocation happens once, on the host, in shared code.** Move the parsing and
   relocation rules out of `WiiUMemory` into a pure function in a shared source file.
   The translator and the runtime loader both call it. Then translator input equals
   runtime memory by construction. Keep I/O at the edges.
2. **Manifest per module.** Pins: module name, raw-file digest, load address, import
   bindings (name to address, or module and index), and the list of roots (exports
   translated). The runtime checks these against what the loader actually resolved,
   before the policy returns `true`.
3. **Stronger policy.** Replace `HasBlockInRange` with a check that every executable
   segment is fully covered by blocks of this module, and that the module identity in
   the manifest matches. Blocks are not removed on unload, so reloading a different
   module at the same address must be rejected.
4. **Wire the policy** in `HLEServiceManager` or `main.cpp`, outside the diagnostics
   `#ifdef`. Until this happens, loading any CRO with code fails closed at dispatch.

**Trade-offs of the recommendation.** Only games that load known modules at known
addresses work. If a title picks addresses at random, A fails for it. D removes this
limit but is much more work. We should decide after a sample of real module loads is
available (see Open questions).

## Required runtime changes

- `wiiu_runtime/src/cro_loader.cpp`: split pure relocation out of `LoadModule`, so
  the translator can call it. Keep `WiiUMemory` writes at the edge.
- `wiiu_runtime/include/cro_loader.hpp`: the policy context should carry the manifest
  entry, not just the module. Keep `CodePolicy` signature stable if possible.
- `wiiu_runtime/include/wiiu_engine.hpp:28`, `wiiu_engine.cpp:29`: add a full-coverage
  query (or record which blocks belong to which module). `HasBlockInRange` stays for
  existing tests.
- `wiiu_runtime/src/main.cpp:86-90` and `wiiu_runtime/src/hle_services.cpp:79-87`:
  install the policy.
- A digest function for the manifest. None was verified to exist in the runtime or
  translator. It must be added or found.
- Update `docs/svc-runtime.md:117-135` and `docs/cro-modules.md` once behaviour changes.

## Required translator changes

- `translator/include/binary_loader.hpp:26-37`: new input path. A CRO plus manifest,
  not only `.code` plus ExHeader. Keep the current path unchanged.
- Multiple roots. `DiscoverCode` takes one entry (`translator/src/code_discovery.cpp:379-381`)
  and `ResolveEntryBlock` requires exactly one (`translator/src/cpp_emitter.cpp:46-59`).
  CRO needs one root per exported function. Whether the existing discovery can take a
  list without change was **not verified**.
- Emitter naming. Generated names must not collide with the main image units
  (`translator/src/unit_publisher.cpp` was not read). **Unverified.**
- Literal pools in CRO rodata/data are outside `.text`. They must read from the
  relocated image supplied by the shared loader.
- Refuse output when a root decodes into a region that a relocation changed after
  translation would make inconsistent. Concretely: fail if the manifest digest does
  not match the input bytes.

## Test plan (synthetic fixtures only)

Fixtures are generated in code, in the style of `Image()` at `wiiu_runtime/tests/cro_tests.cpp:28`.
No commercial module is committed.

1. **Shared relocation equivalence.** For each supported relocation type, the
   translator's relocated bytes equal `CROLoader` memory bytes for the same input and
   base. Fails if the two paths drift.
2. **Translated block execution.** A small ARM and Thumb module with a `BL` to an
   import and an `ABS32` in data. Translate, compile on host, run the translated block
   through the existing host engine mock (`translator/tests/support/wiiu_engine.hpp`,
   contents not read in detail). Assert r0 and memory.
3. **Negative manifest cases.** Wrong base, wrong import binding, wrong digest, and a
   missing export. Each must be rejected by the translator. The runtime must reject the
   load with `CRO module code is not translated` and leave memory unchanged.
4. **Partial coverage.** Blocks cover only the first of two code segments. Policy must
   return false.
5. **Reload identity.** Load module X at address A, unload, load module Y at A. Policy
   must reject Y if its manifest does not match.
6. **Diagnostics.** With `DS2U_MISSING_BLOCK_DETAIL`, a missing block inside a CRO
   reports `CroModuleMiss` (`wiiu_runtime/include/dispatch_diagnostics.hpp:15`).

Host commands follow the existing pattern in `docs/cro-modules.md:89-91`. They were
**not run** for this document.

## Open questions (maintainer decision)

1. Do target titles load their CRO modules at fixed addresses in a fixed order? No
   sample set is in the repository, so this cannot be checked here. Needed before
   committing to A.
2. Is A acceptable, or must D (position-independent) be the target?
3. Where does the manifest live, and who generates it? Shipping translated code derived
   from a commercial module may have licensing implications. **Not assessed.**
4. Are CRO init/exit calls and CRS extraction in scope? `docs/cro-modules.md:78-79`
   excludes them.
5. Is `ro:MOVE`/`ro:1` IPC needed, or are the HLE helpers enough
   (`docs/cro-modules.md:66-67`)?

## Not verified

- No build, test or translator run was done for this document. Only reading and grep.
- CRO header word meanings `h[]` are taken from loader code, not the 3dbrew reference.
- The hash fields at file offset `0x00`-`0x7F` and what they cover.
- Whether `translator/src/mem_translator.cpp` folds literal loads to constants.
- Whether the discovery code can take multiple roots without change.
- Whether `translator/src/unit_publisher.cpp` naming can collide with CRO units.
- Whether a SHA-256 implementation exists in either side.
- The CRO format and the policy wiring were checked against the code at HEAD of
  upstream `main` (`c84ffc1`); PR #17 (`b03dde5`) is in that history.
