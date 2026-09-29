import './styles.css';
import './app';
import { Bridge } from './bridge';
import type { ThunderlinkApp } from './app';

async function start(): Promise<void> {
  let buildId = 'unavailable';
  try {
    const response = await fetch('./manifest.json', { cache: 'no-store' });
    if (response.ok) buildId = String((await response.json()).buildId);
  } catch { /* Native recovery handles missing assets. */ }
  const bridge = new Bridge(buildId);
  const app = document.querySelector<ThunderlinkApp>('thunderlink-app');
  if (app) app.setBridge(bridge);
  bridge.start();
}
void start();
