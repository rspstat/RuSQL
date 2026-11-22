// Puts the engine the installer carries into src-tauri/engine/ (tauri.conf.json: bundle.resources): the Release engine_server.exe of code/build and the three
// Visual C++ runtime DLLs it imports (MSVCP140, VCRUNTIME140, VCRUNTIME140_1), which a PC that never installed the redistributable does not have -- next to the
// exe they are found first. `tauri build` runs this through beforeBuildCommand; it stops the build instead of making an installer that cannot start its engine.
import { spawnSync } from "node:child_process";
import { chmodSync, copyFileSync, existsSync, mkdirSync, statSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const darwin = process.platform === "darwin"; // (macOS: engine_server without the Visual C++ runtime, a single-configuration build folder, and an ad-hoc code signature)
const frontend = join(dirname(fileURLToPath(import.meta.url)), "..");
const target = join(frontend, "src-tauri", "engine");
const engine = darwin
  ? join(frontend, "..", "build", "backend", "server", "engine_server")
  : join(frontend, "..", "build", "backend", "server", "Release", "engine_server.exe");
const system32 = join(process.env.SystemRoot ?? "C:\\Windows", "System32");
const runtime = ["msvcp140.dll", "vcruntime140.dll", "vcruntime140_1.dll"];

function fail(message) {
  console.error(`stage-engine: ${message}`);
  process.exit(1);
}

if (!existsSync(engine)) {
  fail(darwin
    ? `the Release engine is not built: ${engine}\n  build it first: cmake -S code -B code/build -DCMAKE_BUILD_TYPE=Release && cmake --build code/build --target engine_server`
    : `the Release engine is not built: ${engine}\n  build it first: cmake --build code/build --config Release --target engine_server`);
}
mkdirSync(target, { recursive: true });

const files = darwin ? [[engine, "engine_server"]] : [[engine, "engine_server.exe"], ...runtime.map((name) => [join(system32, name), name])];
for (const [from, name] of files) {
  if (!existsSync(from)) fail(`missing ${from}`);
  copyFileSync(from, join(target, name));
  const size = statSync(join(target, name)).size;
  if (size < 20_000) fail(`${name} is only ${size} bytes`);
  console.log(`stage-engine: ${name} (${(size / 1024 / 1024).toFixed(2)} MB)`);
}

if (darwin) {
  // a program Apple Silicon runs must carry a signature, even an ad-hoc one (the linker adds one; signing again after the copy makes sure it is valid for this file)
  const path = join(target, "engine_server");
  chmodSync(path, 0o755);
  const signed = spawnSync("codesign", ["--force", "--sign", "-", path], { encoding: "utf8" });
  if (signed.status !== 0) fail(`codesign failed for ${path}\n${signed.stderr}`);
  console.log("stage-engine: engine_server signed (ad-hoc)");
}
