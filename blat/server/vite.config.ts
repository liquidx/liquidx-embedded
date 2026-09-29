// The web pages: plain TypeScript and DOM, no framework. index.html talks to
// blat-server; bluetooth.html is the host itself, over Web Bluetooth, and
// works from any static host (HTTPS or localhost). `pnpm build` writes dist/,
// which the server serves; `pnpm dev` runs Vite inside the server.
import { fileURLToPath } from 'node:url';
import { defineConfig } from 'vite';

const page = (name: string) => fileURLToPath(new URL(`web/${name}`, import.meta.url));

export default defineConfig({
  root: 'web',
  build: {
    outDir: '../dist',
    emptyOutDir: true,
    rollupOptions: { input: { main: page('index.html'), bluetooth: page('bluetooth.html') } },
  },
});
