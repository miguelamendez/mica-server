/* Authenticated workload controls. No credentials or local paths in this asset. */
(function (root) {
  function speedLabel(metrics) {
    const speed = metrics?.decode_tokens_per_second;
    return metrics?.available && typeof speed === 'number' && Number.isFinite(speed) && speed > 0
      ? `${speed.toFixed(1)} tok/s · decode` : 'Decode speed unavailable';
  }

  function createWorkloadPicker(options) {
    const {select, description, active, notice, button, request, onActivated,
      onBusy, onState, isBusy, confirmSwitch} = options;
    const delay = options.delay || (ms => new Promise(resolve => setTimeout(resolve, ms)));
    let workloads = [], activeId = '', ready = false, switching = false, loading = null;
    function update() {
      const selected = workloads.find(item => item.id === select.value);
      active.textContent = activeId ? `Active: ${activeId}${ready ? '' : ' · warming'}` : 'Connect to list workloads';
      description.textContent = selected ? `${selected.description || selected.id}\n` +
        `${selected.required_ram_gib} GiB RAM · ${selected.required_vram_gib} GiB VRAM required\n` +
        (selected.models || []).map(model => `${model.id} (${model.quantization})`).join(' · ') : '';
      const blockers = selected?.activation_blockers || [];
      notice.textContent = switching ? 'Switching workload and waiting for warmup…' :
        blockers.length ? blockers.join(' ') + ' Use mica-server workload install while stopped.' :
        !ready && activeId ? 'Server is warming. Controls refresh automatically.' :
        selected?.id === activeId ? 'This workload is active.' :
        'Switching keeps compatible loaded models and warms missing startup models.';
      select.disabled = switching || isBusy() || !workloads.length;
      button.disabled = switching || isBusy() || !ready || !selected ||
        selected.id === activeId || !selected.can_activate;
      button.textContent = switching ? 'Switching…' : 'Switch workload';
    }
    async function refresh() {
      if (loading) return loading;
      loading = (async () => {
        const catalog = await request('/api/workloads');
        const previous = select.value;
        workloads = catalog.data || []; activeId = catalog.active_workload || ''; ready = !!catalog.ready;
        select.innerHTML = '';
        for (const item of workloads) {
          const option = new Option(`${item.id}${item.id === activeId ? ' · active' : ''}`, item.id);
          select.add(option);
        }
        select.value = workloads.some(item => item.id === previous) ? previous : activeId;
        update();
        if (onState) onState({ready, activeId});
        return catalog;
      })();
      try { return await loading; } finally { loading = null; }
    }
    async function activate() {
      const selected = workloads.find(item => item.id === select.value);
      if (switching || isBusy() || !ready || !selected || !selected.can_activate || selected.id === activeId) return false;
      if (!confirmSwitch(`Switch the server to ${selected.id}? This affects every connected chat. Your conversation stays saved.`)) return false;
      switching = true; onBusy(true); update();
      try {
        await request('/api/workloads/activate', {method: 'POST',
          headers: {'Content-Type': 'application/json'}, body: JSON.stringify({profile: selected.id})});
        for (let attempt = 0; attempt < 180; ++attempt) {
          await refresh();
          if (activeId !== selected.id) throw Error('Another client changed the active workload. Refresh and try again.');
          if (ready) break;
          await delay(1000);
        }
        await onActivated();
        return true;
      } finally {
        switching = false; onBusy(false); update();
      }
    }
    select.onchange = update;
    return {refresh, activate, update};
  }
  const api = {createWorkloadPicker, speedLabel};
  if (typeof module !== 'undefined' && module.exports) module.exports = api;
  else root.MicaChatSettings = api;
})(typeof globalThis !== 'undefined' ? globalThis : this);
