#!/bin/bash
# 构建 AutoSZUWeb（arm64, macOS 14+），并组装本地签名的 .app
# 用法: ./build.sh   →  产物: build/AutoSZUWeb.app
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
source "$ROOT/tools/macos-toolchain.sh"
SRC="$ROOT/src-tree"
APP="$ROOT/build/AutoSZUWeb.app"
VERSION="$(sed -n 's/.*String = "\([^"]*\)".*/\1/p' "$SRC/include/app_version.h")"

mkdir -p "$ROOT/build"
rm -f "$ROOT/build/AutoSZUWeb"

echo "==> 编译"
"$CXX" "${COMMON[@]}" -O2 \
  "$SRC/src/AutoSZUWeb.cpp" "$SRC/src/web.cpp" "$SRC/src/srun.cpp" \
  "$SRC/src/cli.cpp" "$SRC/src/config.cpp" "$SRC/src/internal/credential_crypto.cpp" \
  "$SRC/src/platform_support.cpp" "$SRC/src/runtime.cpp" "$SRC/src/sys.cpp" \
  "$SRC/src/uninstall.cpp" "$SRC/src/user_data.cpp" "$SRC/src/status.cpp" \
  "$SRC/src/network_observation.cpp" "$SRC/src/macos/environment.cpp" \
  "$SRC/src/json_store.cpp" "$SRC/src/settings.cpp" "$SRC/src/event_history.cpp" "$SRC/src/wake_signal.cpp" "$SRC/src/macos/network_events.mm" \
  "$SRC/src/macos/app_windows.mm" \
  "$SRC/src/macos/ui.mm" "$SRC/src/macos/statusbar.mm" "$SRC/src/macos/netstats.mm" "$SRC/src/macos/notify.mm" "$SRC/src/macos/sys.cpp" "$SRC/src/macos/file_path.cpp" \
  "$SRC/src/posix/network.cpp" "$SRC/src/posix/single_instance.cpp" \
  -framework Cocoa -framework Security -framework Foundation -framework UserNotifications -framework SystemConfiguration \
  "$FW_SRC/libcurl.4.dylib" "$FW_SRC/libssl.3.dylib" "$FW_SRC/libcrypto.3.dylib" \
  -o "$ROOT/build/AutoSZUWeb" || { echo "编译失败"; exit 1; }
[[ -x "$ROOT/build/AutoSZUWeb" ]] || { echo "编译失败"; exit 1; }

echo "==> 组装 bundle"
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Frameworks" "$APP/Contents/Resources"
cp "$ROOT/build/AutoSZUWeb" "$APP/Contents/MacOS/AutoSZUWeb"
cp "$FW_SRC"/*.dylib "$APP/Contents/Frameworks/"
cp "$SRC/res/szu/AppIcon.icns" "$SRC/res/szu/SZUEmblem.pdf" "$APP/Contents/Resources/"
cp "$SRC/res/szu/SOURCE.md" "$APP/Contents/Resources/BrandingSources.md"
cp "$ROOT/LICENSE" "$ROOT/THIRD_PARTY_NOTICES.md" "$APP/Contents/Resources/"
cp -R "$ROOT/licenses" "$APP/Contents/Resources/licenses"
sed -e "s/@PROJECT_VERSION@/$VERSION/g" "$SRC/Info.plist.in" > "$APP/Contents/Info.plist"
plutil -lint "$APP/Contents/Info.plist" >/dev/null

echo "==> 签名"
SIGN_ID="${MACOS_SIGN_IDENTITY:-AutoSZUWeb Local Signing}"
if codesign --force --sign "$SIGN_ID" --entitlements "$SRC/packaging/entitlements.plist" "$APP" 2>/dev/null; then
    echo "    使用固定身份: $SIGN_ID"
else
    echo "    未找到签名身份，使用 ad-hoc 本地签名"
    codesign --force --sign - --entitlements "$SRC/packaging/entitlements.plist" "$APP"
fi
codesign --verify --deep --strict "$APP"

echo "==> 完成: $APP ($(plutil -extract CFBundleShortVersionString raw "$APP/Contents/Info.plist"))"
