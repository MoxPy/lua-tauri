document.addEventListener("DOMContentLoaded", () => {
  const originEl = document.getElementById("origin-indicator");
  if (originEl) {
    originEl.textContent = "Origin: " + window.location.origin;
  }

  let frames = 0;
  let lastTime = performance.now();
  function countFps() {
    frames = frames + 1;
    const now = performance.now();
    const elapsed = now - lastTime;
    if (elapsed >= 500) {
      const currentFps = Math.round((frames / elapsed) * 1000);
      const fpsEl = document.getElementById("fps");
      if (fpsEl) {
        fpsEl.textContent = currentFps + " FPS";
      }
      frames = 0;
      lastTime = now;
    }
    requestAnimationFrame(countFps);
  }
  requestAnimationFrame(countFps);

  async function testStreamingChannel() {
    const btn = document.getElementById("btn-stream");
    const out = document.getElementById("log-stream");
    if (!btn || !out) return;
    btn.disabled = true;
    out.textContent = "Apertura canale stream...\n";

    const channel = new window.__TAURI__.Channel((chunk) => {
      out.textContent += `[TICK ${chunk.remaining}] ${chunk.message}\n`;
      out.scrollTop = out.scrollHeight;
    });

    try {
      const res = await window.__TAURI__.invoke("stream_countdown", { count: 5, onEvent: channel });
      out.textContent += `\nRisultato finale: ${JSON.stringify(res)}`;
    } catch (err) {
      out.textContent += `\nErrore stream: ${JSON.stringify(err)}`;
    } finally {
      btn.disabled = false;
    }
  }

  async function testIsolates() {
    const btn = document.getElementById("btn-isolates");
    const out = document.getElementById("log-isolates");
    if (!btn || !out) return;
    btn.disabled = true;
    out.textContent = "Elaborazione concorrente sui worker...";

    try {
      const promises = [1, 2, 3, 4].map(() =>
        window.__TAURI__.core.invoke("parallel_workload", { limit: 1200000 })
      );
      const results = await Promise.all(promises);
      out.textContent = JSON.stringify(results, null, 2);
    } catch (err) {
      out.textContent = "Errore: " + JSON.stringify(err);
    } finally {
      btn.disabled = false;
    }
  }

  async function testStateRead() {
    const out = document.getElementById("log-state");
    if (!out) return;
    try {
      const res = await window.__TAURI__.core.invoke("get_shared_data");
      out.textContent = JSON.stringify(res, null, 2);
    } catch (err) {
      out.textContent = "Errore: " + JSON.stringify(err);
    }
  }

  async function testStateBurst() {
    const out = document.getElementById("log-state");
    if (!out) return;
    out.textContent = "Invio 10 richieste parallele allo Shared State Store...";
    try {
      const promises = [];
      for (let i = 0; i < 10; i++) {
        promises.push(window.__TAURI__.core.invoke("increment_counter"));
      }
      const res = await Promise.all(promises);
      out.textContent = "Completate 10 scritture atomiche:\n" + JSON.stringify(res, null, 2);
    } catch (err) {
      out.textContent = "Errore: " + JSON.stringify(err);
    }
  }

  async function openSettings() {
    const out = document.getElementById("log-sec");
    if (!out) return;
    out.textContent = "Apertura finestra in corso...";
    try {
      const base = window.location.origin !== "null" ? window.location.origin : "tauri://localhost";
      const targetUrl = base.replace(/\/$/, "") + "/settings.html";
      const winLabel = "settings_" + Date.now();

      await window.__TAURI__.core.invoke("tauri:window:create", {
        label: winLabel,
        title: "Impostazioni",
        url: targetUrl,
        width: 500,
        height: 440,
        resizable: true
      });

      out.textContent = "Finestra aperta (" + winLabel + ") su:\n" + targetUrl;
    } catch (err) {
      out.textContent = "Errore apertura finestra:\n" + JSON.stringify(err);
    }
  }

  async function testSecurityTamper() {
    const out = document.getElementById("log-sec");
    if (!out) return;
    try {
      const payload = JSON.stringify(["instant_ping", "{}", "TOKEN_FALSO_MANOMESSO"]);
      await window.__TAURI_RAW_INVOKE__(payload);
      out.textContent = "ATTENZIONE: La chiamata manomessa non è stata bloccata.";
    } catch (err) {
      out.textContent = "Bloccato con successo da C++:\n" + JSON.stringify(err);
    }
  }

  const btnIsolates = document.getElementById("btn-isolates");
  if (btnIsolates) btnIsolates.addEventListener("click", testIsolates);

  const btnStateRead = document.getElementById("btn-state-read");
  if (btnStateRead) btnStateRead.addEventListener("click", testStateRead);

  const btnStateBurst = document.getElementById("btn-state-burst");
  if (btnStateBurst) btnStateBurst.addEventListener("click", testStateBurst);

  const btnOpenSettings = document.getElementById("btn-open-settings");
  if (btnOpenSettings) btnOpenSettings.addEventListener("click", openSettings);

  const btnSecTamper = document.getElementById("btn-sec-tamper");
  if (btnSecTamper) btnSecTamper.addEventListener("click", testSecurityTamper);

  const btnStream = document.getElementById("btn-stream");
  if (btnStream) btnStream.addEventListener("click", testStreamingChannel);
});
