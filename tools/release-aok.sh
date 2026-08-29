#!/bin/sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
project="$repo_root/iSH-AOK.xcodeproj"
scheme="iSH-AOK"
configuration="Release"
export_options="$repo_root/AppStoreExportOptions.plist"
brew_ruby33_bin="/opt/homebrew/opt/ruby@3.3/bin"
brew_ruby_bin="/opt/homebrew/opt/ruby/bin"

ruby_cmd="${RUBY_CMD:-ruby}"
bundle_cmd="${BUNDLE_CMD:-bundle}"
if [ -x "$brew_ruby33_bin/ruby" ]; then
    ruby_cmd="$brew_ruby33_bin/ruby"
elif [ -x "$brew_ruby_bin/ruby" ]; then
    ruby_cmd="$brew_ruby_bin/ruby"
fi
if [ -x "$brew_ruby33_bin/bundle" ]; then
    bundle_cmd="$brew_ruby33_bin/bundle"
elif [ -x "$brew_ruby_bin/bundle" ]; then
    bundle_cmd="$brew_ruby_bin/bundle"
fi

usage() {
    cat >&2 <<EOF
usage: $0 <command> [args]

Commands:
  preflight
      Check local release prerequisites.

  archive [archive_path]
      Build an .xcarchive. If archive_path is omitted, a timestamped path
      under ~/Library/Developer/Xcode/Archives is used.

  export <archive_path|latest> <export_path>
      Export IPA using tools/export-ios-archive.sh and AppStoreExportOptions.plist.

  export-debug <archive_path|latest> <export_path>
      Export a development/debugging IPA using automatic signing.

  upload-fastlane
      Run Fastlane lane upload_build (requires Ruby/Bundler/Fastlane setup).

Examples:
  $0 preflight
  $0 archive
  $0 export latest /tmp/iSH-AOK-export
  $0 upload-fastlane
EOF
    exit 2
}

note() {
    printf '%s\n' "$*"
}

fail() {
    printf 'error: %s\n' "$*" >&2
    exit 1
}

require_cmd() {
    command -v "$1" >/dev/null 2>&1 || fail "missing required command: $1"
}

ruby_version_ge_3_2() {
    "$ruby_cmd" -e 'exit((RUBY_VERSION.split(".").map(&:to_i) <=> [3,2,0]) >= 0 ? 0 : 1)' >/dev/null 2>&1
}

is_clean_tree() {
    [ -z "$(git -C "$repo_root" status --porcelain=v1)" ]
}

preflight() {
    require_cmd git
    require_cmd xcodebuild

    note "== iSH-AOK Release Preflight =="
    note "repo: $repo_root"
    note "project: $project"
    note "scheme: $scheme"
    note "configuration: $configuration"

    if is_clean_tree; then
        note "[ok] git tree is clean"
    else
        note "[warn] git tree has uncommitted or untracked changes"
        git -C "$repo_root" status --short
    fi

    if [ -f "$export_options" ]; then
        note "[ok] export options found: $export_options"
    else
        fail "missing export options plist: $export_options"
    fi

    note "[ok] Xcode: $(xcodebuild -version | tr '\n' ' ' | sed 's/  */ /g')"

    if [ -x "$ruby_cmd" ] || command -v "$ruby_cmd" >/dev/null 2>&1; then
        if ruby_version_ge_3_2; then
            note "[ok] Ruby version is compatible for current fastlane lockfile"
        else
            note "[warn] Ruby is too old for current fastlane dependencies"
            note "       Current: $("$ruby_cmd" -v | awk '{print $2}') ; Needed: >= 3.2"
            note "       Suggested fix:"
            note "         brew install ruby"
            note "         Script auto-prefers /opt/homebrew/opt/ruby@3.3/bin when present."
        fi
    else
        note "[warn] ruby command not found"
    fi

    if [ -x "$bundle_cmd" ] || command -v "$bundle_cmd" >/dev/null 2>&1; then
        if (cd "$repo_root" && "$bundle_cmd" exec fastlane --version >/dev/null 2>&1); then
            note "[ok] fastlane available through bundle exec"
        else
            note "[warn] bundle exists, but fastlane is not runnable via bundle exec"
            note "       If you want automated TestFlight upload, fix Bundler/Fastlane first."
        fi
    else
        note "[warn] bundle command not found"
        note "       Archive/export can still work. Fastlane upload will not."
    fi
}

archive_default_path() {
    date_dir=$(date +%Y-%m-%d)
    stamp=$(date +%Y%m%d-%H%M%S)
    printf '%s/Library/Developer/Xcode/Archives/%s/iSH-AOK-%s.xcarchive\n' "$HOME" "$date_dir" "$stamp"
}

archive_build() {
    require_cmd xcodebuild
    out="${1:-$(archive_default_path)}"
    note "Archiving to: $out"
    xcodebuild \
        -project "$project" \
        -scheme "$scheme" \
        -configuration "$configuration" \
        -archivePath "$out" \
        archive
    note "Archive complete: $out"
}

export_ipa() {
    [ $# -eq 2 ] || usage
    archive_path=$1
    export_path=$2
    "$repo_root/tools/export-ios-archive.sh" "$archive_path" "$export_path" "$export_options"
    note "Export complete: $export_path"
}

export_debug_ipa() {
    [ $# -eq 2 ] || usage
    archive_path=$1
    export_path=$2
    dev_opts="${TMPDIR:-/tmp}/iSH-AOK-DevExportOptions.plist"
    cat > "$dev_opts" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>destination</key><string>export</string>
  <key>method</key><string>debugging</string>
  <key>signingStyle</key><string>automatic</string>
  <key>stripSwiftSymbols</key><true/>
  <key>teamID</key><string>UYU5FM4LQ4</string>
</dict>
</plist>
EOF
    "$repo_root/tools/export-ios-archive.sh" "$archive_path" "$export_path" "$dev_opts"
    note "Debug/development export complete: $export_path"
}

upload_fastlane() {
    if ! ([ -x "$bundle_cmd" ] || command -v "$bundle_cmd" >/dev/null 2>&1); then
        fail "missing bundle command: $bundle_cmd"
    fi
    (cd "$repo_root" && "$bundle_cmd" exec fastlane upload_build)
}

[ $# -ge 1 ] || usage
cmd=$1
shift

case "$cmd" in
    preflight) preflight "$@" ;;
    archive) archive_build "$@" ;;
    export) export_ipa "$@" ;;
    export-debug) export_debug_ipa "$@" ;;
    upload-fastlane) upload_fastlane "$@" ;;
    *) usage ;;
esac
