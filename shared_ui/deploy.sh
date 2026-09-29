#!/bin/sh
set -eu
root=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$root/.." && pwd)
cd "$root"
action=${1:-}
case "$action" in build|build-preview|preview|clear-build|reset) ;; *) echo '❌ Use: build | build-preview | preview | clear-build | reset' >&2; exit 1;; esac
port=$(node -e 'const p=require(process.argv[1]);const v=require(p).shared_ui;if(!Number.isInteger(v)||v<1024||v>65535)process.exit(1);process.stdout.write(String(v))' "$repo/.vscode/preview-ports.json") || { echo '❌ Invalid preview port' >&2; exit 1; }
safe_remove() {
  for relative in "$@"; do
    target="$root/$relative"
    case "$relative" in ''|.|..|/*|*../*|*/../*|*'/./'*) echo "❌ Unsafe cleanup target: $relative" >&2; exit 1;; esac
    node -e 'const fs=require("fs"),path=require("path");const root=fs.realpathSync(process.argv[1]),target=path.resolve(root,process.argv[2]);if(!target.startsWith(root+path.sep))process.exit(1);for(let p=target;p!==root;p=path.dirname(p))if(fs.existsSync(p)&&fs.lstatSync(p).isSymbolicLink())process.exit(1)' "$root" "$relative" || { echo "❌ Refusing unsafe or linked cleanup target: $relative" >&2; exit 1; }
    [ ! -e "$target" ] || rm -rf -- "$target"
  done
}
clear_generated() { safe_remove dist test-results playwright-report src/generated-validator.js native/Sources/ThunderLinkUIResources/Resources native/windows/generated; }
ensure_deps() {
  echo '⏳ Checking local UI dependencies'
  command -v node >/dev/null && command -v npm >/dev/null || { echo '❌ Node 24 and npm are required' >&2; exit 1; }
  case "$(node --version)" in v24.*) ;; *) echo '❌ Node 24 is required' >&2; exit 1;; esac
  fingerprint=$(node -e 'const fs=require("fs"),crypto=require("crypto");const h=crypto.createHash("sha256");for(const p of ["package.json","package-lock.json"])h.update(fs.readFileSync(p));process.stdout.write(h.digest("hex"))')-$(node --version)-$(npm --version)
  stamp=node_modules/.thunderlink-install-stamp
  if [ ! -f "$stamp" ] || [ "$(cat "$stamp")" != "$fingerprint" ] || ! npm ls --depth=0 --silent >/dev/null 2>&1; then
    echo '⏳ Installing locked UI dependencies'
    npm ci || { echo '❌ npm ci failed' >&2; exit 1; }
    printf '%s' "$fingerprint" > "$stamp"
  fi
}
start_preview() {
  exec node scripts/preview-manager.mjs "$port"
}
case "$action" in
  build) ensure_deps; echo '🔨 Building shared UI'; npm run build || { echo '❌ UI build failed' >&2; exit 1; }; echo '✅ Shared UI ready' ;;
  build-preview) ensure_deps; echo '🔨 Building shared UI'; npm run build || { echo '❌ UI build failed' >&2; exit 1; }; start_preview ;;
  preview) ensure_deps; [ -f dist/manifest.json ] || { echo '❌ No built UI. Run Build & Preview first.' >&2; exit 1; }; start_preview ;;
  clear-build) echo '🧹 Clearing generated UI output'; clear_generated; echo '✅ Cleared generated UI output' ;;
  reset) echo '♻️ Resetting local UI dependencies and output'; clear_generated; safe_remove node_modules; echo '✅ Reset complete' ;;
esac
