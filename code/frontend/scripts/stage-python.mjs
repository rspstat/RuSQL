// Puts the Python the installer carries into src-tauri/pyenv/python/ (tauri.conf.json: bundle.resources -> <install folder>/python): the embeddable Python of python.org and the `mcp`
// package with the packages it needs (pip, for that same Python version, wheels only), so that Claude Desktop can start mcp/mcp_server.py on a PC that has no Python at all.
// `tauri build` runs this through beforeBuildCommand. The first run needs the internet and a Python with pip on this PC (it only downloads the wheels); a later run sees the stamp
// and goes on at once. It stops the build instead of making an installer whose MCP server cannot start.
import { execFileSync, spawnSync } from "node:child_process";
import { createHash } from "node:crypto";
import { existsSync, mkdirSync, readdirSync, readFileSync, rmSync, statSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const PYTHON = "3.12.10"; // (the embeddable build; the wheels below are chosen for the same version)
const MCP = "1.27.2"; // (the version the MCP server was tested with)

const frontend = join(dirname(fileURLToPath(import.meta.url)), "..");
const pyenv = join(frontend, "src-tauri", "pyenv");
const target = join(pyenv, "python");
const darwin = process.platform === "darwin"; // (macOS: a relocatable CPython of python-build-standalone instead of python.org's embeddable zip)
const exe = darwin ? join(target, "bin", "python3") : join(target, "python.exe");
const sitePackages = darwin ? join(target, "lib", `python${PYTHON.split(".").slice(0, 2).join(".")}`, "site-packages") : join(target, "Lib", "site-packages");
const stamp = join(target, ".rusql-stage");
const want = `python ${PYTHON}, mcp ${MCP}`;

function fail(message) {
  console.error(`stage-python: ${message}`);
  process.exit(1);
}

function run(command, args, options = {}) {
  const r = spawnSync(command, args, { encoding: "utf8", ...options });
  return { ok: r.status === 0, out: `${r.stdout ?? ""}${r.stderr ?? ""}`.trim() };
}

function countFiles(dir) {
  let n = 0;
  let bytes = 0;
  for (const entry of readdirSync(dir, { withFileTypes: true })) {
    const path = join(dir, entry.name);
    if (entry.isDirectory()) {
      const sub = countFiles(path);
      n += sub.n;
      bytes += sub.bytes;
    } else {
      n += 1;
      bytes += statSync(path).size;
    }
  }
  return { n, bytes };
}

// the Python the installer asked for is staged already (the stamp says so, and the files are really there)
if (existsSync(stamp) && readFileSync(stamp, "utf8").trim() === want && existsSync(exe) && statSync(exe).size > 10_000 && existsSync(join(sitePackages, "mcp"))) {
  console.log(`stage-python: ${want} is staged already`);
  process.exit(0);
}

rmSync(target, { recursive: true, force: true });
mkdirSync(target, { recursive: true });

if (darwin) {
  // python-build-standalone (astral-sh): the file name and its SHA-256 are pinned (the SHA256SUMS of that release); nothing of it is run before the hash matches.
  const PBS = "20250409";
  const sums = {
    aarch64: "0be1fe0b35a4d3c382141764ef16ed3b8cc2b4620b657f678daa7b7f8df39699",
    x86_64: "ad3bef94b6054adcf8e0a47886e21b00dfc6a37f22eea229cf0f8725bd0e1023",
  };
  const arch = process.arch === "arm64" ? "aarch64" : process.arch === "x64" ? "x86_64" : fail(`unsupported CPU: ${process.arch}`);
  const name = `cpython-${PYTHON}+${PBS}-${arch}-apple-darwin-install_only_stripped.tar.gz`;
  const archive = join(pyenv, name);
  const sha256 = (path) => createHash("sha256").update(readFileSync(path)).digest("hex");
  if (!existsSync(archive) || sha256(archive) !== sums[arch]) {
    const url = `https://github.com/astral-sh/python-build-standalone/releases/download/${PBS}/${encodeURIComponent(name)}`;
    console.log(`stage-python: downloading ${url}`);
    const response = await fetch(url);
    if (!response.ok) fail(`${url} answered ${response.status}`);
    writeFileSync(archive, Buffer.from(await response.arrayBuffer()));
  }
  if (sha256(archive) !== sums[arch]) fail(`${name} is not the file this script was written for (SHA-256 differs)`);
  const untar = run("tar", ["-xzf", archive, "-C", pyenv]); // (the archive holds a folder `python/`)
  if (!untar.ok || !existsSync(exe)) fail(`could not unpack ${archive}\n${untar.out}`);

  // the mcp package, into the Python's own site-packages (no .pyc: nothing may be written into the folder the app bundle will carry)
  const env = { ...process.env, PYTHONDONTWRITEBYTECODE: "1" };
  console.log(`stage-python: pip install mcp==${MCP}`);
  const install = run(exe, ["-B", "-m", "pip", "install", "--no-compile", "--only-binary=:all:", "--upgrade", `mcp==${MCP}`], { env });
  if (!install.ok) fail(`pip install failed\n${install.out.split("\n").slice(-12).join("\n")}`);
  // (pip's launcher scripts in bin/ name the build machine's path in their first line: of no use in an app bundle)
  for (const entry of readdirSync(join(target, "bin"))) {
    if (!entry.startsWith("python")) rmSync(join(target, "bin", entry), { force: true });
  }

  // every Mach-O file of it gets an ad-hoc signature (Apple Silicon runs nothing unsigned; a hash of the file is all an ad-hoc signature is)
  let signedFiles = 0;
  const signTree = (dir) => {
    for (const entry of readdirSync(dir, { withFileTypes: true })) {
      const path = join(dir, entry.name);
      if (entry.isSymbolicLink()) continue;
      if (entry.isDirectory()) { signTree(path); continue; }
      const head = readFileSync(path).subarray(0, 4).toString("hex");
      if (!["cffaedfe", "feedfacf", "cafebabe", "bebafeca"].includes(head)) continue;
      const signed = run("codesign", ["--force", "--sign", "-", path]);
      if (!signed.ok) fail(`codesign failed for ${path}\n${signed.out}`);
      signedFiles += 1;
    }
  };
  signTree(target);

  const probe = run(exe, ["-B", "-c", "from mcp.server.fastmcp import FastMCP"], { cwd: tmpdir(), env });
  if (!probe.ok) fail(`the staged Python cannot import mcp.server.fastmcp\n${probe.out}`);

  writeFileSync(stamp, `${want}\n`);
  const { n, bytes } = countFiles(target);
  console.log(`stage-python: ${want} staged in ${target} (${n} files, ${(bytes / 1024 / 1024).toFixed(1)} MB, ${signedFiles} signed)`);
  process.exit(0);
}

// 1. the embeddable Python of python.org
const zip = join(pyenv, `python-${PYTHON}-embed-amd64.zip`);
if (!existsSync(zip) || statSync(zip).size < 5_000_000) {
  const url = `https://www.python.org/ftp/python/${PYTHON}/python-${PYTHON}-embed-amd64.zip`;
  console.log(`stage-python: downloading ${url}`);
  const response = await fetch(url);
  if (!response.ok) fail(`${url} answered ${response.status}`);
  writeFileSync(zip, Buffer.from(await response.arrayBuffer()));
}
const unzip = run(join(process.env.SystemRoot ?? "C:\\Windows", "System32", "tar.exe"), ["-xf", zip, "-C", target]); // (Windows' own bsdtar; a GNU tar on the PATH reads C: as a host)
if (!unzip.ok || !existsSync(exe)) fail(`could not unpack ${zip}\n${unzip.out}`);

// 2. it must be the real one: python.exe is signed by the Python Software Foundation
const signature = run("powershell", ["-NoProfile", "-Command", `$s = Get-AuthenticodeSignature '${exe}'; "$($s.Status)|$($s.SignerCertificate.Subject)"`]);
if (!signature.out.startsWith("Valid|") || !signature.out.includes("Python Software Foundation")) fail(`python.exe is not signed by the Python Software Foundation: ${signature.out}`);

// 3. the mcp package for that Python: the wheels are fetched by the pip of this PC (nothing is run from the download)
const pip = [["py", "-3"], ["python"]].find((cmd) => run(cmd[0], [...cmd.slice(1), "-m", "pip", "--version"]).ok);
if (!pip) fail("no Python with pip on this PC to fetch the mcp wheels (py -3 -m pip / python -m pip)");
const [major, minor] = PYTHON.split(".");
mkdirSync(sitePackages, { recursive: true });
console.log(`stage-python: pip install mcp==${MCP} for Python ${major}.${minor}`);
const install = run(pip[0], [...pip.slice(1), "-m", "pip", "install", "--target", sitePackages, "--platform", "win_amd64", "--python-version", `${major}.${minor}`, "--implementation", "cp",
  "--only-binary=:all:", "--no-compile", "--upgrade", `mcp==${MCP}`]);
if (!install.ok) fail(`pip install failed\n${install.out.split("\n").slice(-12).join("\n")}`);
rmSync(join(sitePackages, "bin"), { recursive: true, force: true });

// 4. the embeddable Python reads python3NN._pth instead of the environment: add the packages, and `import site` (pywin32 puts its folders on the path through a .pth file)
const pth = readdirSync(target).find((name) => name.endsWith("._pth"));
if (!pth) fail("no python3NN._pth in the embeddable Python");
const lines = readFileSync(join(target, pth), "utf8").split(/\r?\n/).filter((line) => line.trim() !== "" && !line.startsWith("#") && line.trim() !== "import site");
writeFileSync(join(target, pth), [...lines, "Lib\\site-packages", "import site", ""].join("\r\n"));

// 5. does the MCP server start on it? (-B: no .pyc files are written into the folder the installer will list)
const probe = run(exe, ["-B", "-c", "from mcp.server.fastmcp import FastMCP"], { cwd: tmpdir() });
if (!probe.ok) fail(`the staged Python cannot import mcp.server.fastmcp\n${probe.out}`);

writeFileSync(stamp, `${want}\n`);
const { n, bytes } = countFiles(target);
console.log(`stage-python: ${want} staged in ${target} (${n} files, ${(bytes / 1024 / 1024).toFixed(1)} MB)`);
