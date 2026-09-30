// The HTTP side: a JSON API over the DeviceManager, server-sent events, and
// the web page (built into dist/, or served by Vite with --dev).

import { existsSync, readFileSync, statSync } from 'node:fs';
import { type IncomingMessage, type Server, type ServerResponse, createServer } from 'node:http';
import { extname, join, normalize, sep } from 'node:path';
import { fileURLToPath } from 'node:url';

import type { ApiError, ServerEvent } from './api.ts';
import { BlatError } from './blat/client.ts';
import { STATUS } from './blat/wire.ts';
import type { Scanner } from './ble.ts';
import { type DeviceManager, NotConnected, NotFound } from './devices.ts';

const ROOT = fileURLToPath(new URL('..', import.meta.url));
const DIST = join(ROOT, 'dist');

export interface AppOptions {
  manager: DeviceManager;
  /** null with --sim: nothing to scan. */
  scanner: Scanner | null;
  /** Serve the page through Vite (live reload) rather than from dist/. */
  dev: boolean;
  scanSeconds?: number;
}

class HttpError extends Error {
  code: number;
  constructor(code: number, message: string) {
    super(message);
    this.code = code;
  }
}

type Handler = (req: IncomingMessage, params: string[], body: Record<string, unknown>) => Promise<unknown> | unknown;

export async function createApp(options: AppOptions): Promise<Server> {
  const { manager, scanner } = options;
  const routes: Array<[string, RegExp, Handler]> = [
    ['GET', /^\/api\/devices$/, () => manager.list()],
    [
      'POST',
      /^\/api\/scan$/,
      async (_req, _p, body) => {
        if (!scanner) throw new HttpError(400, 'Scanning needs Bluetooth; the server is running with --sim');
        await scanner.start(Number(body.seconds) || options.scanSeconds || 15);
        return { scanning: true };
      },
    ],
    ['GET', /^\/api\/devices\/([^/]+)$/, (_r, [id]) => manager.state(id)],
    ['POST', /^\/api\/devices\/([^/]+)\/connect$/, (_r, [id]) => manager.connect(id)],
    ['POST', /^\/api\/devices\/([^/]+)\/disconnect$/, (_r, [id]) => (manager.disconnect(id), {})],
    ['POST', /^\/api\/devices\/([^/]+)\/pair$/, (_r, [id]) => manager.pair(id)],
    [
      'POST',
      /^\/api\/devices\/([^/]+)\/code$/,
      (_r, [id], body) => {
        const code = String(body.code ?? '').replace(/\D/g, '');
        if (!code) throw new HttpError(400, 'Enter the code shown on the device');
        return manager.code(id, code);
      },
    ],
    ['POST', /^\/api\/devices\/([^/]+)\/forget$/, (_r, [id]) => manager.forget(id)],
    [
      'PUT',
      /^\/api\/devices\/([^/]+)\/values$/,
      (_r, [id], body) => {
        if (!body.values || typeof body.values !== 'object') throw new HttpError(400, 'Expected {"values": {id: value}}');
        return manager.set(id, body.values as Record<string, never>);
      },
    ],
    [
      'POST',
      /^\/api\/devices\/([^/]+)\/invoke$/,
      async (_r, [id], body) => ({
        status: await manager.invoke(id, Number(body.action), (body.params ?? {}) as Record<string, never>),
      }),
    ],
  ];

  let vite: import('vite').ViteDevServer | null = null;
  const server = createServer(async (req, res) => {
    const url = new URL(req.url ?? '/', 'http://localhost');
    if (url.pathname === '/api/events' && req.method === 'GET') return events(req, res, manager);
    if (url.pathname.startsWith('/api/')) return api(req, res, url.pathname, routes);
    if (vite) return vite.middlewares(req, res);
    serveStatic(url.pathname, res);
  });

  if (options.dev) {
    const { createServer: createVite } = await import('vite');
    vite = await createVite({
      configFile: join(ROOT, 'vite.config.ts'),
      server: { middlewareMode: true, hmr: { server } },
      appType: 'spa',
    });
    server.on('close', () => void vite?.close());
  }
  return server;
}

async function api(req: IncomingMessage, res: ServerResponse, path: string, routes: Array<[string, RegExp, Handler]>) {
  try {
    const route = routes.find(([method, re]) => method === req.method && re.test(path));
    if (!route) throw new HttpError(404, `No ${req.method} ${path}`);
    if (req.method !== 'GET') checkSameOrigin(req);
    const params = route[1].exec(path)!.slice(1).map(decodeURIComponent);
    const body = req.method === 'GET' ? {} : await readJson(req);
    const result = await route[2](req, params, body);
    send(res, 200, result ?? {});
  } catch (err) {
    const [code, body] = errorBody(err);
    send(res, code, body);
  }
}

function errorBody(err: unknown): [number, ApiError] {
  if (err instanceof HttpError) return [err.code, { error: err.message }];
  if (err instanceof NotFound) return [404, { error: err.message }];
  if (err instanceof NotConnected) return [409, { error: err.message }];
  if (err instanceof BlatError) {
    const refused: number[] = [STATUS.authRequired, STATUS.wrongCode, STATUS.lockedOut, STATUS.notPermitted];
    return [refused.includes(err.status) ? 403 : 422, { error: err.message, status: err.status, detail: err.detail }];
  }
  return [502, { error: err instanceof Error ? err.message : String(err) }];
}

// State-changing requests must come from our own page: JSON (which a plain
// cross-site form can't send) and, when the browser says where it's from,
// from this host. Otherwise any site open in the same browser could change
// devices' settings through a server on localhost.
function checkSameOrigin(req: IncomingMessage) {
  const origin = req.headers.origin;
  if (origin && new URL(origin).host !== req.headers.host) throw new HttpError(403, 'Cross-origin request refused');
  if (!String(req.headers['content-type'] ?? '').startsWith('application/json')) {
    throw new HttpError(415, 'Send JSON');
  }
}

async function readJson(req: IncomingMessage): Promise<Record<string, unknown>> {
  let text = '';
  for await (const chunk of req) {
    text += chunk;
    if (text.length > 65536) throw new HttpError(413, 'Request too large');
  }
  if (!text) return {};
  try {
    const value = JSON.parse(text);
    if (typeof value !== 'object' || value === null || Array.isArray(value)) throw new Error();
    return value;
  } catch {
    throw new HttpError(400, 'Expected a JSON object');
  }
}

function send(res: ServerResponse, code: number, body: unknown) {
  res.writeHead(code, { 'content-type': 'application/json', 'cache-control': 'no-store' });
  res.end(JSON.stringify(body));
}

function events(req: IncomingMessage, res: ServerResponse, manager: DeviceManager) {
  res.writeHead(200, { 'content-type': 'text/event-stream', 'cache-control': 'no-store', connection: 'keep-alive' });
  const write = (e: ServerEvent) => res.write(`data: ${JSON.stringify(e)}\n\n`);
  write(manager.devicesEvent());
  const unsubscribe = manager.subscribe(write);
  const keepAlive = setInterval(() => res.write(': keep-alive\n\n'), 25000);
  req.on('close', () => {
    unsubscribe();
    clearInterval(keepAlive);
  });
}

const TYPES: Record<string, string> = {
  '.html': 'text/html; charset=utf-8',
  '.js': 'text/javascript',
  '.css': 'text/css',
  '.svg': 'image/svg+xml',
  '.png': 'image/png',
  '.ico': 'image/x-icon',
  '.json': 'application/json',
};

function serveStatic(pathname: string, res: ServerResponse) {
  if (!existsSync(join(DIST, 'index.html'))) {
    res.writeHead(503, { 'content-type': 'text/plain' });
    res.end('The web page isn\'t built. Run "pnpm build", or start the server with --dev.\n');
    return;
  }
  let file = normalize(join(DIST, decodeURIComponent(pathname)));
  if (!file.startsWith(DIST + sep) || !existsSync(file) || statSync(file).isDirectory()) file = join(DIST, 'index.html');
  res.writeHead(200, { 'content-type': TYPES[extname(file)] ?? 'application/octet-stream' });
  res.end(readFileSync(file));
}
