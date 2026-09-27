# Release Notes Since `builds/iSH-AOK_525`

These notes summarize changes from `builds/iSH-AOK_525` intended for `builds/iSH-AOK_526`.

## Highlights

- Expanded experimental amd64/x86_64 guest bring-up with broader interpreter and JIT opcode coverage.
- Improved amd64 fallback behavior so unsupported or unsafe JIT cases continue through the interpreter.
- Added LLM workspace integration plumbing with Settings gating and persistent workspace data paths.
- Fixed Workspace scene discovery so existing Workspace windows, including the first/default workspace, remain reachable from the Workspaces window.
- Improved Docker/rootfs boot behavior and local amd64 regression support.

## User-Facing Changes

### Experimental amd64 Support

- Added amd64 baseline emulation, flag opcode support, and additional opcode coverage for JIT lowering.
- Improved amd64 boot fallback behavior and interpreter/JIT handoff safety.
- Added JIT coverage for register moves, bit shifts, `bswap`, control-flow edge cases, and selected test-oriented lowering paths.
- Kept unsupported or unsafe amd64 instructions on safe fallback paths instead of emitting unverified direct lowering.

### App and Workspace

- Added optional LLM workspace integration and persisted related data under the AOK persistence path.
- Added workspace controls for amd64 JIT behavior.
- Fixed Workspaces window scene enumeration to include live connected scenes as well as open sessions.
- Fixed Workspace role inference for live scenes whose restoration metadata is missing or not yet populated.
- Fixed focusing and closing Workspace scene entries discovered through the merged scene/session list.
- Improved accessibility labels for visual update badges on Settings buttons.

## Testing and Developer Support

- Improved Docker rootfs boot behavior and amd64 baseline test coverage.
- Added amd64 gas probing and expanded amd64 JIT smoke/regression coverage.
- Verified the final Workspace fix with `git diff --check` and an iOS Simulator Debug build.

## Known Notes

- amd64/x86_64 guest support remains experimental.
- The amd64 JIT remains guarded by Settings, `/proc/ish/amd64_jit`, and standalone `ISH_HOST_AMD64_JIT` controls.
- Direct amd64 conditional branch lowering remains deferred until flag-state-safe condition evaluation is proven.
- Apple Foundation Models integration remains blocked in this tree because the installed SDK does not include `FoundationModels.framework`.

## Maintainer Notes

- `builds/iSH-AOK_525` points to commit `5778c072`.
- `builds/iSH-AOK_526` tags the Workspace scene discovery fix and release notes after the range below.

## Commit Range

- `45471e92a` Improve Docker rootfs boot and amd64 baseline emulation
- `cd976ec3b` Improve amd64 boot fallback and emulation
- `0083a135d` Add amd64 flag opcode support and gas probe
- `f838510be` Hoist strlen in sys_getdents_common
- `42ff7fa0c` Expose visual update badges on settings buttons to screen readers
- `d3a557271` Fuse JIT register moves
- `ada915a48` Use shared cache base for hook vm_protect
- `f8d9849be` Fill amd64 JIT opcode gaps
- `ff0bc3137` Cover more amd64 JIT bit shifts
- `e85c363cf` Add amd64 JIT bswap helper
- `a301dabd5` Improve AOK persistence and amd64 JIT lowering
- `600b5ef94` Add LLM workspace integration and amd64 test lowering
- `fc4bc5114` Improve amd64 JIT flow and workspace controls
- `93872832a` Fix amd64 JIT control-flow edge cases
- `bc30cc6b7` Fix workspace scene discovery
