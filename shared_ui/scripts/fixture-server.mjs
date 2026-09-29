import { createServer } from 'node:http';
import { readFile } from 'node:fs/promises';
import { extname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
const root = fileURLToPath(new URL('../dist/', import.meta.url));
const test = fileURLToPath(new URL('../test/', import.meta.url));
const port = Number(process.env.TLNK_E2E_PORT ?? 41733);
const types = { '.html': 'text/html; charset=utf-8', '.js': 'text/javascript; charset=utf-8', '.css': 'text/css; charset=utf-8', '.json': 'application/json; charset=utf-8', '.woff2': 'font/woff2' };
createServer(async (request, response) => {
  const pathname = new URL(request.url, `http://127.0.0.1:${port}`).pathname;
  const known = new Set(['/', '/index.html', '/manifest.json', '/ui.js', '/ui.css', '/fixture.html', '/fixture-host.js', '/fixture-flag.js', '/fixture-ui']);
  if (!known.has(pathname) && !/^\/fonts\/inter-latin-(400|500|600|700)-normal\.woff2$/.test(pathname)) { response.writeHead(404); response.end(); return; }
  const fixture = pathname.startsWith('/fixture') && pathname !== '/fixture-ui';
  const filename = pathname === '/' || pathname === '/fixture-ui' ? 'index.html' : pathname.slice(1);
  try {
    let body = await readFile(join(fixture ? test : root, filename));
    if (pathname === '/fixture-ui') body = Buffer.from(String(body).replace('<script src="./ui.js"', '<script src="/fixture-flag.js"></script><script src="./ui.js"'));
    response.writeHead(200, { 'Content-Type': types[extname(filename)] ?? 'application/octet-stream', 'Cache-Control': 'no-store', 'X-Content-Type-Options': 'nosniff' });
    response.end(body);
  } catch { response.writeHead(404); response.end(); }
}).listen(port, '127.0.0.1', () => console.log(`Fixture host http://127.0.0.1:${port}/fixture.html`));
