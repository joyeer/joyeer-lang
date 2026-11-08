# lib

Everything here is written in Joyeer (plus the C each part needs for its native side).

- `Foundation/`, `System/`, `Compression/` are **standard modules**: programs
  `import` them, and the toolchain ships them as source under `<toolchain>/lib`
  (staged by `scripts/cmake/stageStandardLibrary.cmake`).
- `Runtime/` is **not a module** (it has no `project.toml` and is never staged).
  It is the runtime compiled into every program: `core/` holds the Joyeer sources
  built into the `JoyeerRuntime` archive (core units, scalars only), and
  `include/joyeer/` holds the C headers that declare its ABI for native libraries,
  included as `joyeer/runtime.h` and `joyeer/native.h`.
