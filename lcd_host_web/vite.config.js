import { defineConfig } from 'vite';

export default defineConfig({
  root: '.',
  publicDir: false,
  build: {
    outDir: 'dist-ui',
    emptyOutDir: true,
    rollupOptions: { input: 'index.html' },
  },
});
