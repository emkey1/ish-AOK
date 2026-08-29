#!/bin/sh
# Ad-hoc sign a built .app so it carries its entitlements.
#
# The IPAs published to GitHub Releases are built with CODE_SIGNING_ALLOWED=NO
# and are meant to be re-signed with AltStore, SideStore or Sideloadly. An
# unsigned bundle has no entitlements blob at all, and the AltStore family
# decides what to request from Apple by reading the entitlements of the app it
# is re-signing: finding none, it logs "has no app groups, skipping assignment"
# and signs the app with no App Group. iOS then hands the app no shared
# container, and the first thing AOK does on a fresh install -- import its
# bundled root -- fails with "No filesystem storage available" (reported
# 2026-08-29). The app has a private-container fallback now, but it costs the
# Files integration, so it is worth the signer seeing what we actually want.
#
# An ad-hoc signature needs no certificate and no keychain, and the re-signer
# replaces it wholesale, exactly as it does for a real App Store signature. All
# this leaves behind is a readable entitlements blob.
set -eu

app=${1:?usage: adhoc-sign-app.sh <path to built .app>}
repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
project="$repo_root/iSH-AOK.xcodeproj"
configuration="${CONFIGURATION:-Release}"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

# The entitlements a target is built with, as an absolute path. Read from the
# project rather than hardcoded, because the app and the extension use
# different files and the Release ones have changed before.
entitlements_for_target() {
    target=$1
    fallback=$2
    value=$(xcodebuild -project "$project" -target "$target" -configuration "$configuration" \
                -sdk iphoneos -showBuildSettings 2>/dev/null \
            | awk -F' = ' '/ CODE_SIGN_ENTITLEMENTS = /{print $2; exit}')
    [ -n "$value" ] || value=$fallback
    case $value in
        /*) echo "$value" ;;
        *) echo "$repo_root/$value" ;;
    esac
}

# $(PRODUCT_APP_GROUP_IDENTIFIER) and friends are expanded by Xcode's packaging
# step, which does not run when code signing is off, so expand the ones our
# entitlements files use here. Substituting an empty value would silently ship
# an entitlement for the group named "", so refuse instead.
expand() {
    src=$1
    dst=$2
    sed -e "s|\$(PRODUCT_APP_GROUP_IDENTIFIER)|$app_group|g" \
        -e "s|\$(PRODUCT_BUNDLE_IDENTIFIER)|$bundle_id|g" "$src" > "$dst"
    if grep -q '\$(' "$dst"; then
        echo "adhoc-sign-app.sh: unexpanded build setting left in $src:" >&2
        grep -n '\$(' "$dst" >&2
        exit 1
    fi
}

settings=$(xcodebuild -project "$project" -target iSH-AOK -configuration "$configuration" \
               -sdk iphoneos -showBuildSettings 2>/dev/null)
app_group=$(echo "$settings" | awk -F' = ' '/ PRODUCT_APP_GROUP_IDENTIFIER = /{print $2; exit}')
bundle_id=$(echo "$settings" | awk -F' = ' '/ PRODUCT_BUNDLE_IDENTIFIER = /{print $2; exit}')
: "${app_group:?could not resolve PRODUCT_APP_GROUP_IDENTIFIER}"
: "${bundle_id:?could not resolve PRODUCT_BUNDLE_IDENTIFIER}"

app_entitlements=$(entitlements_for_target iSH-AOK app/iSH.entitlements)
appex_entitlements=$(entitlements_for_target iSH-AOK.FileProvider iSHFileProviderRelease.entitlements)
expand "$app_entitlements" "$work/app.plist"
expand "$appex_entitlements" "$work/appex.plist"

# Nested code first: the app's signature seals whatever its bundle contains.
for appex in "$app"/PlugIns/*.appex; do
    [ -e "$appex" ] || continue
    echo "ad-hoc signing $(basename "$appex")"
    codesign --force --sign - --entitlements "$work/appex.plist" --timestamp=none "$appex"
done
echo "ad-hoc signing $(basename "$app")"
codesign --force --sign - --entitlements "$work/app.plist" --timestamp=none "$app"

# The whole point of this script is the app group reaching the re-signer, so
# fail the build rather than publish another IPA that silently lacks it.
if ! codesign -d --entitlements :- "$app" 2>/dev/null | grep -q "$app_group"; then
    echo "adhoc-sign-app.sh: $app_group is missing from the signed bundle's entitlements" >&2
    exit 1
fi
echo "entitlements embedded, app group $app_group"
