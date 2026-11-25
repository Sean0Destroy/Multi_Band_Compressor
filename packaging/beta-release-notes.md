**Beta test build. Not signed, not finished.** It may crash your DAW or sound wrong, and settings can change between betas, so sessions saved with one beta may not reload the same in the next. Don't use it on a project you can't afford to lose.

| Download | For | Formats |
|---|---|---|
| `...-windows-x64-setup.exe` | Windows 10 / 11, 64-bit | VST3, CLAP |
| `...-macos-arm64.pkg` | Mac with Apple Silicon (M1 or newer). No Intel Macs. | AU, VST3, CLAP |

**Windows:** close your DAW and run the `.exe`. Because it isn't signed, Windows shows "Windows protected your PC": click **More info**, then **Run anyway**. Pick VST3, CLAP or both. Uninstall from **Settings > Apps > Installed apps**.

**Mac:** quit your DAW and open the `.pkg`. Because it isn't signed, macOS says it can't check it: click **Done**, then go to **System Settings > Privacy & Security** and click **Open Anyway**. Tick AU, VST3 and/or CLAP. To uninstall, open `/Library/Application Support/Tone Legion/Multiband` and double-click **Uninstall Tone Legion Multiband.command**.

Full tester notes, including where the plugins go and how to report a bug, are installed next to the uninstaller and are in `docs/beta/TESTER-NOTES.md` in the repository.

All rights reserved. Please don't redistribute this build.
