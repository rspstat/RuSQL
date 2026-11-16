// Puts the engine the installer carries into src-tauri/engine/ (tauri.conf.json: bundle.resources): the Release engine_server.exe of code/build and the three
// Visual C++ runtime DLLs it imports (MSVCP140, VCRUNTIME140, VCRUNTIME140_1), which a PC that never installed the redistributable does not have -- next to the
// exe they are found first. `tauri build` runs this through beforeBuildCommand; it stops the build instead of making an installer that cannot start its engine.
import { copyFileSync, existsSync, mkdirSync, statSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const frontend = join(dirname(fileURLToPath(import.meta.url)), "..");
const target = join(frontend, "src-tauri", "engine");
const engine = join(frontend, "..", "build", "backend", "server", "Release", "engine_server.exe");
const system32 = join(process.env.SystemRoot ?? "C:\\Windows", "System32");
const runtime = ["msvcp140.dll", "vcruntime140.dll", "vcruntime140_1.dll"];

function fail(message) {
  console.error(`stage-engine: ${message}`);
  process.exit(1);
}

if (!existsSync(engine)) {
  fail(`the Release engine is not built: ${engine}\n  build it first: cmake --build code/build --config Release --target engine_server`);
}
mkdirSync(target, { recursive: true });

const files = [[engine, "engine_server.exe"], ...runtime.map((name) => [join(system32, name), name])];
for (const [from, name] of files) {
  if (!existsSync(from)) fail(`missing ${from}`);
  copyFileSync(from, join(target, name));
  const size = statSync(join(target, name)).size;
  if (size < 20_000) fail(`${name} is only ${size} bytes`);
  console.log(`stage-engine: ${name} (${(size / 1024 / 1024).toFixed(2)} MB)`);
}
