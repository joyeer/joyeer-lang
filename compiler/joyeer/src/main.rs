//! The `joyeer` command-line compiler. Intentionally thin: all logic lives in
//! `joyeer_driver` (mirrors `compiler/rustc`'s `fn main`).

fn main() -> std::process::ExitCode {
    joyeer_driver::main()
}
