# Trusted Script Tool Plugins

SmolClaw can host trusted in-process Python tool plugins when the corresponding
build flag and host option are enabled.

These runtimes are not sandboxes. Use WASM or a process boundary for untrusted
plugins. Python code runs in the SmolClaw process and can use its own runtime
APIs unless a future host version adds stronger isolation.

## Build

```sh
cmake -S . -B build/python -DSC_DEPS_PROVIDER=auto -DSC_ENABLE_PLUGINS=ON -DSC_ENABLE_PYTHON_PLUGINS=ON
cmake --build build/python
```

## Manifest

Python plugins use a `.py` artifact:

```json
{
  "name": "example-python-plugin",
  "version": "1.0.0",
  "abi_major": 0,
  "capabilities": ["python", "tool"],
  "requested_permissions": ["tool"],
  "entry_artifact": "/absolute/path/to/example_plugin.py"
}
```

## Python API

```python
def sc_plugin_init(host):
    host.register_tool({
        "name": "python_echo",
        "description": "Echo text from JSON args.",
        "input_schema": {"type": "object"},
        "risk": 0,
        "capability_category": 0,
        "side_effect": 0,
        "default_autonomy": 0,
    }, invoke)


def invoke(args, call):
    return {"success": True, "output": args.get("message", "")}


def sc_plugin_shutdown():
    pass
```
