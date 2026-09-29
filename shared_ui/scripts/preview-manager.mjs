import { spawn } from 'node:child_process';
import { randomBytes } from 'node:crypto';
import { createServer } from 'node:http';
import { mkdir, readFile, rm, writeFile } from 'node:fs/promises';
import { resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const root = fileURLToPath(new URL('../', import.meta.url));
const ownerFile = new URL('../.tmp/preview-owner.json', import.meta.url);
const port = Number(process.argv[2]);
if (!Number.isInteger(port) || port < 1024 || port > 65535) throw new Error('Invalid preview port');
await mkdir(new URL('../.tmp/', import.meta.url), { recursive: true });

async function stopPrevious() {
  let previous;
  try { previous = JSON.parse(await readFile(ownerFile, 'utf8')); } catch { return; }
  if (previous.root !== resolve(root) || previous.previewPort !== port || !Number.isInteger(previous.controlPort) || typeof previous.token !== 'string') return;
  try { process.kill(previous.pid, 0); } catch { await rm(ownerFile, { force: true }); return; }
  try {
    const response = await fetch(`http://127.0.0.1:${previous.controlPort}/stop`, { method: 'POST', headers: { 'X-ThunderLink-Token': previous.token }, signal: AbortSignal.timeout(3000) });
    if (!response.ok) throw new Error('Previous preview refused stop');
  } catch { throw new Error(`Could not stop the previous owned preview on port ${port}. Inspect the process before retrying.`); }
  for (let attempt = 0; attempt < 50; attempt++) {
    try { process.kill(previous.pid, 0); } catch { return; }
    await new Promise(resolve => setTimeout(resolve, 100));
  }
  throw new Error(`Previous preview on port ${port} did not exit.`);
}
await stopPrevious();
const token = randomBytes(32).toString('hex');
let child;
let exiting = false;
const control = createServer((request, response) => {
  if (request.method !== 'POST' || request.url !== '/stop' || request.headers['x-thunderlink-token'] !== token) { response.writeHead(403); response.end(); return; }
  response.writeHead(200); response.end('Stopping');
  shutdown(0);
});
await new Promise((resolve, reject) => { control.once('error', reject); control.listen(0, '127.0.0.1', resolve); });
const controlPort = control.address().port;
child = spawn(process.execPath, [fileURLToPath(new URL('../node_modules/vite/bin/vite.js', import.meta.url)), 'preview', '--host', '127.0.0.1', '--strictPort', '--port', String(port)], { cwd: root, stdio: 'inherit', windowsHide: true });
await writeFile(ownerFile, JSON.stringify({ root: resolve(root), pid: process.pid, childPid: child.pid, previewPort: port, controlPort, token }));
console.log(`▶️ Preview at http://127.0.0.1:${port}/`);

async function shutdown(code) {
  if (exiting) return;
  exiting = true;
  child?.kill();
  control.close();
  try {
    const recorded = JSON.parse(await readFile(ownerFile, 'utf8'));
    if (recorded.pid === process.pid && recorded.token === token) await rm(ownerFile);
  } catch { /* Already replaced or removed. */ }
  process.exit(code);
}
process.on('SIGINT', () => { void shutdown(0); });
process.on('SIGTERM', () => { void shutdown(0); });
process.on('exit', () => { child?.kill(); });
child.on('exit', code => { void shutdown(code ?? 1); });
