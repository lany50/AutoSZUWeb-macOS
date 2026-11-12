#import <AppKit/AppKit.h>
#include "wake_signal.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <unistd.h>
namespace fs=std::filesystem;
static fs::path directory;
fs::path GetConfigPath(){return directory/"setting.json";}
int main()
{
    @autoreleasepool
    {
        directory=fs::temp_directory_path()/("AutoSZUEvents-"+std::to_string(getpid()));fs::create_directories(directory);
        {std::ofstream file(GetConfigPath());file<<"{}";}
        WakeSignal::Take(); const bool native=NativeEvents::Start();
        {std::ofstream file(directory/"new.json");file<<"{\"AutoAuth\":false}";}
        fs::rename(directory/"new.json",GetConfigPath());
        bool received=false;
        for(int i=0;i<40 && !received;++i)
        {
            [[NSRunLoop mainRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:.05]];
            received = (WakeSignal::Take() & WakeSignal::Settings) != 0;
        }
        std::cout << (native ? "PASS " : "FAIL ") << "native network and config event observers start\n";
        std::cout << (received ? "PASS " : "FAIL ") << "atomic config replacement wakes monitor without polling\n";
        fs::remove_all(directory);return native&&received?0:1;
    }
}
