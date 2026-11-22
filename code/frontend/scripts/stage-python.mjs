// Puts the Python the installer carries into src-tauri/pyenv/python/ (tauri.conf.json: bundle.resources -> <install folder>/python): the embeddable Python of python.org and the `mcp`
// package with the packages it needs (pip, for that same Python version, wheels only), so that Claude Desktop can start mcp/mcp_server.py on a PC that has no Python at all.
// `tauri build` runs this through beforeBuildCommand. The first run needs the internet and a Python with pip on this PC (it only downloads the wheels); a later run sees the stamp
// and goes on at once. It stops the build instead of making an installer whose MCP server cannot start.
import { execFileSync, spawnSync } from "node:child_process";
import { existsSync, mkdirSync, readdirSync, readFileSync, rmSync, statSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const PYTHON = "3.12.10"; // (the embeddable build; the wheels below are chosen for the same version)
const MCP = "1.27.2"; // (the version the MCP server was tested with)

const frontend = join(dirname(fileURLToPath(import.meta.url)), "..");
const pyenv = join(frontend, "src-tauri", "pyenv");
const target = join(pyenv, "python");
const exe = join(target, "python.exe");
const sitePackages = join(target, "Lib", "site-packages");
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
