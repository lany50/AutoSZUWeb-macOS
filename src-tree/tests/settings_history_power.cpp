#include "settings.h"
#include "config.h"
#include "json_store.h"
#include "event_history.h"
#include "monitor_schedule.h"
#include "wake_signal.h"
#include "credential_crypto.h"
#include <fstream>
#include <iostream>
#include <thread>
#include <atomic>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace fs = std::filesystem;
namespace
{
    fs::path root;
    int failures = 0;
    const std::vector<unsigned char> key(32, 0x54); // Synthetic test key, never real credentials.
    void Check(bool ok, const char* text) { std::cout << (ok ? "PASS " : "FAIL ") << text << '\n'; if (!ok) ++failures; }
    std::string Bytes(const fs::path& path) { std::ifstream file(path); return {std::istreambuf_iterator<char>(file),{}}; }
}
fs::path GetConfigPath() { return root / "AutoSZUWeb/setting.json"; }
std::string EncryptStr(const std::string& value) { return CredentialCrypto::EncryptAesGcm(key,value); }
std::string DecryptStr(const std::string& value) { return CredentialCrypto::DecryptAesGcm(key,value); }

int main(int argc, char* argv[])
{
    root = fs::temp_directory_path() / ("AutoSZU-Settings-" + std::to_string(getpid()));
    fs::create_directories(root);
    const auto path = GetConfigPath();
    std::string error;
    Settings::Preferences prefs;
    const std::string user = "TEST-ACCOUNT-1234", pass = "synthetic-only-password";
    Check(!Settings::Save(path,user,"","",prefs,error) && !fs::exists(path), "incomplete first setup does not create a config");
    Check(!Settings::Save(path,user,pass,"mismatch",prefs,error) && !fs::exists(path), "password mismatch cannot save");
    Check(Settings::Save(path,user,pass,pass,prefs,error), "complete settings save succeeds without any network code");
    nlohmann::json data; JsonStore::Read(path,data);
    const auto encryptedUser = data["Account"], encryptedPass = data["Password"];
    Check(Bytes(path).find(pass) == std::string::npos && Bytes(path).find(user) == std::string::npos,
        "settings persist no plaintext account or password");
    Check(DecryptStr(encryptedUser) == user && DecryptStr(encryptedPass) == pass,
        "production AES-GCM implementation round trips saved credentials");
    struct stat mode{}; stat(path.c_str(), &mode);
    Check((mode.st_mode & 0777) == 0600, "config replacement has owner-only permissions");
    const auto summary = Settings::ReadSummary(path);
    Check(summary.accountHint.find("1234") != std::string::npos && summary.accountHint.find(user) == std::string::npos,
        "settings summary reveals only account suffix");
    prefs.autoAuth = false; prefs.powerSaving = false; prefs.stableInterval = 30;
    Check(Settings::Save(path,"","","",prefs,error), "blank fields allow preferences-only save");
    JsonStore::Read(path,data);
    Check(data["Account"] == encryptedUser && data["Password"] == encryptedPass && data["AutoAuth"] == false,
        "blank fields preserve exact encrypted credentials");
    const auto beforeMismatch = Bytes(path);
    Check(!Settings::Save(path,"","different","mismatch",prefs,error) && Bytes(path) == beforeMismatch,
        "rejected password update preserves original bytes");
    Check(Settings::Save(path,"","new-synthetic","new-synthetic",prefs,error), "password-only update is supported");
    JsonStore::Read(path,data);
    Check(data["Account"] == encryptedUser && DecryptStr(data["Password"]) == "new-synthetic", "password update preserves existing account");
    const auto saved = Bytes(path);
    { std::ofstream file(path); file << "corrupt original data"; }
    Check(!Settings::Save(path,user,pass,pass,prefs,error) && Bytes(path) == "corrupt original data",
        "corrupt config is never silently replaced");
    { std::ofstream file(path); file << saved; }
    JsonStore::Update(path,false,[](auto& value,std::string&) { value["Custom"]="preserve"; return true; },error);
    Check(Config::WriteAutoAuthEnabled(path,true,error), "legacy auto-auth switch still writes through atomic store");
    JsonStore::Read(path,data);
    Check(data["Custom"] == "preserve" && data["Account"] == encryptedUser, "auto-auth updates preserve unrelated fields");
    prefs.stableInterval = 15;
    Check(!Settings::Save(path,"","","",prefs,error), "invalid monitor interval cannot be stored");
    const auto counterPath = root / "counter.json";
    auto increment = [&] {
        for (int i=0;i<20;++i)
        {
            std::string issue;
            if (!JsonStore::Update(counterPath,true,[](auto& value,std::string&) {
                value["count"] = value.value("count",0)+1; return true; },issue)) return false;
        }
        return true;
    };
    const pid_t child = fork();
    if (child == 0) _exit(increment() ? 0 : 1);
    const bool parentOk = increment(); int status = -1;
    if (child > 0) waitpid(child,&status,0);
    JsonStore::Read(counterPath,data);
    Check(child>0 && parentOk && WIFEXITED(status) && WEXITSTATUS(status)==0 && data["count"]==40,
        "separate processes cannot lose merged JSON writes");
    const auto events = root / "events.json";
    for (int i=0;i<205;++i) History::Add(events,"test","Synthetic event "+std::to_string(i),"controlled summary");
    auto list = History::Read(events);
    Check(list.size()==200 && list.front().title=="Synthetic event 204" && list.back().title=="Synthetic event 5",
        "history retains the newest 200 events in reverse order");
    History::Add(events,"test","Synthetic event 204","controlled summary");
    Check(History::Read(events).size()==200, "identical adjacent events are deduplicated");
    std::string chinese; for (int i=0;i<300;++i) chinese += "测";
    History::Add(events,"test","UTF-8",chinese);
    list = History::Read(events);
    Check(!list.empty() && list.front().detail.size()<=600 && list.front().detail.size()%3==0,
        "long event details preserve UTF-8 boundaries");
    JsonStore::Update(events,false,[](auto& value,std::string&) { value["events"].push_back({{"kind",42}}); return true; },error);
    Check(History::Add(events,"test","Valid event after bad row","safe"), "malformed old rows do not prevent future events");
    Check(History::Text(events).find(pass)==std::string::npos, "history output includes no test credentials");
    Monitor::AdaptiveSchedule schedule;
    Check(schedule.Healthy(true,60)==10 && schedule.Healthy(true,60)==20 && schedule.Healthy(true,60)==40 &&
        schedule.Healthy(true,60)==60 && schedule.Healthy(true,60)==60, "stable network checks back off from 10 to 60 seconds");
    schedule.Reset();
    Check(schedule.Healthy(true,60)==10, "wake or network change resets the stable schedule");
    Check(schedule.Healthy(false,120)==10 && schedule.Healthy(false,120)==10, "disabling power saving keeps ten-second checks");
    schedule.Reset(); int maximum=0;for(int i=0;i<10;++i) maximum=schedule.Healthy(true,120);
    Check(maximum==120, "configured two-minute ceiling is honored");
    WakeSignal::Take(); WakeSignal::Notify(WakeSignal::Network); WakeSignal::Notify(WakeSignal::Manual);
    const auto pending = WakeSignal::Wait(std::chrono::seconds(60));
    Check((pending & WakeSignal::Network) && (pending & WakeSignal::Manual), "events arriving before sleep cannot be lost");
    const auto start=std::chrono::steady_clock::now();
    std::thread thread([] { std::this_thread::sleep_for(std::chrono::milliseconds(20)); WakeSignal::Notify(WakeSignal::Settings); });
    const auto signal=WakeSignal::Wait(std::chrono::seconds(60)); thread.join();
    Check((signal & WakeSignal::Settings) && std::chrono::steady_clock::now()-start<std::chrono::seconds(1),
        "settings signal interrupts a long wait immediately");
    Check(WakeSignal::Wait(std::chrono::milliseconds(10))==WakeSignal::None, "ordinary timeout reports no false network event");
    if (argc == 2)
    {
        const fs::path fixture=fs::path(argv[1])/"AutoSZUWeb";fs::create_directories(fixture);
        Settings::Preferences uiPrefs;uiPrefs.autoAuth=false;
        Check(Settings::Save(fixture/"setting.json",user,pass,pass,uiPrefs,error), "isolated UI fixture created");
        std::ofstream file(fixture/"credential.key",std::ios::binary);file.write(reinterpret_cast<const char*>(key.data()),key.size());file.close();
        fs::permissions(fixture/"credential.key",fs::perms::owner_read|fs::perms::owner_write);
        History::Add(fixture/"events.json","test","测试示例：检测到连接中断","用于界面检查，并非真实网络故障");
        History::Add(fixture/"events.json","test","测试示例：等待重试","网络超时；等待 60 秒");
        History::Add(fixture/"events.json","test","测试示例：连接已恢复","HTTP 204 验证通过");
    }
    fs::remove_all(root);
    std::cout << "Failures: " << failures << '\n';
    return failures ? 1:0;
}
