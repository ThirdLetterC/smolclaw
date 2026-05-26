# SmolClaw Service Management

`smolclaw service` manages local service descriptors without hiding privileged host operations. The CLI supports `status`, `dry-run`, `install`, `uninstall`, `start`, `stop`, and `restart`. `install` writes the platform descriptor; start/stop/restart print the host command to run. Set `SMOLCLAW_SERVICE_DRY_RUN=1` to preview install or uninstall without changing files.

The managed process is `smolclaw daemon`. It reads `$SMOLCLAW_CONFIG` when set, otherwise `smolclaw.toml`, and it uses `$SMOLCLAW_WORKSPACE` when set, otherwise the `workspace/` directory beside the config file.

## systemd

On Linux, `smolclaw service install` writes:

```text
$XDG_CONFIG_HOME/systemd/user/smolclaw.service
```

If `XDG_CONFIG_HOME` is unset, it uses:

```text
$HOME/.config/systemd/user/smolclaw.service
```

After install, enable it explicitly:

```sh
systemctl --user daemon-reload
systemctl --user enable --now smolclaw.service
systemctl --user status smolclaw.service
```

## launchctl

On macOS, `smolclaw service install` writes:

```text
$HOME/Library/LaunchAgents/com.thirdletterc.smolclaw.plist
```

Load and inspect it explicitly:

```sh
launchctl bootstrap gui/$(id -u) "$HOME/Library/LaunchAgents/com.thirdletterc.smolclaw.plist"
launchctl print gui/$(id -u)/com.thirdletterc.smolclaw
```

Unload it with:

```sh
launchctl bootout gui/$(id -u) "$HOME/Library/LaunchAgents/com.thirdletterc.smolclaw.plist"
```

## Tests

The CTest suite covers `smolclaw service status`, `dry-run`, `install`, and `uninstall` with `SMOLCLAW_SERVICE_DRY_RUN=1`, so CI verifies command parsing and descriptor planning without mutating the host service manager.
