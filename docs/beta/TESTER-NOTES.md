# Tone Legion Multiband beta: tester notes

Thanks for trying the beta. This build is for Windows 10 and 11 (64-bit).

## Install

1. If the download is a `.zip`, right-click it and pick **Extract All**, then open the folder.
2. Close your DAW.
3. Double-click `ToneLegionMultiband-<version>-beta-windows-x64-setup.exe`.
4. Windows may show **"Windows protected your PC"**. The installer isn't code-signed yet, so Windows
   doesn't recognise the publisher. Click **More info**, then **Run anyway**. Your browser may also say
   the file "isn't commonly downloaded"; choose **Keep**.
5. Allow the admin prompt. The plugins go into system folders, so this is needed.
6. Read the beta notice, pick the formats you want (VST3, CLAP or both) and click **Install**.
7. Open your DAW and rescan plugins if it doesn't find "Tone Legion Multiband" by itself.

## Where it goes

| What | Folder |
|---|---|
| VST3 | `C:\Program Files\Common Files\VST3\Tone Legion Multiband.vst3` |
| CLAP | `C:\Program Files\Common Files\CLAP\Tone Legion Multiband.clap` |
| These notes, the beta notice, licences, uninstaller | `C:\Program Files\Tone Legion\Multiband` |

## Update to a newer beta

Close your DAW and run the new installer. It replaces the old build; you don't need to uninstall first.
Sessions saved with an older beta may not reload exactly the same, because settings can still change
between beta builds.

## Uninstall

Open **Settings > Apps > Installed apps**, find **Tone Legion Multiband beta** and choose **Uninstall**.
This removes the VST3, the CLAP and the folder above. Close your DAW first.

## Report a bug

Send it to the person who gave you the installer. These details help most:

- What happened, and what you expected instead.
- The steps to make it happen again, if you know them.
- Your DAW and its version, the plugin format (VST3 or CLAP) and your Windows version.
- The beta version, from the installer's file name or **Installed apps**.
- Your sample rate and buffer size.
- If the DAW crashed: its crash log or crash report, and a session file that shows the problem if you
  can share one.
- A short audio clip or screenshot if the problem is something you hear or see.
