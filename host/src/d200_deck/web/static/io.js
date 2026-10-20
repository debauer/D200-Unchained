// Header and footer controls that talk to the host directly: export, import, start brightness.

import { api } from "./api.js";
import { serializable } from "./autosave.js";

const $ = (id) => document.getElementById(id);

function download(name, text) {
  const url = URL.createObjectURL(new Blob([text], { type: "application/json" }));
  const a = Object.assign(document.createElement("a"), { href: url, download: name });
  a.click();
  setTimeout(() => URL.revokeObjectURL(url), 1000);
}

// ctx: { flush(), config(), setConfig(cfg), reload(), toast(message, isError) }
export function bindIO(ctx) {
  $("export").addEventListener("click", async () => {
    await ctx.flush().catch((e) => ctx.toast(`Unsaved edits are not in the export: ${e.message}`, true));
    const date = new Date().toISOString().slice(0, 10);
    download(`${date}-d200-config.json`, JSON.stringify(await api.config(), null, 2));
  });
  $("import-file").addEventListener("change", async (e) => {
    const file = e.target.files[0];
    e.target.value = "";
    if (!file) return;
    const wasInert = document.body.inert;
    document.body.inert = true;
    try {
      const imported = JSON.parse(await file.text());
      await ctx.flush();
      await api.putConfig(imported);
      await ctx.reload();
      ctx.toast("Config imported (key images are not part of the file)");
    } catch (err) {
      ctx.toast(`Import failed: ${err.message}`, true);
    } finally {
      document.body.inert = wasInert;
    }
  });
  let timer;
  $("brightness").addEventListener("input", (e) => {
    clearTimeout(timer);
    timer = setTimeout(async () => {
      ctx.setConfig({ ...ctx.config(), brightness: +e.target.value });
      await api.putConfig(serializable(ctx.config())).catch((err) => ctx.toast(err.message, true));
    }, 300);
  });
}
