# Release Notes Since `builds/iSH-AOK_526`

These notes summarize changes from `builds/iSH-AOK_526` intended for `builds/iSH-AOK_527`.

## Highlights

- Fixed two real crash paths in core runtime code:
  - `fcntl` record-lock crash on non-seekable FDs (`file_lock_from_flock` null `lseek` call).
  - JIT store-path hardening around stale TLB/MMU mapping transitions.
- Hardened guest string handling to reject non-NUL-terminated user strings within buffer bounds.
- Added incremental release automation helper for iSH-AOK with practical preflight checks and IPA export commands.
- Added a one-command debug/development IPA export path for local distribution without App Store distribution signing.
- Improved release helper Ruby auto-detection to use Homebrew Ruby/Bundler, preferring `ruby@3.3`, without requiring PATH edits.

## User-Facing Changes

### Stability and Crash Fixes

- Fixed a crash when POSIX file locking (`fcntl` setlk/getlk with `LSEEK_CUR`) was attempted on file descriptors without an `lseek` operation; now returns `_ESPIPE` instead of crashing.
- Hardened JIT memory write fast paths by validating `tlb->mem_changes` against `mmu->changes` before using cached TLB pointer mappings.
- Hardened `user_read_string` so it fails when no terminating NUL is found within the provided maximum length, preventing unsafe downstream `strlen` behavior.

### Release and Distribution Workflow

- Added `tools/release-aok.sh` as an incremental release helper with:
  - `preflight`
  - `archive`
  - `export` (App Store export options)
  - `upload-fastlane`
- Added `export-debug` mode for dev/debugging IPA export using automatic signing:
  - `./tools/release-aok.sh export-debug latest /path/to/export`
- Updated helper behavior to auto-detect explicit Ruby/Bundler executables:
  - preferring `/opt/homebrew/opt/ruby@3.3/bin`
  - then `/opt/homebrew/opt/ruby/bin`
  - then shell defaults.

## Maintainer Notes

- `builds/iSH-AOK_526` is the previous release tag.
- `builds/iSH-AOK_527` is updated to current `HEAD` after these notes.
- App Store export still requires appropriate distribution signing assets; debug IPA export path is now available for immediate local distribution.

## Commit Range

- `e6665a14` Merge selected working PR fixes
- `557c65e4` Fix crash paths in file locking, JIT store miss, and user string reads
- `4e1941c4` Add incremental iSH-AOK release automation helper
- `b8f82d9b` Improve release preflight with Ruby/Fastlane setup guidance
- `5806a3bd` Prefer Homebrew Ruby/Bundler in release helper
- `7eae0106` Prefer ruby@3.3 in release helper auto-detection
- `c9b11572` Add debug IPA export mode to release helper
