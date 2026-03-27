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
    :root {
      --bg: #f3efe5;
      --panel: rgba(255,255,255,0.84);
      --ink: #16211d;
      --muted: #5d695f;
      --accent: #0f7c59;
      --accent-2: #c96b2c;
      --line: rgba(22,33,29,0.12);
      --shadow: 0 24px 60px rgba(30, 40, 35, 0.14);
    }
    * { box-sizing: border-box; }
    body {
      margin: 0;
      font-family: Georgia, "Times New Roman", serif;
      color: var(--ink);
      background:
        radial-gradient(circle at top left, rgba(201,107,44,0.18), transparent 32%),
        radial-gradient(circle at bottom right, rgba(15,124,89,0.14), transparent 28%),
        linear-gradient(140deg, #efe7d7, var(--bg));
      min-height: 100vh;
    }
    .shell {
      max-width: 980px;
      margin: 0 auto;
      padding: 28px 18px 48px;
    }
    .hero {
      padding: 28px;
      border: 1px solid var(--line);
      background: linear-gradient(135deg, rgba(255,255,255,0.9), rgba(255,255,255,0.62));
      box-shadow: var(--shadow);
      border-radius: 28px;
      backdrop-filter: blur(10px);
    }
    h1 {
      margin: 0 0 8px;
      font-size: clamp(2rem, 3.2vw, 3.2rem);
      line-height: 0.95;
      letter-spacing: -0.04em;
    }
    .subtitle {
      margin: 0;
      max-width: 700px;
      color: var(--muted);
      font-size: 1rem;
    }
    .grid {
      display: grid;
      grid-template-columns: repeat(auto-fit, minmax(280px, 1fr));
      gap: 18px;
      margin-top: 18px;
    }
    .panel {
      border: 1px solid var(--line);
      background: var(--panel);
      border-radius: 24px;
      padding: 22px;
      box-shadow: var(--shadow);
      backdrop-filter: blur(8px);
    }
    h2 {
      margin: 0 0 16px;
      font-size: 1.15rem;
      letter-spacing: 0.02em;
      text-transform: uppercase;
    }
    label {
      display: block;
      margin-bottom: 14px;
      font-size: 0.9rem;
      color: var(--muted);
    }
    input, select, button, textarea {
      width: 100%;
      margin-top: 6px;
      border-radius: 14px;
      border: 1px solid rgba(22,33,29,0.18);
      background: rgba(255,255,255,0.92);
      padding: 12px 14px;
      font: inherit;
      color: var(--ink);
    }
    textarea {
      min-height: 280px;
      resize: vertical;
      font-family: "Courier New", monospace;
      font-size: 0.9rem;
      line-height: 1.4;
    }
    button {
      cursor: pointer;
      background: linear-gradient(135deg, var(--accent), #0e6146);
      color: #f7fbf8;
      font-weight: 700;
      border: none;
      transition: transform 140ms ease, box-shadow 140ms ease;
      box-shadow: 0 16px 30px rgba(15,124,89,0.22);
    }
    button.secondary {
      background: linear-gradient(135deg, var(--accent-2), #9b4f1f);
      box-shadow: 0 16px 30px rgba(201,107,44,0.18);
    }
    button:hover {
      transform: translateY(-1px);
    }
    .stack {
      display: grid;
      gap: 10px;
    }
    .chips {
      display: flex;
      flex-wrap: wrap;
      gap: 10px;
    }
    .chip {
      padding: 10px 12px;
      border-radius: 999px;
      border: 1px solid var(--line);
      background: rgba(255,255,255,0.78);
      font-size: 0.88rem;
    }
    .status {
      min-height: 52px;
      white-space: pre-wrap;
      color: var(--muted);
    }
    .status strong {
      color: var(--ink);
    }
    .actions {
      display: grid;
      grid-template-columns: repeat(auto-fit, minmax(180px, 1fr));
      gap: 10px;
    }
  </style>
</head>
<body>
  <div class="shell">
    <section class="hero">
      <h1>ESP32-C3<br>VTX Control Deck</h1>
      <p class="subtitle">Configure PWM input pin, VTX control pin, active protocol, and the selected VTX table. Upload new Betaflight-style JSON tables directly into LittleFS and manage the device over HTTP and QuickESPNow on the same AP channel.</p>
    </section>

    <div class="grid">
      <section class="panel">
        <h2>Live State</h2>
        <div class="chips" id="state-chips"></div>
        <div class="status" id="status-text">Loading state...</div>
      </section>

      <section class="panel">
        <h2>Device Config</h2>
        <form id="config-form" class="stack">
          <label>PWM input pin
            <input type="number" min="0" max="21" name="pwmInputPin" required>
          </label>
          <label>VTX control pin
            <input type="number" min="0" max="21" name="vtxUartPin" required>
          </label>
          <label>Protocol
            <select name="protocol">
              <option value="smartaudio">SmartAudio</option>
              <option value="tramp">TRAMP</option>
            </select>
          </label>
          <label>WiFi / ESP-NOW channel
            <input type="number" min="1" max="13" name="wifiChannel" required>
          </label>
          <label>VTX table
            <select name="selectedVtxPath" id="table-select"></select>
          </label>
          <label>QuickESPNow
            <select name="espNowEnabled">
              <option value="1">Enabled</option>
              <option value="0">Disabled</option>
            </select>
          </label>
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
          <label>Load table from memory or file list
            <select name="path" id="json-table-select"></select>
          </label>
          <div class="actions">
            <button type="button" id="load-json-button">Load selected JSON</button>
            <button type="button" id="load-active-button" class="secondary">Load active JSON</button>
          </div>
          <label>Save as file name
            <input type="text" name="savePath" id="save-path" placeholder="custom_vtx.json">
          </label>
          <label>VTX table JSON
            <textarea name="json" id="json-editor" spellcheck="false"></textarea>
          </label>
          <div class="actions">
            <button type="submit">Save JSON table</button>
            <button type="button" id="select-saved-button" class="secondary">Save and select</button>
          </div>
        </form>
      </section>
    </div>
  </div>

  <script>
    const stateText = document.getElementById('status-text');
    const chips = document.getElementById('state-chips');
    const tableSelect = document.getElementById('table-select');
    const configForm = document.getElementById('config-form');
    const uploadForm = document.getElementById('upload-form');
    const jsonForm = document.getElementById('json-form');
    const jsonTableSelect = document.getElementById('json-table-select');
    const jsonEditor = document.getElementById('json-editor');
    const savePathInput = document.getElementById('save-path');
    const loadJsonButton = document.getElementById('load-json-button');
    const loadActiveButton = document.getElementById('load-active-button');
    const selectSavedButton = document.getElementById('select-saved-button');

    function setStatus(message, strong) {
      stateText.innerHTML = strong ? `<strong>${strong}</strong>\n${message}` : message;
    }

    function fillState(state) {
      const formData = new FormData(configForm);
      for (const [key, value] of Object.entries({
        pwmInputPin: state.pwmInputPin,
        vtxUartPin: state.vtxUartPin,
        protocol: state.protocol,
        wifiChannel: state.wifiChannel,
        espNowEnabled: state.espNowEnabled ? '1' : '0'
      })) {
        const input = configForm.elements.namedItem(key);
        if (input) {
          input.value = value;
        }
      }

      tableSelect.innerHTML = '';
      jsonTableSelect.innerHTML = '';
      const tablePaths = Array.isArray(state.tables) ? [...state.tables] : [];
      if (state.selectedVtxPath && !tablePaths.includes(state.selectedVtxPath)) {
        tablePaths.push(state.selectedVtxPath);
      }
      tablePaths.forEach((path) => {
        const option = document.createElement('option');
        option.value = path;
        option.textContent = path;
        if (path === state.selectedVtxPath) {
          option.selected = true;
        }
        tableSelect.appendChild(option);

        const editorOption = option.cloneNode(true);
        jsonTableSelect.appendChild(editorOption);
      });

      if (!savePathInput.value) {
        savePathInput.value = (state.selectedVtxPath || '').replace(/^\//, '');
      }

      chips.innerHTML = '';
      [
        `SSID ${state.ssid}`,
        `IP ${state.ip}`,
        `Protocol ${state.protocol}`,
        `PWM GPIO ${state.pwmInputPin}`,
        `VTX GPIO ${state.vtxUartPin}`,
        `Pulse ${state.filteredPulse}`,
        `ESP-NOW ${state.espNowEnabled ? 'on' : 'off'}`
      ].forEach((text) => {
        const chip = document.createElement('div');
        chip.className = 'chip';
        chip.textContent = text;
        chips.appendChild(chip);
      });

      setStatus(`Current table: ${state.selectedVtxPath}\nLast mapped slot: ${state.lastMappedIndex}\nTransport ready: ${state.transportReady ? 'yes' : 'no'}`);
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

    async function loadTableJson(path = '') {
      const url = path ? `/api/vtx-table?path=${encodeURIComponent(path)}` : '/api/vtx-table';
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
      setStatus(`Loaded ${data.path || 'active table'} into editor`, 'JSON ready');
      return data;
    }

    async function saveTableJson(selectAfterSave) {
      const body = new URLSearchParams();
      body.set('path', savePathInput.value || 'custom_vtx.json');
      body.set('json', jsonEditor.value);
      body.set('select', selectAfterSave ? '1' : '0');

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
      const body = new URLSearchParams(new FormData(configForm));
      const response = await fetch('/api/config', { method: 'POST', body });
      const data = await response.json();
      setStatus(data.message || 'Configuration saved', data.restartRequired ? 'Restarting' : 'Saved');
      await loadState();
    });

    uploadForm.addEventListener('submit', async (event) => {
      event.preventDefault();
      const body = new FormData(uploadForm);
      const response = await fetch('/api/upload', { method: 'POST', body });
      const data = await response.json();
      setStatus(data.message || 'Upload processed', data.ok ? 'Upload complete' : 'Upload failed');
      await loadState();
      uploadForm.reset();
    });

    loadJsonButton.addEventListener('click', async () => {
      await loadTableJson(jsonTableSelect.value);
    });

    loadActiveButton.addEventListener('click', async () => {
      await loadTableJson('');
    });

    jsonForm.addEventListener('submit', async (event) => {
      event.preventDefault();
      await saveTableJson(false);
    });

    selectSavedButton.addEventListener('click', async () => {
      await saveTableJson(true);
    });

    loadState().then(() => loadTableJson('')).catch((error) => {
      setStatus(error.message || String(error), 'Error');
    });
  </script>
</body>
</html>
)html";
