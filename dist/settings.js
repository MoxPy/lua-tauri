document.addEventListener("DOMContentLoaded", () => {
  async function saveSettings() {
    const userEl = document.getElementById("username");
    const themeEl = document.getElementById("theme");
    const notifEl = document.getElementById("notifications");
    const statusEl = document.getElementById("status");

    const prefs = {
      username: userEl ? userEl.value : "",
      theme: themeEl ? themeEl.value : "dark",
      notifications: notifEl ? notifEl.checked : true
    };

    try {
      await window.__TAURI__.core.invoke("tauri:state:set", {
        key: "app_preferences",
        value: prefs
      });

      await window.__TAURI__.event.emit("settings-changed", prefs);
      if (statusEl) {
        statusEl.textContent = "Salvato nello State Store:\n" + JSON.stringify(prefs, null, 2);
      }
    } catch (err) {
      if (statusEl) {
        statusEl.textContent = "Errore durante il salvataggio:\n" + JSON.stringify(err);
      }
    }
  }

  async function closeWindow() {
    try {
      await window.__TAURI__.webviewWindow.getCurrentWebviewWindow().close();
    } catch (err) {
      window.close();
    }
  }

  const btnClose = document.getElementById("btn-close");
  if (btnClose) btnClose.addEventListener("click", closeWindow);

  const btnSave = document.getElementById("btn-save");
  if (btnSave) btnSave.addEventListener("click", saveSettings);
});
