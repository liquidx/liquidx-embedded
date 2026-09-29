// The web page: plain TypeScript and DOM, no framework. `pnpm build` writes
// dist/, which the server serves; `pnpm dev` runs Vite inside the server.
import { defineConfig } from 'vite';

export default defineConfig({
  root: 'web',
  build: { outDir: '../dist', emptyOutDir: true },
});
