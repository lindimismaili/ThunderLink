#!/bin/sh
# macOS half of the ThunderLink VS Code "Build" and "Run" tasks.
#
#   build : verify the Swift toolchain (offering to launch the Command Line
#           Tools installer when it is missing), then build the sender.
#   run   : build only when a source file is newer than the binary, then launch
#           the sender in this terminal.
#
# The Windows receiver is driven by the sibling scripts/dev.ps1.
set -eu

task=build
if [ $# -gt 0 ]; then
    task=$1
    shift
fi

repo=$(cd "$(dirname "$0")/.." && pwd)
package="$repo/mac_sender"
binary="$package/.build/debug/ThunderLinkSender"

step() { printf '\033[36m==> %s\033[0m\n' "$1"; }
note() { printf '\033[90m    %s\033[0m\n' "$1"; }
fail() { printf '%s\n' "$1" >&2; exit 1; }

confirm() {
    if [ "${TLNK_AUTO_INSTALL:-}" = 1 ]; then
        return 0
    fi
    if [ ! -t 0 ]; then
        return 1
    fi
    printf '%s [y/N] ' "$1"
    read -r reply || return 1
    case "$reply" in
        y|Y|yes|Yes|YES) return 0 ;;
        *) return 1 ;;
    esac
}

[ "$(uname -s)" = Darwin ] || \
    fail 'The ThunderLink sender builds and runs only on macOS. Use scripts/dev.ps1 on Windows.'

# Swift 5.9+ ships with Xcode or the standalone Command Line Tools. Both are
# GUI/licence-gated installs, so this offers the installer rather than forcing one.
ensure_toolchain() {
    step 'Checking the macOS build toolchain'

    if ! xcode-select -p >/dev/null 2>&1 || ! command -v swift >/dev/null 2>&1; then
        printf '\033[33m%s\033[0m\n' 'The Swift toolchain was not found on this Mac.'
        note 'ThunderLink needs Xcode or the Xcode Command Line Tools with Swift 5.9 or newer.'
        if confirm 'Start the macOS Command Line Tools installer now?'; then
            xcode-select --install || true
            fail 'Finish the installer, then run this task again.'
        fi
        fail 'Install them with: xcode-select --install'
    fi

    version=$(swift --version 2>/dev/null | sed -n 's/.*[Ss]wift version \([0-9][0-9.]*\).*/\1/p' | head -n 1)
    if [ -n "$version" ]; then
        major=${version%%.*}
        minor=0
        case "$version" in
            *.*) rest=${version#*.}; minor=${rest%%.*} ;;
        esac
        if [ "$major" -lt 5 ] || { [ "$major" -eq 5 ] && [ "$minor" -lt 9 ]; }; then
            fail "Swift $version is too old; ThunderLink needs Swift 5.9 or newer. Update Xcode or the Command Line Tools."
        fi
        note "swift  $version ($(xcode-select -p))"
    else
        note "swift  $(command -v swift) (version string not recognised; continuing)"
    fi
}

build_sender() {
    ensure_toolchain
    step 'Building the ThunderLink sender (debug)'
    # Plain `swift build` covers both executables: the sender and the power helper
    # it launches for closed-lid sessions.
    swift build --package-path "$package"
    printf '\033[32m%s\033[0m\n' "Built $binary"
}

# Cheap staleness check so an unchanged tree launches without invoking SwiftPM.
is_up_to_date() {
    [ -x "$binary" ] || return 1
    newer=$(find "$package/Sources" "$package/PowerHelper" "$package/PowerSupport" \
        "$package/Package.swift" -newer "$binary" -print 2>/dev/null | head -n 1 || true)
    [ -z "$newer" ]
}

run_sender() {
    if is_up_to_date; then
        step 'Build is already current; skipping compilation'
    else
        build_sender
    fi

    step 'Starting the ThunderLink sender'
    note 'macOS may ask for Screen Recording and Accessibility access; re-run this task after granting it.'
    exec "$binary" "$@"
}

case "$task" in
    build) build_sender ;;
    run)   run_sender "$@" ;;
    *)     fail "Unknown task '$task'. Use: build | run" ;;
esac
