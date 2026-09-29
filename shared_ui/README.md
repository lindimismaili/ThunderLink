# Shared control UI

This package builds ThunderLink's local control assets from Lit, TypeScript, and
Dark Studio CSS. Native applications remain responsible for connection, settings,
pairing, topology, permissions, and input. The browser preview has no native
bridge. The fixture host used by Playwright lives only in `test/` and is excluded
from `dist/` and the native resource maps.

Use Node 24 and npm:

```text
npm ci
npm run check
npm test
npx playwright install chromium webkit
npm run test:e2e
npm run build
```

`npm run build` produces a deterministic asset manifest, enforces the 2 MiB
budget, and copies the same assets into the SwiftPM resource package. It also
generates the Windows resource map. Run it before a raw native build. Native
build wrappers prepare it automatically. The `deploy.sh` and `deploy.ps1`
commands `build-preview`, `preview`, `clear-build`, and `reset` use the port in
`.vscode/preview-ports.json`.

The manifest build ID is the SHA-256 of sorted relative paths and content hashes.
The frontend loads that ID from its local manifest before its `ready` message.
The host must compare it with its embedded copy and supply a fresh UI instance ID.
Until both native hosts are connected, the existing dashboards remain in use.
