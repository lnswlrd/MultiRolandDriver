#!/bin/bash
set -euo pipefail

PLUGIN="MultiRolandDriver.plugin"
INSTALL_DIR="$HOME/Library/Audio/MIDI Drivers"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SOURCE="$SCRIPT_DIR/$PLUGIN"

if [ ! -d "$SOURCE" ]; then
    echo "Error: $PLUGIN not found in $SCRIPT_DIR"
    exit 1
fi

echo "Installing $PLUGIN..."

# Remove macOS quarantine flag (downloaded files)
xattr -rd com.apple.quarantine "$SOURCE" 2>/dev/null || true

# Ad-hoc code sign
codesign --force --sign - "$SOURCE"

# Copy to MIDI Drivers
mkdir -p "$INSTALL_DIR"
cp -R "$SOURCE" "$INSTALL_DIR/"

# Restart MIDIServer so it picks up the new plugin
killall MIDIServer 2>/dev/null || true

echo "Installed to $INSTALL_DIR/"
echo "MIDIServer restarted. Connect your Roland synth and open Audio MIDI Setup to verify."
