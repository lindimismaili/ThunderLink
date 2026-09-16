#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
# Swift 6.4 CLT ships Testing but swiftbuild does not discover its macro plugin.
# Full Xcode and other toolchains use SwiftPM's ordinary discovery.
developer_dir=$(xcode-select -p)
plugin="$developer_dir/usr/lib/swift/host/plugins/testing/libTestingMacros.dylib"
if [ "$developer_dir" = /Library/Developer/CommandLineTools ] && [ -f "$plugin" ]; then
    exec swift test --package-path mac_sender --disable-xctest \
        -Xswiftc -load-plugin-library -Xswiftc "$plugin" "$@"
fi
exec swift test --package-path mac_sender --disable-xctest "$@"
