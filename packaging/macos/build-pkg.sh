#!/usr/bin/env bash
# Builds the macOS beta installer (.pkg) with Apple's pkgbuild / productbuild.
#   packaging/macos/build-pkg.sh [artefact-dir] [build-label]
# Expects a Release build first (cmake --build build --config Release).
# Output: build/installer/ToneLegionMultiband-<version>-beta-macos-arm64.pkg
# The package is NOT signed or notarised.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
art=${1:-"$root/build/ToneLegionMultiband_artefacts/Release"}
label=${2:-local}
name="Tone Legion Multiband"
id_base="com.tonelegion.multiband.pkg"

version=$(sed -nE 's/^project\(ToneLegionMultiband VERSION ([0-9]+\.[0-9]+\.[0-9]+).*/\1/p' "$root/CMakeLists.txt")
[ -n "$version" ] || { echo "Could not read the project version from CMakeLists.txt" >&2; exit 1; }

work="$root/build/pkg-work"
out="$root/build/installer"
rm -rf "$work"; mkdir -p "$work/components" "$work/resources" "$out"

# format, artefact sub-folder, bundle extension, install folder
formats=(
  "au|AU|component|/Library/Audio/Plug-Ins/Components"
  "vst3|VST3|vst3|/Library/Audio/Plug-Ins/VST3"
  "clap|CLAP|clap|/Library/Audio/Plug-Ins/CLAP"
)

for f in "${formats[@]}"; do
  IFS='|' read -r key sub ext dest <<<"$f"
  src="$art/$sub/$name.$ext"
  [ -d "$src" ] || { echo "Missing build output: $src" >&2; exit 1; }
  stage="$work/stage-$key"
  mkdir -p "$stage$dest"
  ditto "$src" "$stage$dest/$name.$ext"

  # pkgbuild marks bundles relocatable by default: Installer would then "upgrade" a copy with the
  # same bundle ID found elsewhere on disk instead of writing to $dest. Turn that off for every bundle.
  plist="$work/$key-components.plist"
  pkgbuild --analyze --root "$stage" "$plist" >/dev/null
  i=0
  while /usr/libexec/PlistBuddy -c "Print :$i" "$plist" >/dev/null 2>&1; do
    /usr/libexec/PlistBuddy -c "Set :$i:BundleIsRelocatable false" "$plist"
    i=$((i + 1))
  done

  scripts=()
  if [ "$key" = au ]; then scripts=(--scripts "$here/scripts-au"); fi
  pkgbuild --root "$stage" --component-plist "$plist" --install-location / \
           --identifier "$id_base.$key" --version "$version" ${scripts[@]+"${scripts[@]}"} \
           "$work/components/$key.pkg"
done

# Support files (always installed, not a choice): uninstaller, notes, notices, licences.
support="$work/stage-support/Library/Application Support/Tone Legion/Multiband"
mkdir -p "$support/licenses"
cp "$here/Uninstall Tone Legion Multiband.command" "$support/"
chmod 755 "$support/Uninstall Tone Legion Multiband.command"
cp "$root/docs/beta/TESTER-NOTES.md" "$support/Tester notes.txt"
cp "$root/packaging/beta-notice.txt" "$support/Beta notice.txt"
cp "$root/packaging/THIRD-PARTY-NOTICES.txt" "$support/"
cp "$root"/resources/fonts/OFL-*.txt "$support/licenses/"
[ -f "$root/libs/JUCE/LICENSE.md" ] && cp "$root/libs/JUCE/LICENSE.md" "$support/licenses/JUCE-LICENSE.md"
[ -f "$root/libs/clap-juce-extensions/LICENSE" ] && cp "$root/libs/clap-juce-extensions/LICENSE" "$support/licenses/clap-juce-extensions-LICENSE.txt"
[ -f "$root/libs/clap-juce-extensions/clap-libs/clap/LICENSE" ] && cp "$root/libs/clap-juce-extensions/clap-libs/clap/LICENSE" "$support/licenses/CLAP-LICENSE.txt"
pkgbuild --root "$work/stage-support" --install-location / --identifier "$id_base.support" \
         --version "$version" "$work/components/support.pkg"

cp "$root/packaging/beta-notice.txt" "$work/resources/beta-notice.txt"
sed -e "s/@VERSION@/$version/g" -e "s/@BUILD@/$label/g" "$here/distribution.xml.in" > "$work/distribution.xml"

pkg="$out/ToneLegionMultiband-$version-beta-macos-arm64.pkg"
productbuild --distribution "$work/distribution.xml" --package-path "$work/components" \
             --resources "$work/resources" "$pkg"
echo "Built $pkg"
