#!/usr/bin/env bash
# CI smoke test: install everything, check, remove; then install AU only via a choice-changes file.
set -euo pipefail
pkg=$1
au="/Library/Audio/Plug-Ins/Components/Tone Legion Multiband.component"
vst3="/Library/Audio/Plug-Ins/VST3/Tone Legion Multiband.vst3"
clap="/Library/Audio/Plug-Ins/CLAP/Tone Legion Multiband.clap"
support="/Library/Application Support/Tone Legion/Multiband"
uninstall="$support/Uninstall Tone Legion Multiband.command"
check() { if eval "$1"; then echo "ok: $2"; else echo "FAILED: $2" >&2; exit 1; fi; }

sudo installer -pkg "$pkg" -target /
check '[ -d "$au/Contents/MacOS" ]' "AU installed"
check '[ -d "$vst3/Contents/MacOS" ]' "VST3 installed"
check '[ -d "$clap/Contents/MacOS" ]' "CLAP installed"
check '[ -x "$uninstall" ] && [ -f "$support/Tester notes.txt" ]' "notes and executable uninstaller installed"
check 'pkgutil --pkgs | grep -q com.tonelegion.multiband.pkg.au' "AU receipt registered"
sudo sh "$uninstall" </dev/null
check '[ ! -e "$au" ] && [ ! -e "$vst3" ] && [ ! -e "$clap" ] && [ ! -e "$support" ]' "uninstaller removed everything"
check '! pkgutil --pkgs | grep -q com.tonelegion.multiband.pkg' "receipts forgotten"

choices=$(mktemp)
cat > "$choices" <<'XML'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><array>
  <dict><key>attributeSetting</key><integer>0</integer><key>choiceAttribute</key><string>selected</string><key>choiceIdentifier</key><string>vst3</string></dict>
  <dict><key>attributeSetting</key><integer>0</integer><key>choiceAttribute</key><string>selected</string><key>choiceIdentifier</key><string>clap</string></dict>
</array></plist>
XML
sudo installer -pkg "$pkg" -target / -applyChoiceChangesXML "$choices"
check '[ -d "$au/Contents/MacOS" ]' "AU-only install has AU"
check '[ ! -e "$vst3" ] && [ ! -e "$clap" ]' "AU-only install has no VST3 or CLAP"
check '[ -x "$uninstall" ]' "AU-only install still has the uninstaller"
sudo sh "$uninstall" </dev/null
echo "pkg smoke test passed."
