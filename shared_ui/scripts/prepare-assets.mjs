import { createHash } from 'node:crypto';
import { readFile, readdir, writeFile, mkdir, copyFile, rm, cp } from 'node:fs/promises';
import { join, relative } from 'node:path';
import { fileURLToPath } from 'node:url';

const root = new URL('../dist/', import.meta.url);
const rootPath = fileURLToPath(root);
const html = `<!doctype html><html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><meta http-equiv="Content-Security-Policy" content="default-src 'none'; script-src 'self'; style-src 'self'; font-src 'self'; img-src 'self' data:; connect-src 'self'; object-src 'none'; base-uri 'none'; form-action 'none'"><title>ThunderLink</title><link rel="stylesheet" href="./ui.css"></head><body><thunderlink-app></thunderlink-app><script src="./ui.js" defer></script></body></html>`;
await writeFile(new URL('index.html', root), html);
await mkdir(new URL('fonts/', root), { recursive: true });
let fontCss = '';
for (const weight of [400, 500, 600, 700]) {
  const name = `inter-latin-${weight}-normal.woff2`;
  await copyFile(new URL(`../node_modules/@fontsource/inter/files/${name}`, import.meta.url), new URL(`fonts/${name}`, root));
  fontCss += `@font-face{font-family:Inter;font-style:normal;font-weight:${weight};font-display:swap;src:url('./fonts/${name}') format('woff2')}\n`;
}
const cssPath = new URL('ui.css', root);
await writeFile(cssPath, fontCss + await readFile(cssPath, 'utf8'));
await copyFile(new URL('../licenses/Inter-OFL.txt', import.meta.url), new URL('Inter-OFL.txt', root));
await copyFile(new URL('../licenses/Lit-BSD-3-Clause.txt', import.meta.url), new URL('Lit-BSD-3-Clause.txt', root));
async function files(dir) {
  const entries = await readdir(dir, { withFileTypes: true });
  const output = [];
  for (const entry of entries) {
    const path = join(dir, entry.name);
    if (entry.isDirectory()) output.push(...await files(path));
    else if (entry.isFile() && !['manifest.json'].includes(entry.name)) output.push(path);
  }
  return output;
}
const paths = (await files(rootPath)).sort();
const assets = [];
for (const path of paths) {
  const data = await readFile(path);
  assets.push({ path: relative(rootPath, path).replaceAll('\\', '/'), sha256: createHash('sha256').update(data).digest('hex'), bytes: data.length });
}
const bytes = assets.reduce((sum, file) => sum + file.bytes, 0);
if (bytes > 2 * 1024 * 1024) throw new Error(`Shared UI assets exceed 2 MiB: ${bytes} bytes`);
const buildId = createHash('sha256').update(assets.map(a => `${a.path}\0${a.sha256}`).join('\n')).digest('hex');
await writeFile(new URL('manifest.json', root), JSON.stringify({ contractVersion: 1, buildId, assets }, null, 2) + '\n');
const resourceDir = new URL('../native/Sources/ThunderLinkUIResources/Resources/', import.meta.url);
await rm(resourceDir, { recursive: true, force: true });
await cp(root, resourceDir, { recursive: true });
const windowsDir = new URL('../native/windows/generated/', import.meta.url);
await mkdir(windowsDir, { recursive: true });
const mime = path => path.endsWith('.html') ? 'text/html' : path.endsWith('.css') ? 'text/css' : path.endsWith('.js') ? 'text/javascript' : path.endsWith('.json') ? 'application/json' : path.endsWith('.woff2') ? 'font/woff2' : 'text/plain';
const rc = assets.map((asset, index) => `${1000 + index} RCDATA "${join(rootPath, asset.path).replaceAll('\\', '/')}"`).join('\n') + `\n${1000 + assets.length} RCDATA "${join(rootPath, 'manifest.json').replaceAll('\\', '/')}"\n`;
await writeFile(new URL('UIAssets.rc', windowsDir), rc);
const entries = [...assets, { path: 'manifest.json', sha256: '', bytes: 0 }];
const header = `#pragma once\n#include <cstddef>\nnamespace thunderlink::ui {\nstruct Asset { const char* path; int resourceId; const char* mime; const char* sha256; };\ninline constexpr int kContractVersion = 1;\ninline constexpr const char* kBuildId = "${buildId}";\ninline constexpr Asset kAssets[] = {\n${entries.map((asset, index) => `    {"${asset.path}", ${1000 + index}, "${mime(asset.path)}", "${asset.sha256}"},`).join('\n')}\n};\ninline constexpr size_t kAssetCount = sizeof(kAssets)/sizeof(kAssets[0]);\n}\n`;
await writeFile(new URL('UIAssets.h', windowsDir), header);
console.log(`Shared UI ${buildId.slice(0, 12)}: ${assets.length} assets, ${bytes} bytes`);
