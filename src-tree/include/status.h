#pragma once

#include <string>
#include <functional>

// 运行状态中枢：常驻循环写入，菜单栏/日志读取。全部接口线程安全。
namespace Status
{
    enum class State { Unknown, Online, Offline, Authenticating, Paused, Waiting, Attention };

    struct Snapshot
    {
        State state = State::Unknown;
        std::string detail;          // 附加说明（失败原因等）
        std::string lastAuthTime;    // "2026-11-03 12:50:44"
        std::string lastAuthMethod;  // "ePortal(宿舍区)"
        std::string lastAuthIp;
        bool lastAuthOk = false;
        bool autoAuth = true;
        bool authInProgress = false;
        bool manualAuthPending = false;
        int latencyMs = -1;          // 最近一次外网探测耗时，-1 表示未知
        long long lastCheckTime = 0;
        long long onlineSince = 0;   // 进入在线态的时间戳（秒），0 表示当前不在线
        int authCount = 0;           // 本次运行以来的认证次数
        std::string networkLabel = "正在识别…";
        std::string currentIp;
        std::string retryText = "尚未开始";
        bool diagnosticRunning = false;
        unsigned diagnosticSequence = 0;
        std::string diagnosticReport;
        int monitorInterval = 0;
        long long nextCheckTime = 0;
        unsigned probeCount = 0;
        bool powerSaving = true;
    };

    void SetState(State state, const std::string& detail = std::string());
    void SetAutoAuth(bool enabled);
    void SetLatency(int milliseconds);
    void RecordAuth(bool ok, const std::string& method, const std::string& ip,
                    const std::string& detail);
    Snapshot Get();
    // The callback runs after releasing the status mutex. UI clients dispatch to their main queue.
    void SetChangeCallback(std::function<void()> callback);
    const char* StateText(State state);

    // 菜单栏「立即认证」：置位后由常驻循环在 0.5 秒内消费并立刻认证一次。
    void RequestManualAuth();
    bool TryBeginAuth();
    void EndAuth();
    bool ManualAuthPending();
    bool ConsumeManualAuthRequest();
    void SetNetwork(const std::string& label, const std::string& ip);
    void SetRetry(const std::string& text);
    void RequestDiagnostic();
    bool DiagnosticPending();
    bool ConsumeDiagnosticRequest();
    void CompleteDiagnostic(const std::string& report);
    void SetMonitoring(int seconds, bool saving, bool didProbe = false);
}
