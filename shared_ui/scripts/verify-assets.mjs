import { createHash } from 'node:crypto';
import { readFile } from 'node:fs/promises';
import { resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
const dist = fileURLToPath(new URL('../dist/', import.meta.url));
const swift = fileURLToPath(new URL('../native/Sources/ThunderLinkUIResources/Resources/', import.meta.url));
const manifest = JSON.parse(await readFile(new URL('../dist/manifest.json', import.meta.url), 'utf8'));
if (manifest.contractVersion !== 1 || !/^[a-f0-9]{64}$/.test(manifest.buildId)) throw new Error('Invalid UI manifest version or build ID');
let total = 0;
for (const asset of manifest.assets) {
  if (!/^[a-zA-Z0-9_./-]+$/.test(asset.path) || asset.path.split('/').includes('..')) throw new Error(`Unsafe asset path ${asset.path}`);
  const disk = await readFile(resolve(dist, asset.path));
  const copied = await readFile(resolve(swift, asset.path));
  const hash = createHash('sha256').update(disk).digest('hex');
  if (hash !== asset.sha256 || !disk.equals(copied) || disk.length !== asset.bytes) throw new Error(`Asset mismatch ${asset.path}`);
  total += disk.length;
}
if (total > 2 * 1024 * 1024) throw new Error('Shared assets exceed the 2 MiB budget');
const expected = createHash('sha256').update(manifest.assets.map(a => `${a.path}\0${a.sha256}`).join('\n')).digest('hex');
if (expected !== manifest.buildId) throw new Error('Build ID does not match asset hashes');
const windowsMap = await readFile(new URL('../native/windows/generated/UIAssets.h', import.meta.url), 'utf8');
if (!windowsMap.includes(manifest.buildId) || manifest.assets.some(asset => !windowsMap.includes(`"${asset.path}"`))) throw new Error('Windows resource map does not match manifest');
console.log(`Verified ${manifest.assets.length} identical Swift/Windows source assets (${total} bytes).`);
