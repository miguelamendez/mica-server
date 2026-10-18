import type {ExtensionAPI} from '@earendil-works/pi-coding-agent';
import {connectionOptions, discover} from './discovery.mjs';

export default async function (pi: ExtensionAPI) {
  const registered = new Set<string>();
  async function refresh() {
    const connection = await connectionOptions();
    const snapshot = await discover(connection);
    // Updated only after successful discovery; previous providers survive outages.
    process.env.MICA_PI_API_KEY = connection.key;
    const next = new Set<string>();
    for (const provider of snapshot.providers) {
      pi.registerProvider(provider.id, provider.config);
      next.add(provider.id);
    }
    for (const id of registered) if (!next.has(id)) {
      if (snapshot.failedProviders.includes(id)) next.add(id);
      else pi.unregisterProvider(id);
    }
    registered.clear();
    for (const id of next) registered.add(id);
    return `${snapshot.providers.reduce((sum, provider) => sum + provider.config.models.length, 0)} model entries across ${next.size} workloads` +
      (snapshot.warnings.length ? `\n${snapshot.warnings.join('\n')}` : '');
  }
  pi.registerCommand('mica-refresh', {
    description: 'Discover Mica workload models and refresh limits/credentials',
    handler: async (_args, ctx) => {
      try { ctx.ui.notify(await refresh(), 'info'); }
      catch (error) { ctx.ui.notify(`Mica discovery failed: ${error.message}`, 'error'); }
    },
  });
  // Pi awaits the factory, making discovered providers available to --list-models.
  try { await refresh(); }
  catch (error) { console.error(`Mica discovery unavailable: ${error.message}. Use /mica-refresh after reconnecting.`); }
}
