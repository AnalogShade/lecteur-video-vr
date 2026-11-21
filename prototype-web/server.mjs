import { spawn } from 'node:child_process';
import { existsSync } from 'node:fs';
import { readFile } from 'node:fs/promises';
import { createServer } from 'node:http';
import { join } from 'node:path';

const types = new Map([
  ['index.html', 'text/html; charset=utf-8'],
  ['app.js', 'text/javascript; charset=utf-8'],
  ['styles.css', 'text/css; charset=utf-8'],
]);
const assets = new Map();
for (const [name, type] of types) {
  try {
    assets.set(`/${name}`, { type, body: await readFile(new URL(`./dist/${name}`, import.meta.url)) });
  } catch (error) {
    if (error.code !== 'ENOENT') throw error;
    console.error('Le lecteur doit être compilé avec npm run build.');
    process.exit(1);
  }
}
assets.set('/', assets.get('/index.html'));

const host = '127.0.0.1';
const port = Number(process.env.HORIZON_PORT ?? 4173);
const server = createServer((request, response) => {
  if (request.method !== 'GET' && request.method !== 'HEAD') {
    response.writeHead(405, { Allow: 'GET, HEAD' }).end();
    return;
  }
  const asset = assets.get(request.url?.split('?')[0]);
  if (!asset) {
    response.writeHead(404).end();
    return;
  }
  response.writeHead(200, {
    'Content-Type': asset.type,
    'Content-Length': asset.body.byteLength,
    'Cache-Control': 'no-cache',
    'X-Content-Type-Options': 'nosniff',
  });
  response.end(request.method === 'HEAD' ? undefined : asset.body);
});

server.on('error', error => {
  if (error.code !== 'EADDRINUSE') throw error;
  console.error(`Le port ${port} est déjà utilisé. Si le lecteur est ouvert, utilise la fenêtre de lancement existante.`);
  process.exitCode = 1;
});

server.listen(port, host, () => {
  const url = `http://${host}:${server.address().port}/`;
  console.log(`Lecteur vidéo VR est prêt sur ${url}\nGarde cette fenêtre ouverte pendant la lecture.`);
  if (!process.argv.includes('--open') || process.platform !== 'win32') return;

  const locations = [process.env.ProgramFiles, process.env['ProgramFiles(x86)'], process.env.LOCALAPPDATA].filter(Boolean);
  const candidates = ['Google/Chrome/Application/chrome.exe', 'Microsoft/Edge/Application/msedge.exe']
    .flatMap(browser => locations.map(location => join(location, browser)));
  const browser = candidates.find(existsSync);
  if (!browser) {
    console.log('Ouvre cette adresse dans Chrome ou Edge.');
    return;
  }
  const child = spawn(browser, [url], { detached: true, stdio: 'ignore' });
  child.on('error', () => console.log('Ouvre cette adresse dans Chrome ou Edge.'));
  child.unref();
});
