# AutoSZUWeb 1.7.0-szu

深圳大学校园网菜单栏认证工具，个人维护的 macOS 版本。支持 macOS 14 及以上、Apple Silicon（M1/M2/M3/M4 等 M 系列芯片）。

## 下载与安装

前往 [最新版本下载页](https://github.com/lany50/AutoSZUWeb-macOS/releases/latest)，下载附件中的 `AutoSZUWeb-1.7.0-szu-macOS-arm64.dmg`。

1. 打开 DMG，将 `AutoSZUWeb.app` 拖到「应用程序」。
2. 从「应用程序」启动 AutoSZUWeb，点击屏幕顶部菜单栏的深大校徽。
3. 打开「账号与监测设置」，填写自己的校园网账号和密码，保存设置。
4. 连接校园网后，可点击「立即认证／恢复重试」检查认证结果。启动后会注册登录自启。

当前安装包使用本地签名，尚未进行 Apple 公证。首次启动如提示无法验证开发者，尝试打开一次后，进入「系统设置 → 隐私与安全性」，在对应提示处点击「仍要打开」，再确认打开。

本次发行面向 Apple Silicon Mac、macOS 14 及以上。Intel Mac 和 Windows 的运行情况尚未验证。

## 日常使用

点击菜单栏深大校徽查看连接状态、自动认证开关、最近检查时间、最近认证尝试和下次检查时间。

- **账号与监测设置**管理账号、密码、自动认证及稳定检查间隔。留空的凭据字段沿用现有值，更改密码需要二次确认。
- **立即认证／恢复重试**执行一次手动尝试。认证与外网复查期间合并重复点击。
- **一键诊断**检查物理网络、校园门户、DNS、默认路由路径与物理直连，并显示系统代理配置的检测覆盖情况。
- **最近故障与恢复**显示最近 200 条受控事件摘要，支持复制。
- **菜单栏显示网速**开启即时流量显示。关闭后，仅在菜单展开期间按秒采样。

设置加载和保存通过后台队列处理。打开的设置窗口保留填写草稿，只提交实际修改的偏好字段。保存失败会显示错误并保留输入。诊断完成后更新结果，已关闭的结果窗口保持关闭。

## 自动恢复规则

程序保留外网可用、门户拦截和原因未知三种探测结果。只有在物理网络确认深大门户后才允许提交凭据。

SRun 门户明确返回未在线，或物理探针收到指向已确认校园门户的可信 HTTP 重定向时，可以自动认证。宿舍门户的无账号识别响应只用于确认门户身份，会话状态保持未知。

原因未知时在 2 秒和 5 秒后复查，仍无法判断则每 30 秒观察，并提供手动入口。DNS 故障、出口故障及探针超时会显示待确认状态。当前可信拦截识别覆盖 HTTP 3xx／511 的明确门户目标，实际门户返回的其他形式需要具体响应证据。

网络接口、默认出口、DNS、唤醒和配置变化会使旧任务失效。认证的每一步及结果发布都会复核，正在等待的网络请求支持取消。自动认证关闭后，常驻联网探测暂停，诊断与手动操作仍可使用。

认证请求经过进程内状态与跨进程锁协调，锁覆盖门户再次确认、登录及外网复查。接口接受请求后继续检查外网，只有外网检查通过才记录恢复。认证冷却期间继续观察连接是否已经恢复。

稳定联网时按 10、20、40、60 秒逐步降低检查频率。上限可选 30、60 或 120 秒。每轮优先使用最近成功的端点，失败时展开其他端点，成功端点每 10 分钟轮换。完整诊断收集所有端点结果。

## 代理与网速

默认路由探测使用 libcurl 的路由和环境代理行为。诊断同时显示系统 HTTP／SOCKS／PAC 配置是否存在以及当前覆盖范围。浏览器执行的系统代理或 PAC 路径需要结合其自身状态判断。

网速取当前活动物理接口的字节计数，使用单调时钟计算。切换接口、链路变化、计数器重置及较长采样间隔会重新建立基线，正常的 32 位字节计数回绕仍可计算。

## 本地数据

数据目录为 `~/Library/Application Support/AutoSZUWeb`，权限为 `0700`。配置、密钥、事件与锁文件权限为 `0600`，启动及同值保存会修复旧权限。

账号和密码采用 AES-GCM 加密，密钥保存于本机 `credential.key`。首次迁移可读取原有钥匙串条目。写入采用受限权限临时文件、同步和原子替换，文件类型、归属及写入错误会被检查。

故障时间线位于 `events.json`。每日认证日志位于 `logs`。

## 构建与验证

需要 Apple Silicon Mac、macOS 14 及以上、Xcode Command Line Tools 和 Python 3。可通过 `xcode-select --install` 安装命令行开发工具。在仓库根目录执行以下命令。

```bash
./test.sh
./build.sh
```

构建产物为 `build/AutoSZUWeb.app`。工具链使用 Command Line Tools 中的编译器与 SDK，部署目标为 macOS 14、arm64。构建和测试共同读取 `tools/macos-toolchain.sh`。

依赖固定在 `deps`，每次运行先校验 `deps/manifest.json` 中的 SHA-256。运行库为 libcurl 8.21.0、OpenSSL 3.6.3，其他动态库版本记录于文件名与清单。编译头文件为 curl 8.7.1 和 OpenSSL 3.5.4，使用兼容的公开 API。构建结果由 `AutoSZUWeb Local Signing` 本地身份签名，该身份不可用时使用 ad-hoc 签名。

测试使用合成账号、临时目录、本地 HTTP 服务与受控认证响应。统一入口覆盖旧逻辑、网络探测、取消、状态决策、设置并发、权限、CLI、卸载协调、AppKit 窗口、网速和通知进程回收。

## 命令行

推荐通过隐藏输入完成一次性认证。

```bash
/Applications/AutoSZUWeb.app/Contents/MacOS/AutoSZUWeb --login
/Applications/AutoSZUWeb.app/Contents/MacOS/AutoSZUWeb --login 校园卡号
/Applications/AutoSZUWeb.app/Contents/MacOS/AutoSZUWeb --password-stdin 校园卡号
/Applications/AutoSZUWeb.app/Contents/MacOS/AutoSZUWeb --auto false
/Applications/AutoSZUWeb.app/Contents/MacOS/AutoSZUWeb --settings
/Applications/AutoSZUWeb.app/Contents/MacOS/AutoSZUWeb --history
/Applications/AutoSZUWeb.app/Contents/MacOS/AutoSZUWeb --diagnose
```

`--password-stdin` 从标准输入读取一行密码。旧的两个位置参数调用继续兼容。一次性认证输出接口结果和外网复查结果，退出码 0 延续接口接受语义，1 为认证失败，2 为参数或输入错误，75 为已有认证正在进行。生命周期协调冲突返回 3。

## 卸载与回退

`--uninstall` 通过原有确认窗口启动卸载。程序先停止经验证的常驻后台，再取得独占生命周期锁。独立设置等数据窗口仍打开时，提示关闭窗口并保留数据。桌面 `userdata.txt` 根据来源记录与文件身份确认归属后清理。

生命周期锁位于 `~/Library/Application Support/.AutoSZUWeb.lifecycle.lock`。卸载后保留这个空协调文件，避免并发启动绕过已有锁。数据目录覆盖变量会将协调锁隔离到对应测试根目录。

更新前可保留当前 App 的副本，便于需要时回退。

## 验证范围

自动测试覆盖可重复的网络故障和认证流程中换网，实际校园门户与自然断网恢复需要继续结合真实使用观察。资源消耗使用同机测量记录，瞬时 CPU 与内存读数仅代表对应采样时刻。

## 来源与许可

本项目基于 ATMLuck 的 AutoSZUWeb 继续维护，保留原作者版权与 MIT 许可。本仓库集中整理 macOS 菜单栏、设置、网络监测和认证恢复等改动。[上游项目](https://github.com/ATMLuck/AutoSZUWeb) · [MIT 许可](LICENSE)

打包的第三方依赖许可见 [第三方组件说明](THIRD_PARTY_NOTICES.md)。校徽素材来源记录见 [SOURCE.md](src-tree/res/szu/SOURCE.md)，相关标识权利归原权利人所有。

如遇到问题，可在 [Issues](https://github.com/lany50/AutoSZUWeb-macOS/issues) 提供 macOS 版本、芯片型号、操作步骤和错误提示。发送诊断或日志前，请先检查其中的账号和网络地址信息。
