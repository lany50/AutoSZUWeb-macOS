#!/bin/bash
# Shared by the local build and regression entry points.
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CXX=/Library/Developer/CommandLineTools/usr/bin/clang++
SDK=/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk
FW_SRC="$ROOT/deps/runtime-macos-arm64"
python3 "$ROOT/tools/verify_dependencies.py"
COMMON=(-std=c++17 -arch arm64 -mmacosx-version-min=14.0 -isysroot "$SDK"
  -I"$ROOT/src-tree/include" -I"$ROOT/src-tree/src/internal"
  -I"$ROOT/deps/curl/include" -I"$ROOT/deps/openssl/include"
  -I"$ROOT/deps/json-src/single_include")
CURL_LIB="$FW_SRC/libcurl.4.dylib"
CRYPTO_LIB="$FW_SRC/libcrypto.3.dylib"
export DYLD_LIBRARY_PATH="$FW_SRC"
