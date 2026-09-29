import { defineConfig } from 'vitest/config';

export default defineConfig({
  base: './',
  build: {
    emptyOutDir: true,
    cssCodeSplit: false,
    lib: { entry: 'src/main.ts', name: 'ThunderLinkUI', formats: ['iife'], fileName: () => 'ui.js', cssFileName: 'ui' },
    assetsInlineLimit: 0,
    minify: 'oxc',
  },
  test: { environment: 'node', include: ['test/*.test.ts'] },
});
