import { defineConfig } from '@playwright/test';
export default defineConfig({
  testDir: './test/e2e',
  reporter: 'list',
  use: { baseURL: 'http://127.0.0.1:41733', trace: 'off', screenshot: 'off' },
  projects: [{ name: 'chromium', use: { browserName: 'chromium' } }, { name: 'webkit', use: { browserName: 'webkit' } }],
  webServer: { command: 'node scripts/fixture-server.mjs', url: 'http://127.0.0.1:41733/fixture.html', reuseExistingServer: false, timeout: 30000 },
});
