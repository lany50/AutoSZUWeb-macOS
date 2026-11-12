# 第三方组件与许可证

AutoSZUWeb 的原项目采用 MIT License，版权声明保留于 `src-tree/LICENSE`。本文件记录当前分发的第三方依赖及其许可证。许可证全文位于 `licenses/`，下载来源和 SHA-256 位于 `licenses/sources.json`。

## macOS 运行库与构建头文件

| 组件 | 当前版本与用途 | 许可证全文 |
| --- | --- | --- |
| curl | 8.21.0 运行库、8.7.1 头文件 | [8.21.0](licenses/curl-8.21.0.txt)、[8.7.1](licenses/curl-8.7.1.txt) |
| OpenSSL | 3.6.3 运行库、3.5.4 头文件，Apache-2.0 | [3.6.3](licenses/openssl-3.6.3.txt)、[3.5.4](licenses/openssl-3.5.4.txt) |
| Brotli | 1.2.0，MIT | [LICENSE](licenses/brotli-1.2.0.txt) |
| nghttp2 | 1.70.0，MIT | [COPYING](licenses/nghttp2-1.70.0.txt) |
| nghttp3 | 1.18.0，MIT | [COPYING](licenses/nghttp3-1.18.0.txt) |
| ngtcp2 | 1.25.0，含 OpenSSL 适配运行库，MIT | [COPYING](licenses/ngtcp2-1.25.0.txt) |
| libpsl | 0.23.3，MIT | [COPYING](licenses/libpsl.txt) |
| libssh2 | 1.11.1，BSD-3-Clause | [COPYING](licenses/libssh2-1.11.1.txt) |
| Zstandard | 1.5.7，按 BSD-3-Clause 分发 | [LICENSE](licenses/zstd-1.5.7.txt) |
| nlohmann/json | 3.12.0 单头文件，MIT | [LICENSE.MIT](licenses/nlohmann-json-3.12.0.txt) |

版本来自依赖清单、运行库版本接口及库内版本字符串。固定依赖的文件校验值见 `deps/manifest.json`。

## 源码树保留的 Windows 静态库

`src-tree/lib/` 的历史静态库包含 curl 8.20.0、LibreSSL 4.3.1、nghttp2 1.69.0、nghttp3 1.15.0、ngtcp2 1.22.1、libpsl 0.21.5、libssh2 1.11.1、Zstandard 1.5.7、Brotli，以及 zlib-ng 2.3.3。zlib-ng 的 zlib 兼容版本字符串为 `1.3.1.zlib-ng`。Brotli 静态库的具体版本尚待核实，随附项目 MIT 许可证。

对应的版本许可证见 [curl](licenses/curl-8.20.0.txt)、[LibreSSL](licenses/libressl-4.3.1.txt)、[nghttp2](licenses/nghttp2-1.69.0.txt)、[nghttp3](licenses/nghttp3-1.15.0.txt)、[ngtcp2](licenses/ngtcp2-1.22.1.txt)、[libpsl](licenses/libpsl-0.21.5.txt)、[zlib-ng](licenses/zlib-ng.txt)。其余组件沿用上表所列许可证文件。

LibreSSL 分发声明如下。

> This product includes software developed by the OpenSSL Project for use in the OpenSSL Toolkit (http://www.openssl.org/)
>
> This product includes cryptographic software written by Eric Young (eay@cryptsoft.com).
>
> This product includes software written by Tim Hudson (tjh@cryptsoft.com).

## Public Suffix List 数据

libpsl 内置的 Public Suffix List 数据采用 Mozilla Public License 2.0。macOS 运行库报告的数据 SHA-1 为 `ecd73e513368ee32a1d0c8b3efac6c838fa02540`。项目维护方提供完整可编辑数据源及版本历史。[许可证全文](licenses/public-suffix-list.txt) · [数据源与历史](https://github.com/publicsuffix/list/blob/master/public_suffix_list.dat)

## 图标来源

深圳大学校徽素材的来源与制作记录见 `src-tree/res/szu/SOURCE.md`。校徽素材的权利归原权利人所有。
