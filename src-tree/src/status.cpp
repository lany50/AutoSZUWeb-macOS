#include "status.h"
#include "wake_signal.h"

#include <atomic>
#include <ctime>
#include <mutex>

namespace
{
    std::mutex gMutex;
    Status::Snapshot gSnapshot;
    std::function<void()> gChangeCallback;
    std::atomic<bool> gDiagnosticRequested{false};

    // Notify after unlocking so callbacks may safely read the current snapshot.
    class Change
    {
        std::unique_lock<std::mutex> lock{gMutex};
    public:
        ~Change()
        {
            const auto callback = gChangeCallback;
            lock.unlock();
            if (callback) callback();
        }
    };

    std::string NowStamp()
    {
        const std::time_t now = std::time(nullptr);
        std::tm local{};
        localtime_r(&now, &local);
        char buffer[32] = {};
        std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &local);
        return buffer;
    }
}

namespace Status
{
    void SetState(State state, const std::string& detail)
    {
        Change change;
        // 只在真正进入在线态时打点，避免每轮都刷新在线时长。
        if (state == State::Online && gSnapshot.state != State::Online)
            gSnapshot.onlineSince = static_cast<long long>(std::time(nullptr));
        else if (state != State::Online)
            gSnapshot.onlineSince = 0;
        gSnapshot.state = state;
        gSnapshot.detail = detail;
    }

    void SetLatency(int milliseconds)
    {
        Change change;
        gSnapshot.latencyMs = milliseconds;
        gSnapshot.lastCheckTime = static_cast<long long>(std::time(nullptr));
    }

    void SetAutoAuth(bool enabled)
    {
        Change change;
        gSnapshot.autoAuth = enabled;
    }

    void RecordAuth(bool ok, const std::string& method, const std::string& ip,
                    const std::string& detail)
    {
        Change change;
        gSnapshot.lastAuthOk = ok;
        gSnapshot.lastAuthTime = NowStamp();
        gSnapshot.lastAuthMethod = method;
        gSnapshot.lastAuthIp = ip;
        // Only the subsequent connectivity check may mark the network online.
        gSnapshot.state = ok ? State::Authenticating : State::Offline;
        gSnapshot.detail = detail;
        ++gSnapshot.authCount;
        gSnapshot.onlineSince = 0;
    }

    void SetChangeCallback(std::function<void()> callback)
    {
        std::lock_guard<std::mutex> lock(gMutex);
        gChangeCallback = std::move(callback);
    }

    Snapshot Get()
    {
        std::lock_guard<std::mutex> lock(gMutex);
        return gSnapshot;
    }

    const char* StateText(State state)
    {
        switch (state)
        {
            case State::Online:         return "外网可用";
            case State::Offline:        return "离线";
            case State::Authenticating: return "认证中…";
            case State::Paused:         return "已暂停";
            case State::Waiting:        return "自动认证待命";
            case State::Attention:      return "需要处理";
            case State::Unknown:        break;
        }
        return "启动中…";
    }

    void RequestManualAuth()
    {
        Change change;
        if (gSnapshot.authInProgress || gSnapshot.manualAuthPending) return;
        gSnapshot.manualAuthPending = true;
        WakeSignal::Notify(WakeSignal::Manual);
    }

    bool TryBeginAuth()
    {
        Change change;
        if (gSnapshot.authInProgress) return false;
        gSnapshot.authInProgress = true;
        gSnapshot.manualAuthPending = false;
        return true;
    }

    void EndAuth()
    {
        Change change;
        gSnapshot.authInProgress = false;
    }

    bool ManualAuthPending()
    {
        std::lock_guard<std::mutex> lock(gMutex);
        return gSnapshot.manualAuthPending;
    }

    bool ConsumeManualAuthRequest()
    {
        Change change;
        const bool pending = gSnapshot.manualAuthPending;
        gSnapshot.manualAuthPending = false;
        return pending;
    }

    void SetNetwork(const std::string& label, const std::string& ip)
    {
        Change change;
        gSnapshot.networkLabel = label;
        gSnapshot.currentIp = ip;
    }
    void SetRetry(const std::string& text)
    {
        Change change;
        gSnapshot.retryText = text;
    }
    void RequestDiagnostic()
    {
        Change change;
        if (gSnapshot.diagnosticRunning) return;
        gSnapshot.diagnosticRunning = true;
        gDiagnosticRequested.store(true);
        WakeSignal::Notify(WakeSignal::Diagnostic);
    }
    bool DiagnosticPending() { return gDiagnosticRequested.load(); }
    bool ConsumeDiagnosticRequest() { return gDiagnosticRequested.exchange(false); }
    void CompleteDiagnostic(const std::string& report)
    {
        Change change;
        gSnapshot.diagnosticReport = report;
        gSnapshot.diagnosticRunning = false;
        ++gSnapshot.diagnosticSequence;
    }
    void SetMonitoring(int seconds, bool saving, bool didProbe)
    {
        Change change;
        gSnapshot.monitorInterval = seconds;
        gSnapshot.nextCheckTime = seconds ? static_cast<long long>(std::time(nullptr)) + seconds : 0;
        gSnapshot.powerSaving = saving;
        if (didProbe) ++gSnapshot.probeCount;
    }
}
