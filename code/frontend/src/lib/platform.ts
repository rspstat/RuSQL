// What differs between Windows (WebView2) and macOS (WKWebView). Windows keeps doing what it did: a link that downloads a Blob, window.confirm, window.alert. The macOS WebView
// downloads nothing from a link and shows no JavaScript dialogs (confirm answers "no", alert says nothing), so there the app asks Rust, which writes the file into the Downloads
// folder (and Finder shows it) and shows the system's own dialog.

import { invoke } from "@tauri-apps/api/core";

export const IS_MAC = typeof navigator !== "undefined" && /Mac/i.test(navigator.platform);

// Saves `text` as the file `name`: a download on Windows, a file in the Downloads folder on macOS.
export async function saveTextFile(name: string, text: string, mime: string): Promise<void> {
  if (IS_MAC) {
    try {
      await invoke("save_download", { name, content: text });
    } catch (e) {
      await alertBox(String(e));
    }
    return;
  }
  const blob = new Blob([text], { type: mime });
  const url = URL.createObjectURL(blob);
  const a = document.createElement("a");
  a.href = url;
  a.download = name;
  document.body.appendChild(a);
  a.click();
  a.remove();
  URL.revokeObjectURL(url);
}

export async function confirmBox(message: string): Promise<boolean> {
  if (IS_MAC) return invoke<boolean>("native_confirm", { message });
  return window.confirm(message);
}

export async function alertBox(message: string): Promise<void> {
  if (IS_MAC) {
    await invoke("native_alert", { message });
    return;
  }
  window.alert(message);
}
