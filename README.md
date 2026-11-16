# plug-in-collection

A collection of vibe-coded effects that I wanted and thought someone else might too, for
**Akai MPC** and **Akai Force** standalone hardware. By Sure Eel (shown on the device as
manufacturer **sureeel**).

| Plugin | What it is |
|---|---|
| [LUFS Master](#lufs-master) | Loudness meter, leveler and transparent true-peak limiter |

More plugins are on the way.

---

## LUFS Master

A loudness meter, live leveler and transparent true-peak limiter for mastering to streaming or
broadcast loudness targets (ITU-R BS.1770 / EBU R128). Measure a song once, lock the gain, and
bounce it at the target loudness, with true peaks held under the ceiling and no colouring of
the sound.

![LUFS Master touch panel](docs/images/lufs-master-panel.png)

*The MASTER page, rendered from the plugin's own skin files with real readouts captured while it
measured a test signal (fonts are approximate). A second page, METERS, shows loudness over time
and every meter as a number.*

### Install

1. Download the zip for your hardware from the
   [latest release](https://github.com/SureEel/plug-in-collection/releases/latest):
   - **`...-force-armv7.zip`**: 32-bit hardware, including the **Akai Force**.
   - **`...-mpc-aarch64.zip`**: 64-bit hardware, including the **MPC Key 37 Gen 2**.

   A build for the wrong type is silently ignored by the device. If you are not sure which type
   yours is, install both: they hold the same plugin and each device uses the build it can run.
2. Unzip it. Copy the folder **`sureeel - VST - LUFS Master`** into a folder named
   **`VST2 Plugins`** at the top of a drive your MPC / Force uses (internal drive, SD card or
   USB). Create `VST2 Plugins` if it does not exist.
3. On the device: **Preferences → Enhancements (Beta)**, switch on **Enable Enhancements** and
   **External Plugin Support**. Needs **MPC OS / Force OS 3.10.1 or later**.
4. Restart the device. Insert **LUFS Master** (manufacturer **sureeel**) as an insert effect,
   last on the track or master you want to deliver.

### Use

1. Pick a **Target**: -14 LUFS (streaming), -16 (Apple Music / podcasts), -23 (EBU R128),
   -24 (ATSC A/85) or Custom. Leave **Ceiling** at -1.0 dBTP unless you need another.
2. Set **Mode** to **Measure** and play the whole song once. A slow leveler rides the gain so
   you already hear it near the target.
3. Set **Mode** to **Locked**. The plugin calculates one fixed gain and shows the **Predicted**
   output loudness.
4. Press **Refresh**, then bounce. **Output integrated loudness** should read the target at the end.

| Control | What it does |
|---|---|
| Mode | **Bypass** (bit-exact), **Measure** (meters and leveler), **Locked** (fixed gain; the limiter only catches peaks) |
| Target / Custom LUFS | The loudness to reach; Custom allows -30 to -6 LUFS |
| Ceiling | True-peak ceiling of the limiter, -3 to 0 dBTP |
| Reset | Clears the measurement, the lock and all readouts |
| Refresh | Clears the readouts only; keeps the measurement and the lock |

Touch any meter or label to see an explanation in the help window. Details, measured accuracy
and the test method are in [plugins/LUFS Master/README.md](plugins/LUFS%20Master/README.md).

---

## Build from source

The plugins are built with the MPC Plugin Builder SDK, included in this repository. You need
Python 3 (Windows, macOS or Linux); the first build downloads a compiler into `.sdk-tools/`.

```
python tools/build.py "plugins/LUFS Master"
```

The finished plugin folder appears in `plugins/LUFS Master/build/`. The touch panel is generated
by `plugins/LUFS Master/skin_src/make_skin.py` (run it after a layout change, before `build.py`).
See [GETTING_STARTED.md](GETTING_STARTED.md) and [docs/SDK-README.md](docs/SDK-README.md) for the
SDK itself.

## Licence

- The plugins (everything in `plugins/`) are MIT-licensed, © 2026 Sure Eel: see [LICENSE](LICENSE).
- The MPC Plugin Builder SDK (everything else) is MIT-licensed, © 2026 inMusic Brands, Inc.: see
  [LICENSE-SDK](LICENSE-SDK). One small addition to it, an optional `programLoaded()` hook in
  `framework/mpc_plugin.h`, is used by an upcoming plugin and does nothing for the others.
