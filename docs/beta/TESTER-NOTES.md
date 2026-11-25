# Tone Legion Multiband beta: tester notes

Thanks for trying the beta. There are two installers:

- **Windows 10 and 11 (64-bit):** `ToneLegionMultiband-<version>-beta-windows-x64-setup.exe`, with VST3 and CLAP.
- **Mac with Apple Silicon (M1 or newer):** `ToneLegionMultiband-<version>-beta-macos-arm64.pkg`, with AU, VST3
  and CLAP. Intel Macs are not supported in this beta.

If your download is a `.zip`, open it first (Windows: right-click, **Extract All**; Mac: double-click).

## Windows

### Install

1. Close your DAW.
2. Double-click the setup `.exe`.
3. Windows may show **"Windows protected your PC"**. The installer isn't signed yet, so Windows doesn't
   recognise the publisher. Click **More info**, then **Run anyway**. Your browser may also say the file
   "isn't commonly downloaded"; choose **Keep**.
4. Allow the admin prompt. The plugins go into system folders, so this is needed.
5. Read the beta notice, pick the formats you want (VST3, CLAP or both) and click **Install**.
6. Open your DAW and rescan plugins if it doesn't find "Tone Legion Multiband" by itself.

### Where it goes

| What | Folder |
|---|---|
| VST3 | `C:\Program Files\Common Files\VST3\Tone Legion Multiband.vst3` |
| CLAP | `C:\Program Files\Common Files\CLAP\Tone Legion Multiband.clap` |
| These notes, the beta notice, licences, uninstaller | `C:\Program Files\Tone Legion\Multiband` |

### Uninstall

Close your DAW, open **Settings > Apps > Installed apps**, find **Tone Legion Multiband beta** and choose
**Uninstall**. This removes the VST3, the CLAP and the folder above.

## Mac

### Install

1. Quit your DAW.
2. Double-click the `.pkg`. Because it isn't signed yet, macOS will say it can't check it for malicious
   software (or that it's from an unidentified developer). Click **Done** or **OK**, then open
   **System Settings > Privacy & Security**, scroll down and click **Open Anyway** next to the installer's
   name, and confirm with your password.
3. Read the beta notice, then on **Installation Type** tick the formats you want (AU, VST3, CLAP) and click
   **Install**.
4. Open your DAW. Logic and GarageBand use the AU; if Logic doesn't list it, open
   **Logic Pro > Settings > Plug-in Manager** and rescan it.

### Where it goes

| What | Folder |
|---|---|
| AU | `/Library/Audio/Plug-Ins/Components/Tone Legion Multiband.component` |
| VST3 | `/Library/Audio/Plug-Ins/VST3/Tone Legion Multiband.vst3` |
| CLAP | `/Library/Audio/Plug-Ins/CLAP/Tone Legion Multiband.clap` |
| These notes, the beta notice, licences, uninstaller | `/Library/Application Support/Tone Legion/Multiband` |

### Uninstall

Quit your DAW. In Finder choose **Go > Go to Folder**, type `/Library/Application Support/Tone Legion/Multiband`
and double-click **Uninstall Tone Legion Multiband.command**. It opens Terminal and asks for your Mac password,
then removes the AU, VST3, CLAP and that folder.

## Updating to a newer beta

Quit your DAW and run the new installer. It replaces the old build; you don't need to uninstall first.
Sessions saved with an older beta may not reload exactly the same, because settings can still change
between beta builds.

## Report a bug

Send it to the person who gave you the installer. These details help most:

- What happened, and what you expected instead.
- The steps to make it happen again, if you know them.
- Your computer (Windows or Mac model), OS version, DAW and its version, and the plugin format (AU, VST3 or CLAP).
- The beta version, from the installer's file name.
- Your sample rate and buffer size.
- If the DAW crashed: its crash log or crash report (on a Mac, the report window that appears after the crash
  has the text), and a session file that shows the problem if you can share one.
- A short audio clip or screenshot if the problem is something you hear or see.
