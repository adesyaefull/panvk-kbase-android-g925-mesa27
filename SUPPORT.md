# Support, bug reports, and log collection

This repository publishes an experimental Mesa 27.0.0-devel PanVK build for Mali-G925. A Vulkan API version or extension count does not guarantee conformance or that every feature works. Please query the ICD you are actually testing and report the exact device, driver package, and steps.

## Open a bug report

Use the [bug report form](https://github.com/adesyaefull/panvk-kbase-android-g925-mesa27/issues/new?template=bug_report.yml). Before submitting:

1. Check the [README](./README.md), [CHANGELOG](./CHANGELOG.md), [Vulkan extension inventory](./VULKAN-EXTENSIONS.md), and existing [open issues](https://github.com/adesyaefull/panvk-kbase-android-g925-mesa27/issues).
2. Reproduce once more if practical and write down the exact steps, expected result, actual result, and whether it happens every time.
3. State which ICD was selected (system, bundled, or imported), the ADPKG/file name, and the reported driver version/build. Do not mix results from a different Mesa build or driver.
4. Attach the smallest useful logs and a screenshot/video only if it helps show the failure. If a previous package worked, name that build too.

### Include these details

- Device manufacturer/model, GPU (G925 / `0xd8300010` if applicable), Android version/build, and Kbase/kernel version if known.
- Driver/package version and whether you tested the release ADPKG or a locally imported `.so`.
- Application/game and version; launch mode, relevant settings, and whether the issue occurs with other apps.
- Exact reproduction steps, expected behavior, observed behavior, frequency, and the first error/device-lost line.
- The relevant PanProbe test name/result or Vulkan Info JSON when the report concerns a Vulkan feature/extension.
- A log bundle captured as described below. Mention if a crash caused a tombstone; the app may not have permission to attach the tombstone itself.

Do not file a report that contains only “does not work”; logs and reproducible steps are the fastest way to diagnose it.

## PanProbe: capture Vulkan information and test logs

1. Open **PanProbe** and go to the **Driver** tab. Select the driver you intend to test. When verifying the published ADPKG, select **Imported** and choose that build's ICD `.so`; do not assume the system or bundled ICD is the package under test.
2. Open **Info**, tap **Load**, then tap **Share** to export the raw Vulkan Info JSON. This capture includes device properties, extension names/versions, features, limits, formats, and queues. Attach the JSON when the issue concerns extensions, capabilities, or a mismatch in Vulkan Info.
3. Open **Tests**, run the relevant focused test first (or **Run all** only if appropriate), then note the exact test name and PASS/FAIL/SKIP result.
4. Open **Logs**, select the log for that test or the `vkinfo` query, and tap **Share**. Attach the complete text rather than a cropped error line where possible. Do not tap **Clear** until logs have been shared.

The PanProbe application stores per-run logs under its own private `files/logs/` directory. Its **Logs → Share** action is the recommended way to export these logs without needing privileged access to app data.

## PanPlay: collect a game session bundle

PanPlay automatically collects session diagnostics after a game exits or crashes and opens **Session logs**. You can also open the last session from the Games tab. Review the summary, then tap **Share as ZIP** and attach the archive to the issue.

The ZIP contains a per-run `summary.txt`, `session.json`, `wine-run.log` (Wine stdout/stderr, including DXVK/Mesa output), `mesa-panvk.txt` (filtered PanVK/Kbase/Vulkan lines), `launcher.log`, `xserver.log`, `logcat.txt`, `device.txt`, and `config.txt`; it also includes `dxvk-*.log` and Unity `Player.log` files when present. `device.txt` records the selected ICD and ELF build ID when available. A game launched through PanPlay is best diagnosed with this session bundle because it keeps the logs from the same run together.

## Capture Android logcat with ADB

Use this when an app/driver crash occurs outside PanPlay, when the UI export is unavailable, or when collecting system-side messages. Connect the device with USB debugging enabled and check its serial:

```sh
adb devices -l
```

Start a live log capture on the host:

```sh
adb -s SERIAL logcat -v threadtime > panvk-logcat.txt
```

Replace `SERIAL` with the device serial from `adb devices -l`. Reproduce the issue while the command is running, wait a few seconds after the failure, then stop capture with **Ctrl+C**. Attach `panvk-logcat.txt`. This does not clear existing device logs. If only one device is connected, `adb logcat -v threadtime > panvk-logcat.txt` also works.

For a game launched in PanPlay, prefer the **Share as ZIP** session archive; its per-run Wine/DXVK/Mesa logs are often more useful than global logcat alone. Android may restrict access to tombstones and other processes' private logs, so absence of a tombstone in the export does not prove that no crash occurred.

## Privacy before sharing

Logs may contain device/build fingerprints, application and game paths, usernames, environment variables, controller settings, and unrelated Android log messages. Review the bundle before posting it publicly. Redact personal data, account identifiers, tokens, private URLs, and unrelated app content, but keep the relevant PanVK/Kbase/Vulkan error context and driver/build identifiers. Never upload passwords, access tokens, or private keys.

## Validation references

- [Vulkan extension inventory](./VULKAN-EXTENSIONS.md) — names/spec versions from the Mesa 27.0.0-devel G925 runtime capture.
- [CHANGELOG](./CHANGELOG.md) — curated feature and project history.
- [BCn configuration](./README.md#bcn-texture-emulation) — BC texture path summary.
- [Runtime options](./README.md#runtime-configuration) — environment-variable reference location.
