import { test, expect } from '@playwright/test';

test('connection, settings draft, and Cross arrangement work through the fixture bridge', async ({ page }) => {
  await page.goto('/fixture.html');
  const ui = page.frameLocator('iframe');
  await expect(ui.getByRole('heading', { name: 'Connection' })).toBeVisible();
  await ui.getByRole('button', { name: 'Connect', exact: true }).click();
  await expect(ui.getByText('1920 × 1080')).toBeVisible();
  await ui.getByRole('button', { name: 'Settings' }).click();
  const port = ui.getByLabel('Video port');
  await port.fill('50020');
  await page.evaluate(() => (window as typeof window & { fixtureUpdate: (value: unknown) => void }).fixtureUpdate({ diagnostics: { protocolVersion: 4, logs: ['new status'] } }));
  await expect(port).toHaveValue('50020');
  await ui.getByRole('button', { name: 'Save and Restart' }).click();
  await ui.getByRole('button', { name: 'Connection' }).click();
  await ui.getByRole('button', { name: 'Cross', exact: true }).click();
  await expect(ui.getByRole('heading', { name: 'Arrange displays' })).toBeVisible();
  await ui.getByRole('button', { name: 'Connect Cross' }).click();
  await expect(ui.getByText('482 931')).toBeVisible();
  await ui.getByRole('button', { name: 'Approve pairing' }).click();
  await expect(ui.getByText('Paired: Mac Studio')).toBeVisible();
  await ui.getByRole('button', { name: 'Mac display', exact: true }).click();
  await ui.getByRole('button', { name: 'Right' }).click();
  await expect(ui.getByText('Every Mac display needs a touching-edge route back to Windows.')).toBeVisible();
  await ui.getByRole('button', { name: 'Left' }).click();
  await expect(ui.getByRole('button', { name: 'Apply arrangement' })).toBeEnabled();
  await ui.getByRole('button', { name: 'Apply arrangement' }).click();
});

test('small and large viewports keep navigation and controls accessible', async ({ page }) => {
  await page.goto('/fixture.html');
  const ui = page.frameLocator('iframe');
  for (const width of [390, 768, 1024, 1440]) {
    await page.setViewportSize({ width, height: 800 });
    await expect(ui.getByRole('button', { name: 'Settings' })).toBeVisible();
    await ui.getByRole('button', { name: 'Settings' }).click();
    await expect(ui.getByLabel('Video port')).toBeVisible();
    await expect(ui.getByRole('button', { name: 'Save and Restart' })).toBeVisible();
  }
});

test('Mac display choice keeps an unavailable saved display visible for review', async ({ page }) => {
  await page.goto('/fixture.html?role=mac');
  const ui = page.frameLocator('iframe');
  await ui.getByRole('button', { name: 'Settings' }).click();
  const display = ui.getByLabel('Capture display');
  await expect(display).toHaveValue('7');
  await expect(display.locator('option[value="7"]')).toHaveText('Saved display unavailable');
  await display.selectOption('0');
  await expect(display).toHaveValue('0');
  await expect(display.locator('option[value="1"]')).toHaveAttribute('disabled', '');
});
