# --- icon (Icon Composer .icon -> Assets.car for macOS 26+, .icns fallback)
echo "==> icon"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
ICON_NAME=$(basename "$ICON" .icon)
if ! xcrun actool "$ICON" --compile "$C/Resources" --app-icon "$ICON_NAME" --platform macosx \
    --target-device mac --minimum-deployment-target 11.0 --output-partial-info-plist "$TMP/icon.plist" \
    --enable-on-demand-resources NO --development-region en >"$TMP/actool.log" 2>&1; then
    echo "warning: actool unavailable; using a standard .icns icon"
    ICONSET="$TMP/$ICON_NAME.iconset"
    mkdir -p "$ICONSET"
    for size in 16 32 128 256 512; do
        sips -z "$size" "$size" "$ICON/Assets/callofduty.png" --out "$ICONSET/icon_${size}x${size}.png" >/dev/null
        double=$((size * 2))
        sips -z "$double" "$double" "$ICON/Assets/callofduty.png" --out "$ICONSET/icon_${size}x${size}@2x.png" >/dev/null
    done
    # Wrap the PNG representations directly; iconutil can also fail in a sandbox.
    python3 - "$ICONSET" "$C/Resources/$ICON_NAME.icns" <<'PYICON'
from pathlib import Path
import struct
import sys
source = Path(sys.argv[1])
representations = {
    "icp4": "icon_16x16.png", "icp5": "icon_32x32.png",
    "ic07": "icon_128x128.png", "ic08": "icon_256x256.png",
    "ic09": "icon_512x512.png", "ic10": "icon_512x512@2x.png",
    "ic11": "icon_16x16@2x.png", "ic12": "icon_32x32@2x.png",
    "ic13": "icon_128x128@2x.png", "ic14": "icon_256x256@2x.png",
}
chunks = []
for kind, filename in representations.items():
    data = (source / filename).read_bytes()
    if not data.startswith(b"\x89PNG\r\n\x1a\n"):
        raise SystemExit(f"Expected PNG icon representation: {filename}")
    chunks.append(kind.encode("ascii") + struct.pack(">I", len(data) + 8) + data)
body = b"".join(chunks)
Path(sys.argv[2]).write_bytes(b"icns" + struct.pack(">I", len(body) + 8) + body)
PYICON
fi

