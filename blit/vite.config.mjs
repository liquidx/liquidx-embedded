// Dev server for the web demo and examples: `npx vite` from blit/.
// The root is blit/ (not web/) because the pages import ../js/.
export default {
  server: {
    host: 'localhost', // Web Bluetooth needs a secure context; localhost is one
    port: 8000,
    open: '/web/',
  },
  // Only the web pages are app entries; chrome-extension/ has its own build.
  optimizeDeps: { entries: ['web/**/*.html'] },
};
