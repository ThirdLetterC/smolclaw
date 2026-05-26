# SmolClaw WASM Tool Plugin Examples

These examples target the current raw WAMR plugin ABI. They are small tool
plugins, not WebAssembly Component Model modules.

Build SmolClaw with the WAMR host first:

```sh
cmake -S . -B build/wamr \
  -DSC_DEPS_PROVIDER=auto \
  -DSC_ENABLE_PLUGINS=ON \
  -DSC_ENABLE_WASM_PLUGINS=ON
cmake --build build/wamr
```

Each plugin must export:

- `sc_plugin_guest_abi_version`
- `sc_plugin_init`
- `sc_plugin_shutdown`
- one or more invoke functions named by `sc_host_register_tool`

The host imports are from module `env`:

- `sc_host_register_tool(spec_json_ptr, spec_json_len, invoke_name_ptr, invoke_name_len)`
- `sc_host_set_tool_result(success, output_ptr, output_len)`
- `sc_host_set_status(code, key_ptr, key_len)`

## C Guest

```sh
cd examples/wasm-plugins/c-tool
make
sed "s|@ARTIFACT_PATH@|$(pwd)/build/sc_echo_c.wasm|" manifest.template.json > build/manifest.json
```

The C example uses `clang --target=wasm32-wasip1` with no libc entry point. Set
`WASM_CC=clang-22` or another compiler name if your default `clang` cannot find
its matching `wasm-ld`.

## Rust Guest

```sh
cd examples/wasm-plugins/rust-tool
rustup target add wasm32-unknown-unknown
cargo build --release --target wasm32-unknown-unknown
mkdir -p build
cp target/wasm32-unknown-unknown/release/sc_echo_rust.wasm build/sc_echo_rust.wasm
sed "s|@ARTIFACT_PATH@|$(pwd)/build/sc_echo_rust.wasm|" manifest.template.json > build/manifest.json
```

The Rust example is `#![no_std]` and imports the same raw hostcalls.

## Loading

When embedding SmolClaw, create a plugin host with:

- `SC_PLUGIN_PERMISSION_TOOL` in `allowed_permissions`
- `wasm_enabled = true`
- plugin roots/extensions that allow the generated `.wasm`

Then parse the generated manifest and call `sc_plugin_load_path`.
