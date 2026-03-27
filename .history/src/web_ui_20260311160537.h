#pragma once

#include <Arduino.h>

static const char WEB_UI_HTML[] PROGMEM = R"html(
<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>ESP32-C3 VTX Control</title>
  <style>
    :root{
      --bg: #0f1720; /* deep slate */
      --card: linear-gradient(180deg,#0b1220 0%,#0f1726 100%);
      --muted: #98a2b3;
      --accent: #4dd0b4;
      --accent-2: #ffa94d;
      --glass: rgba(255,255,255,0.03);
      --line: rgba(255,255,255,0.04);
      --glass-2: rgba(255,255,255,0.02);
    }
    *{box-sizing:border-box}
    html,body{height:100%;}
    body{
      margin:0;
      font-family: Inter, system-ui, -apple-system, "Segoe UI", Roboto, "Helvetica Neue", Arial;
      background: radial-gradient(ellipse at 10% 10%, rgba(77,208,180,0.06), transparent 18%),
                  linear-gradient(180deg,#07121a 0%, #0b1720 100%);
      color: #e6eef6;
      -webkit-font-smoothing:antialiased;
      -moz-osx-font-smoothing:grayscale;
    }
    .shell{
      max-width:1100px;
      margin:18px auto;
      padding:18px;
    }
    .hero{
      padding:14px;
      border-radius:12px;
      background:linear-gradient(180deg, rgba(255,255,255,0.02), rgba(255,255,255,0.01));
      border:1px solid var(--line);
      display:flex;
      gap:12px;
      align-items:center;
    }
    h1{margin:0;font-size:1.25rem;letter-spacing:-0.02em}
    .subtitle{margin:0;color:var(--muted);font-size:0.9rem}
    .grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(260px,1fr));gap:12px;margin-top:12px}
    .panel{background:var(--card);border-radius:12px;padding:12px;border:1px solid var(--glass);box-shadow:0 6px 24px rgba(2,6,23,0.6)}
    h2{margin:0 0 10px;font-size:0.85rem;color:var(--muted);text-transform:uppercase}
    label{display:block;margin-bottom:8px;font-size:0.82rem;color:var(--muted)}
    input,select,button,textarea{width:100%;margin-top:6px;border-radius:8px;border:1px solid var(--glass-2);background:transparent;padding:8px 10px;color:inherit;font:inherit}
    textarea{min-height:200px;resize:vertical;font-family:monospace;font-size:0.85rem}
    button{cursor:pointer;padding:8px 10px;border-radius:8px;border:none;background:linear-gradient(90deg,var(--accent),#27b89d);color:#041018;font-weight:600;font-size:0.9rem}
    button.secondary{background:linear-gradient(90deg,var(--accent-2),#e58a33);color:#041018}
    button:hover{filter:brightness(1.05);transform:translateY(-1px)}
    .stack{display:grid;gap:8px}
    .chips{display:flex;flex-wrap:wrap;gap:8px}
    .chip{padding:6px 10px;border-radius:999px;background:rgba(255,255,255,0.02);font-size:0.8rem;color:var(--muted);border:1px solid var(--glass-2)}
    .status{min-height:40px;white-space:pre-wrap;color:var(--muted);font-size:0.88rem}
    .status strong{color:#e6eef6}
    .actions{display:flex;flex-wrap:wrap;gap:8px}
    .device-list{display:grid;gap:10px}
    .device-card{border-radius:10px;padding:10px;background:linear-gradient(180deg,rgba(255,255,255,0.01),transparent);border:1px solid rgba(255,255,255,0.03);display:flex;flex-direction:column;gap:8px}
    .device-card h3{margin:0;font-size:0.95rem;display:flex;align-items:center;justify-content:space-between;gap:8px}
    .device-card h3 .meta{font-size:0.78rem;color:var(--muted);font-weight:600}
    .control-grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(100px,1fr));gap:8px;margin-top:6px}
    .control-box{margin-top:8px;padding-top:8px;border-top:1px solid var(--glass-2)}
    .device-card .actions{justify-content:flex-end}
    .device-card .actions button{padding:6px 10px;font-size:0.85rem}
    /* compact helpers */
    .small{font-size:0.85rem;padding:6px}
    @media (max-width:640px){
      .hero{flex-direction:column;align-items:flex-start}
      .device-card h3{flex-direction:column;align-items:flex-start}
    }
  </style>
</head>
<body>
  <div class="shell">
    <section class="hero">
      <h1>VTX Control Deck</h1>
      <p class="subtitle">Manage several VTX devices from one ESP32-C3. Each device gets its own PWM input pin, VTX control pin, and protocol. The firmware serializes outgoing commands, so several VTXs can be controlled from one board as long as update rates stay modest.</p>
    </section>

    <div class="grid">
      <section class="panel">
        <h2>Live State</h2>
        <div class="chips" id="state-chips"></div>
        <div class="status" id="status-text">Loading state...</div>
      </section>

      <section class="panel">
        <h2>Global Config</h2>
        <form id="config-form" class="stack">
          <label>WiFi / ESP-NOW channel
            <input type="number" min="1" max="13" name="wifiChannel" required>
          </label>
          <label>QuickESPNow
            <select name="espNowEnabled">
              <option value="1">Enabled</option>
              <option value="0">Disabled</option>
            </select>
          </label>
          <div class="actions">
            <button type="button" id="add-device-button" class="secondary">Add VTX device</button>
          </div>
          <div id="device-list" class="device-list"></div>
          <button type="submit">Save configuration</button>
        </form>
      </section>

      <section class="panel">
        <h2>Upload Table</h2>
        <form id="upload-form" class="stack">
          <label>JSON file
            <input type="file" name="file" accept=".json,application/json" required>
          </label>
          <button type="submit" class="secondary">Upload JSON table</button>
        </form>
      </section>

      <section class="panel" style="grid-column: 1 / -1;">
        <h2>JSON Table Editor</h2>
        <form id="json-form" class="stack">
          <label>Load table from file list
            <select name="path" id="json-table-select"></select>
          </label>
          <label>Device assignment target
            <select id="json-device-select"></select>
          </label>
          <div class="actions">
            <button type="button" id="load-json-button">Load selected JSON</button>
            <button type="button" id="load-active-button" class="secondary">Load selected device table</button>
          </div>
          <label>Save as file name
            <input type="text" name="savePath" id="save-path" placeholder="custom_vtx.json">
          </label>
          <label>VTX table JSON
            <textarea name="json" id="json-editor" spellcheck="false"></textarea>
          </label>
          <div class="actions">
            <button type="submit">Save JSON table</button>
            <button type="button" id="select-saved-button" class="secondary">Save and assign to device</button>
          </div>
        </form>
      </section>
    </div>
  </div>

  <script>
    const stateText = document.getElementById('status-text');
    const chips = document.getElementById('state-chips');
    const configForm = document.getElementById('config-form');
    const uploadForm = document.getElementById('upload-form');
    const jsonForm = document.getElementById('json-form');
    const jsonTableSelect = document.getElementById('json-table-select');
    const jsonDeviceSelect = document.getElementById('json-device-select');
    const jsonEditor = document.getElementById('json-editor');
    const savePathInput = document.getElementById('save-path');
    const loadJsonButton = document.getElementById('load-json-button');
    const loadActiveButton = document.getElementById('load-active-button');
    const selectSavedButton = document.getElementById('select-saved-button');
    const addDeviceButton = document.getElementById('add-device-button');
    const deviceList = document.getElementById('device-list');
    let devicesState = [];
    let tablePaths = [];
    let statePollTimer = null;
    let restartPending = false;
    let restartReloadTimer = null;

    function scheduleReloadAfterRestart(delayMs = 3000) {
      if (restartReloadTimer) {
        clearTimeout(restartReloadTimer);
      }
      restartReloadTimer = window.setTimeout(() => {
        window.location.reload();
      }, delayMs);
    }

    function createDevice(device = {}) {
      return {
        name: device.name || `VTX ${devicesState.length + 1}`,
        pwmInputPin: Number(device.pwmInputPin ?? 2),
        vtxControlPin: Number(device.vtxControlPin ?? 4),
        protocol: device.protocol || 'smartaudio',
        controlMode: device.controlMode || 'pwm',
        activeControlMode: device.activeControlMode || device.controlMode || 'pwm',
        enabled: device.enabled !== false,
        manualBand: Number(device.manualBand ?? 1),
        manualChannel: Number(device.manualChannel ?? 1),
        manualPowerIndex: Number(device.manualPowerIndex ?? 0),
        bandOptions: Array.isArray(device.bandOptions) ? device.bandOptions : [],
        channelCount: Number(device.channelCount ?? 8),
        powerOptions: Array.isArray(device.powerOptions) ? device.powerOptions : [],
        vtxTablePath: device.vtxTablePath || tablePaths[0] || '/peak_thor_t35.json',
        filteredPulse: Number(device.filteredPulse ?? 1500),
        lastMappedIndex: Number(device.lastMappedIndex ?? -1),
        currentPwmUs: Number(device.currentPwmUs ?? device.filteredPulse ?? 1500),
        band: device.band ?? null,
        bandLabel: device.bandLabel || '',
        channel: device.channel ?? null,
        powerIndex: device.powerIndex ?? null,
        powerValue: device.powerValue ?? null,
        powerLabel: device.powerLabel || '',
        frequency: device.frequency ?? null
      };
    }

    function describeDeviceLiveState(device) {
      const band = device.bandLabel || (device.band != null ? String(device.band) : 'n/a');
      const channel = device.channel != null ? device.channel : 'n/a';
      const power = device.powerLabel || (device.powerValue != null ? String(device.powerValue) : 'n/a');
      const frequency = device.frequency != null ? `${device.frequency} MHz` : 'n/a';
      return `PWM ${device.currentPwmUs} us | Band ${band} | Channel ${channel} | Power ${power} | Freq ${frequency}`;
    }

    function getTableOptions(selectedPath) {
      const options = [...tablePaths];
      if (selectedPath && !options.includes(selectedPath)) {
        options.push(selectedPath);
      }
      return options;
    }

    function getSelectedBandFrequencies(device) {
      const selectedBandValue = Number(device.manualBand || 1);
      const selectedBand = (device.bandOptions || []).find((option) => Number(option.value) === selectedBandValue);
      return Array.isArray(selectedBand?.frequencies) ? selectedBand.frequencies : [];
    }

    function addTablePath(path) {
      if (!path || tablePaths.includes(path)) {
        return;
      }
      tablePaths.push(path);
    }

    function renderJsonDeviceSelect() {
      const previousValue = Number(jsonDeviceSelect.value || 0);
      jsonDeviceSelect.innerHTML = '';
      devicesState.forEach((device, index) => {
        const option = document.createElement('option');
        option.value = String(index);
        option.textContent = `${index + 1}: ${device.name}`;
        jsonDeviceSelect.appendChild(option);
      });
      if (devicesState.length > 0) {
        jsonDeviceSelect.value = String(Math.min(previousValue, devicesState.length - 1));
      }
    }

    function renderDeviceList() {
      deviceList.innerHTML = '';
      devicesState.forEach((device, index) => {
        const tableOptions = getTableOptions(device.vtxTablePath).map((path) => `
              <option value="${path}" ${device.vtxTablePath === path ? 'selected' : ''}>${path}</option>`).join('');
        const bandOptions = (device.bandOptions || []).map((option) => `
          <option value="${option.value}" ${Number(device.manualBand) === Number(option.value) ? 'selected' : ''}>${option.label || option.value}</option>`).join('');
        const selectedBandFrequencies = getSelectedBandFrequencies(device);
        const channelOptions = Array.from({ length: Math.max(1, device.channelCount || 8) }, (_, channelIndex) => {
          const value = channelIndex + 1;
          const frequency = Number(selectedBandFrequencies[channelIndex] ?? 0);
          const label = frequency > 0 ? `${value} (${frequency})` : `${value}`;
          return `<option value="${value}" ${Number(device.manualChannel) === value ? 'selected' : ''}>${label}</option>`;
        }).join('');
        const powerOptions = (device.powerOptions || []).map((option) => `
          <option value="${option.value}" ${Number(device.manualPowerIndex) === Number(option.value) ? 'selected' : ''}>${option.label || option.powerValue || option.value}</option>`).join('');
        const serialControlEnabled = device.enabled && device.activeControlMode === 'serial' && !restartPending;
        const card = document.createElement('div');
        card.className = 'device-card';
        card.innerHTML = `
          <h3>Device ${index + 1}</h3>
          <label>Name
            <input type="text" value="${device.name}">
          </label>
          <label>PWM input pin
            <input type="number" min="0" max="21" value="${device.pwmInputPin}">
          </label>
          <label>VTX control pin
            <input type="number" min="0" max="21" value="${device.vtxControlPin}">
          </label>
          <label>Protocol
            <select>
              <option value="smartaudio" ${device.protocol === 'smartaudio' ? 'selected' : ''}>SmartAudio</option>
              <option value="tramp" ${device.protocol === 'tramp' ? 'selected' : ''}>TRAMP</option>
            </select>
          </label>
          <label>Control
            <select>
              <option value="pwm" ${device.controlMode === 'pwm' ? 'selected' : ''}>PWM</option>
              <option value="serial" ${device.controlMode === 'serial' ? 'selected' : ''}>Serial</option>
            </select>
          </label>
          <label>VTX table
            <select>${tableOptions}
            </select>
          </label>
          <label>Enabled
            <select>
              <option value="1" ${device.enabled ? 'selected' : ''}>Enabled</option>
              <option value="0" ${!device.enabled ? 'selected' : ''}>Disabled</option>
            </select>
          </label>
          ${serialControlEnabled ? `
          <div class="control-box">
            <div class="status"><strong>Manual Control</strong></div>
            <div class="control-grid">
              <label>Band
                <select data-role="manual-band">${bandOptions}</select>
              </label>
              <label>Channel
                <select data-role="manual-channel">${channelOptions}</select>
              </label>
              <label>Power
                <select data-role="manual-power">${powerOptions}</select>
              </label>
            </div>
            <div class="actions">
              <button type="button" class="apply-control-button">Apply serial control</button>
            </div>
          </div>` : ''}
          <div class="status" data-role="live-state">${describeDeviceLiveState(device)}</div>
          <div class="actions">
            <button type="button" class="secondary">Remove device</button>
          </div>
        `;

        const [nameInput, pwmInput, vtxInput, protocolInput, controlModeInput, tableInput, enabledInput] = card.querySelectorAll('input, select');
        const manualBandInput = card.querySelector('[data-role="manual-band"]');
        const manualChannelInput = card.querySelector('[data-role="manual-channel"]');
        const manualPowerInput = card.querySelector('[data-role="manual-power"]');
        const applyControlButton = card.querySelector('.apply-control-button');
        nameInput.addEventListener('input', () => { device.name = nameInput.value; });
        pwmInput.addEventListener('input', () => { device.pwmInputPin = Number(pwmInput.value); });
        vtxInput.addEventListener('input', () => { device.vtxControlPin = Number(vtxInput.value); });
        protocolInput.addEventListener('change', () => { device.protocol = protocolInput.value; });
        controlModeInput.addEventListener('change', () => {
          device.controlMode = controlModeInput.value;
          renderDeviceList();
        });
        tableInput.addEventListener('change', () => { device.vtxTablePath = tableInput.value; });
        enabledInput.addEventListener('change', () => { device.enabled = enabledInput.value === '1'; });
        if (manualBandInput) {
          manualBandInput.addEventListener('change', () => { device.manualBand = Number(manualBandInput.value); });
        }
        if (manualChannelInput) {
          manualChannelInput.addEventListener('change', () => { device.manualChannel = Number(manualChannelInput.value); });
        }
        if (manualPowerInput) {
          manualPowerInput.addEventListener('change', () => { device.manualPowerIndex = Number(manualPowerInput.value); });
        }
        if (applyControlButton) {
          applyControlButton.addEventListener('click', async () => {
            const body = new URLSearchParams();
            body.set('device', String(index));
            body.set('band', String(device.manualBand || 1));
            body.set('channel', String(device.manualChannel || 1));
            body.set('power', String(device.manualPowerIndex || 0));
            const response = await fetch('/api/control', { method: 'POST', body });
            const data = await response.json();
            setStatus(data.message || 'Manual control processed', data.ok ? 'Applied' : 'Control failed');
            if (data.ok) {
              await loadState();
            }
          });
        }
        card.querySelector('button.secondary').addEventListener('click', () => {
          devicesState.splice(index, 1);
          if (devicesState.length === 0) {
            devicesState.push(createDevice());
          }
          renderDeviceList();
          renderJsonDeviceSelect();
        });
        deviceList.appendChild(card);
      });
      renderJsonDeviceSelect();
    }

    function applyLiveState(state) {
      chips.innerHTML = '';
      [
        `SSID ${state.ssid}`,
        `IP ${state.ip}`,
        `Devices ${state.deviceCount || devicesState.length}`,
        `ESP-NOW ${state.espNowEnabled ? 'on' : 'off'}`
      ].forEach((text) => {
        const chip = document.createElement('div');
        chip.className = 'chip';
        chip.textContent = text;
        chips.appendChild(chip);
      });

      setStatus(`Configured devices: ${state.deviceCount || devicesState.length}\nTransport ready: ${state.transportReady ? 'yes' : 'no'}\nEach device keeps its own VTX table assignment.\nCommands are serialized across devices.`);
      restartPending = Boolean(state.restartPending);
      if (restartPending) {
        setStatus(`Configured devices: ${state.deviceCount || devicesState.length}\nTransport ready: ${state.transportReady ? 'yes' : 'no'}\nRestart pending; wait for the board to come back before using serial control.`, 'Restarting');
        scheduleReloadAfterRestart();
      }

      const liveDevices = Array.isArray(state.devices) ? state.devices : [];
      liveDevices.forEach((liveDevice, index) => {
        if (!devicesState[index]) {
          return;
        }
        devicesState[index].activeControlMode = liveDevice.controlMode || devicesState[index].activeControlMode;
        devicesState[index].filteredPulse = Number(liveDevice.filteredPulse ?? devicesState[index].filteredPulse);
        devicesState[index].lastMappedIndex = Number(liveDevice.lastMappedIndex ?? devicesState[index].lastMappedIndex);
        devicesState[index].currentPwmUs = Number(liveDevice.currentPwmUs ?? devicesState[index].currentPwmUs);
        devicesState[index].band = liveDevice.band ?? null;
        devicesState[index].bandLabel = liveDevice.bandLabel || '';
        devicesState[index].channel = liveDevice.channel ?? null;
        devicesState[index].powerIndex = liveDevice.powerIndex ?? null;
        devicesState[index].powerValue = liveDevice.powerValue ?? null;
        devicesState[index].powerLabel = liveDevice.powerLabel || '';
        devicesState[index].frequency = liveDevice.frequency ?? null;

        const card = deviceList.children[index];
        const liveState = card ? card.querySelector('[data-role="live-state"]') : null;
        if (liveState) {
          liveState.textContent = describeDeviceLiveState(devicesState[index]);
        }
      });
    }

    function setStatus(message, strong) {
      stateText.innerHTML = strong ? `<strong>${strong}</strong>\n${message}` : message;
    }

    function fillState(state) {
      for (const [key, value] of Object.entries({
        wifiChannel: state.wifiChannel,
        espNowEnabled: state.espNowEnabled ? '1' : '0'
      })) {
        const input = configForm.elements.namedItem(key);
        if (input) {
          input.value = value;
        }
      }

      jsonTableSelect.innerHTML = '';
      tablePaths = Array.isArray(state.tables) ? [...state.tables] : [];
      tablePaths.forEach((path) => {
        const editorOption = document.createElement('option');
        editorOption.value = path;
        editorOption.textContent = path;
        jsonTableSelect.appendChild(editorOption);
      });

      if (!savePathInput.value) {
        savePathInput.value = (tablePaths[0] || '').replace(/^\//, '');
      }

      devicesState = (state.devices || []).map((device) => createDevice(device));
      if (devicesState.length === 0) {
        devicesState = [createDevice()];
      }
      renderDeviceList();

      applyLiveState(state);
    }

    async function loadState() {
      const response = await fetch('/api/state');
      if (!response.ok) {
        throw new Error('Failed to load state');
      }
      const data = await response.json();
      fillState(data);
      return data;
    }

    async function pollLiveState() {
      try {
        const response = await fetch('/api/state');
        if (!response.ok) {
          throw new Error('Failed to refresh state');
        }
        const state = await response.json();
        if (Array.isArray(state.devices) && state.devices.length === devicesState.length) {
          applyLiveState(state);
        }
      } catch (error) {
        if (restartPending) {
          scheduleReloadAfterRestart(1500);
        }
        console.error(error);
      }
    }

    async function loadTableJson(path = '', deviceIndex = null) {
      let url = '/api/vtx-table';
      if (typeof deviceIndex === 'number') {
        url = `/api/vtx-table?device=${encodeURIComponent(deviceIndex)}`;
      } else if (path) {
        url = `/api/vtx-table?path=${encodeURIComponent(path)}`;
      }
      const response = await fetch(url);
      if (!response.ok) {
        throw new Error('Failed to load table JSON');
      }
      const data = await response.json();
      jsonEditor.value = data.json || '';
      savePathInput.value = (data.path || '').replace(/^\//, '');
      if (data.path) {
        jsonTableSelect.value = data.path;
      }
      setStatus(`Loaded ${data.path || 'device table'} into editor`, 'JSON ready');
      return data;
    }

    async function saveTableJson(selectAfterSave) {
      const body = new URLSearchParams();
      body.set('path', savePathInput.value || 'custom_vtx.json');
      body.set('json', jsonEditor.value);
      body.set('select', selectAfterSave ? '1' : '0');
      body.set('device', jsonDeviceSelect.value || '0');

      const response = await fetch('/api/vtx-table', { method: 'POST', body });
      const data = await response.json();
      setStatus(data.message || 'JSON save processed', data.ok ? 'JSON saved' : 'JSON failed');
      await loadState();
      if (data.ok) {
        await loadTableJson(data.path || '');
      }
    }

    configForm.addEventListener('submit', async (event) => {
      event.preventDefault();
      const body = new URLSearchParams();
      body.set('wifiChannel', configForm.elements.namedItem('wifiChannel').value);
      body.set('espNowEnabled', configForm.elements.namedItem('espNowEnabled').value);
      body.set('devicesJson', JSON.stringify(devicesState.map((device) => ({
        name: device.name,
        pwmInputPin: Number(device.pwmInputPin),
        vtxControlPin: Number(device.vtxControlPin),
        protocol: device.protocol,
        controlMode: device.controlMode,
        enabled: Boolean(device.enabled),
        manualBand: Number(device.manualBand || 1),
        manualChannel: Number(device.manualChannel || 1),
        manualPowerIndex: Number(device.manualPowerIndex || 0),
        vtxTablePath: device.vtxTablePath
      }))));
      const response = await fetch('/api/config', { method: 'POST', body });
      const data = await response.json();
      setStatus(data.message || 'Configuration saved', data.restartRequired ? 'Restarting' : 'Saved');
      restartPending = Boolean(data.restartRequired);
      if (restartPending) {
        scheduleReloadAfterRestart();
      }
      await loadState();
    });

    uploadForm.addEventListener('submit', async (event) => {
      event.preventDefault();
      const body = new FormData(uploadForm);
      const response = await fetch('/api/upload', { method: 'POST', body });
      const data = await response.json();
      setStatus(data.message || 'Upload processed', data.ok ? 'Upload complete' : 'Upload failed');
      if (data.ok && data.path) {
        addTablePath(data.path);
      }
      await loadState();
      uploadForm.reset();
    });

    loadJsonButton.addEventListener('click', async () => {
      await loadTableJson(jsonTableSelect.value);
    });

    loadActiveButton.addEventListener('click', async () => {
      await loadTableJson('', Number(jsonDeviceSelect.value || 0));
    });

    jsonForm.addEventListener('submit', async (event) => {
      event.preventDefault();
      await saveTableJson(false);
    });

    selectSavedButton.addEventListener('click', async () => {
      await saveTableJson(true);
    });

    addDeviceButton.addEventListener('click', () => {
      devicesState.push(createDevice({
        name: `VTX ${devicesState.length + 1}`,
        pwmInputPin: 2 + devicesState.length,
        vtxControlPin: 4 + devicesState.length,
        protocol: 'smartaudio',
        controlMode: 'pwm',
        enabled: true,
        vtxTablePath: tablePaths[0] || '/peak_thor_t35.json'
      }));
      renderDeviceList();
    });

    loadState().then(() => {
      loadTableJson('', 0);
      if (statePollTimer) {
        clearInterval(statePollTimer);
      }
      statePollTimer = window.setInterval(pollLiveState, 1000);
    }).catch((error) => {
      setStatus(error.message || String(error), 'Error');
    });
  </script>
</body>
</html>
)html";
