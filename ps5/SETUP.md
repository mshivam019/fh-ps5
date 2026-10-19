# Forza Horizon: controls and settings

See [INSTALL.md](../INSTALL.md) for current setup, manual copying, optional FTP
and validation instructions. The setup v1.2.0 update keeps the v0.1.0 app unchanged.

## Controls

| DualSense | Xbox 360 |
| --- | --- |
| Cross / Circle / Square / Triangle | A / B / X / Y |
| L1 / R1, L2 / R2 | LB / RB, LT / RT |
| Options | Start |
| Touchpad press | Back |
| L3 / R3, D-pad, sticks | as on the 360 |

Every connected DualSense drives player 1.

## Settings

`forza.toml` in the app folder. Notable keys:

- `readback_resolve = "some"`: car thumbnails for the garage.
- `present_frame_pacing = true`: frames timed to the display refresh.
- `deferred_overlapped_delay_ms = 16`: emulated latency of asynchronous
  operations such as save mounts (was 100).
- `shared_memory_dynamic_vertices = false`: content comparison for rewritten
  vertex data; measured without benefit on this game, kept available.
- `ps5_long_frame_log = false`: per-frame diagnostics in the runtime log.
