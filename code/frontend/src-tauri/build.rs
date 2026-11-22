// tauri-build copies every file of bundle.resources (tauri.conf.json) next to the app and stops when one is missing. The engine and its runtime DLLs are only
// there after `npm run stage-engine` (scripts/stage-engine.mjs, which `tauri build` runs first and which refuses to go on without the real files); for
// `cargo tauri dev` and a plain `cargo build` an empty place-holder is made, so that development needs no installer step. main.rs does not use a file under 1 MB
// as the bundled engine (bundled_engine_path), and a debug build never uses the bundle at all.
fn main() {
    let engine = std::path::Path::new("engine");
    let _ = std::fs::create_dir_all(engine);
    for name in ["engine_server.exe", "msvcp140.dll", "vcruntime140.dll", "vcruntime140_1.dll"] {
        let path = engine.join(name);
        if !path.exists() {
            let _ = std::fs::write(&path, b"");
        }
    }
    // (the Python of the installer, scripts/stage-python.mjs: only a place-holder for development, so that the resource folder exists)
    let python = std::path::Path::new("pyenv").join("python");
    let _ = std::fs::create_dir_all(&python);
    if !python.join("python.exe").exists() {
        let _ = std::fs::write(python.join("python.exe"), b"");
    }
    println!("cargo:rerun-if-changed=engine");
    tauri_build::build()
}
