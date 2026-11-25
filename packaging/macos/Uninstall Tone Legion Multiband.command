#!/bin/sh
# Removes the Tone Legion Multiband beta (AU, VST3, CLAP) and its installer receipts.
echo "This removes Tone Legion Multiband (AU, VST3 and CLAP). Your Mac password is needed."
sudo rm -rf "/Library/Audio/Plug-Ins/Components/Tone Legion Multiband.component" \
            "/Library/Audio/Plug-Ins/VST3/Tone Legion Multiband.vst3" \
            "/Library/Audio/Plug-Ins/CLAP/Tone Legion Multiband.clap" \
            "/Library/Application Support/Tone Legion/Multiband"
sudo rmdir "/Library/Application Support/Tone Legion" >/dev/null 2>&1
for p in au vst3 clap support; do sudo pkgutil --forget "com.tonelegion.multiband.pkg.$p" >/dev/null 2>&1; done
killall -9 AudioComponentRegistrar >/dev/null 2>&1
echo "Done. You can close this window."
