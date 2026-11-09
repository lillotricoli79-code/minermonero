#include "system.hpp"

// C++ system-sampling adaptation of NyxRX TelemetrySampler / PowerSampler.
// Upstream: MaleficentAd1803/nyxrx, crates/own-randomx-miner/src/telemetry.rs
// License: MIT; attribution and license text are retained in LICENSES/NYXRX-MIT.txt.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#  define NOMINMAX
#  include <windows.h>
#  include <psapi.h>
#else
#  include <unistd.h>
#endif

namespace unique_rx::telemetry {
namespace {
#ifdef _WIN32
std::uint64_t filetime_value(const FILETIME& value) {
    return (static_cast<std::uint64_t>(value.dwHighDateTime) << 32) |
        static_cast<std::uint64_t>(value.dwLowDateTime);
}
#endif

#ifdef __linux__
struct CpuTimes { std::uint64_t total = 0; std::uint64_t idle = 0; };

std::optional<CpuTimes> read_system_cpu() {
    std::ifstream input("/proc/stat");
    std::string label;
    std::uint64_t user=0, nice=0, system=0, idle=0, iowait=0, irq=0, softirq=0, steal=0;
    if (!(input >> label >> user >> nice >> system >> idle >> iowait >> irq >> softirq >> steal) ||
        label != "cpu") return std::nullopt;
    return CpuTimes{user+nice+system+idle+iowait+irq+softirq+steal, idle+iowait};
}

std::optional<std::pair<std::uint64_t,std::uint64_t>> read_process_cpu_and_rss() {
    std::ifstream input("/proc/self/stat");
    std::string line;
    if (!std::getline(input,line)) return std::nullopt;
    const auto close=line.rfind(')');
    if (close==std::string::npos || close+2>=line.size()) return std::nullopt;
    std::istringstream fields(line.substr(close+2));
    std::vector<std::string> values;
    std::string value;
    while(fields>>value) values.push_back(std::move(value));
    if(values.size()<=21) return std::nullopt;
    try {
        const auto ticks=std::stoull(values[11])+std::stoull(values[12]);
        const auto pages=std::stoull(values[21]);
        const auto page_size=::sysconf(_SC_PAGESIZE);
        if(page_size<=0) return std::nullopt;
        return std::pair{ticks,pages*static_cast<std::uint64_t>(page_size)};
    } catch(...) { return std::nullopt; }
}
#endif

std::optional<double> read_temperature() {
#ifdef __linux__
    const std::filesystem::path root("/sys/class/thermal");
    std::error_code ec;
    double maximum=0.0; bool found=false;
    for(const auto& entry:std::filesystem::directory_iterator(root,ec)) {
        if(ec) break;
        if(!entry.path().filename().string().starts_with("thermal_zone")) continue;
        std::ifstream input(entry.path()/"temp");
        double value=0.0;
        if(!(input>>value)||!std::isfinite(value)) continue;
        if(std::abs(value)>1000.0) value/=1000.0;
        if(value>0.0&&value<150.0) { maximum=found?std::max(maximum,value):value; found=true; }
    }
    if(found) return maximum;
#endif
    return std::nullopt;
}

std::optional<std::uint64_t> read_cpu_frequency_mhz() {
#ifdef __linux__
    const std::filesystem::path root("/sys/devices/system/cpu");
    std::error_code ec; std::uint64_t sum=0,count=0;
    for(const auto& entry:std::filesystem::directory_iterator(root,ec)) {
        if(ec) break;
        const auto name=entry.path().filename().string();
        if(!name.starts_with("cpu")||name.size()<=3||
           !std::all_of(name.begin()+3,name.end(),[](unsigned char c){return c>='0'&&c<='9';})) continue;
        std::ifstream input(entry.path()/"cpufreq/scaling_cur_freq");
        std::uint64_t khz=0;
        if(input>>khz&&khz>0) {sum+=khz;++count;}
    }
    if(count) return (sum/count)/1000;
#endif
    return std::nullopt;
}
std::optional<bool> read_on_battery() {
#ifdef _WIN32
    SYSTEM_POWER_STATUS status{};
    if (GetSystemPowerStatus(&status) != 0 && status.ACLineStatus != 255)
        return status.ACLineStatus == 0;
#elif defined(__linux__)
    const std::filesystem::path root("/sys/class/power_supply");
    std::error_code ec;
    bool has_battery = false;
    for (const auto& entry : std::filesystem::directory_iterator(root, ec)) {
        if (ec) break;
        std::ifstream type_file(entry.path() / "type");
        std::string type;
        if (!(type_file >> type)) continue;
        if (type == "Battery") has_battery = true;
        if (type == "Mains" || type == "USB" || type == "USB_C") {
            std::ifstream online_file(entry.path() / "online");
            int online = -1;
            if (online_file >> online && online == 1) return false;
        }
    }
    if (has_battery) return true;
#endif
    return std::nullopt;
}

} // namespace

struct SystemTelemetry::Impl {
    std::chrono::steady_clock::time_point previous_at=std::chrono::steady_clock::now();
#ifdef _WIN32
    std::uint64_t previous_idle=0,previous_kernel=0,previous_user=0,previous_process=0;
    bool have_previous=false;
#elif defined(__linux__)
    std::optional<CpuTimes> previous_cpu=read_system_cpu();
    std::optional<std::uint64_t> previous_process_ticks;
#endif
    std::filesystem::path energy_path;
    std::optional<std::pair<std::chrono::steady_clock::time_point,std::uint64_t>> previous_energy;

    Impl() {
#ifdef __linux__
        const std::filesystem::path root("/sys/class/powercap");
        std::error_code ec;
        if(std::filesystem::exists(root,ec)) {
            for(std::filesystem::recursive_directory_iterator it(root,ec),end;!ec&&it!=end;it.increment(ec)) {
                if(it->path().filename()=="energy_uj"&&it->is_regular_file(ec)) {energy_path=it->path();break;}
            }
        }
#endif
    }
    std::optional<double> package_power() {
#ifdef __linux__
        if(energy_path.empty()) return std::nullopt;
        std::ifstream input(energy_path); std::uint64_t energy=0;
        if(!(input>>energy)) return std::nullopt;
        const auto now=std::chrono::steady_clock::now();
        std::optional<double> watts;
        if(previous_energy) {
            const auto elapsed=std::chrono::duration<double>(now-previous_energy->first).count();
            if(elapsed>0.0&&energy>=previous_energy->second)
                watts=static_cast<double>(energy-previous_energy->second)/1'000'000.0/elapsed;
        }
        previous_energy=std::pair{now,energy};
        if(watts&&std::isfinite(*watts)&&*watts>=0.0) return watts;
#endif
        return std::nullopt;
    }
};

SystemTelemetry::SystemTelemetry():impl_(std::make_unique<Impl>()){}
SystemTelemetry::~SystemTelemetry()=default;
SystemTelemetry::SystemTelemetry(SystemTelemetry&&) noexcept=default;
SystemTelemetry& SystemTelemetry::operator=(SystemTelemetry&&) noexcept=default;

SystemSample SystemTelemetry::sample() {
    SystemSample result;
#ifdef _WIN32
    FILETIME idle{},kernel{},user{},created{},exited{},pk{},pu{};
    if(GetSystemTimes(&idle,&kernel,&user)!=0&&
       GetProcessTimes(GetCurrentProcess(),&created,&exited,&pk,&pu)!=0) {
        const auto i=filetime_value(idle),k=filetime_value(kernel),u=filetime_value(user);
        const auto p=filetime_value(pk)+filetime_value(pu);
        if(impl_->have_previous) {
            const auto total=(k-impl_->previous_kernel)+(u-impl_->previous_user);
            const auto idle_delta=i-impl_->previous_idle,process_delta=p-impl_->previous_process;
            if(total) {
                result.system_cpu_percent=std::clamp(100.0*static_cast<double>(total-std::min(total,idle_delta))/total,0.0,100.0);
                result.process_cpu_percent=std::clamp(100.0*static_cast<double>(std::min(total,process_delta))/total,0.0,100.0);
            }
        }
        impl_->previous_idle=i;impl_->previous_kernel=k;impl_->previous_user=u;impl_->previous_process=p;impl_->have_previous=true;
    }
    MEMORYSTATUSEX memory{};memory.dwLength=sizeof(memory);
    if(GlobalMemoryStatusEx(&memory)!=0) result.memory_used_bytes=memory.ullTotalPhys-memory.ullAvailPhys;
    PROCESS_MEMORY_COUNTERS_EX pm{};
    if(GetProcessMemoryInfo(GetCurrentProcess(),reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pm),sizeof(pm))!=0)
        result.process_memory_bytes=pm.WorkingSetSize;
#elif defined(__linux__)
    const auto cpu=read_system_cpu();
    if(cpu&&impl_->previous_cpu) {
        const auto total=cpu->total-impl_->previous_cpu->total,idle=cpu->idle-impl_->previous_cpu->idle;
        if(total) result.system_cpu_percent=std::clamp(100.0*static_cast<double>(total-std::min(total,idle))/total,0.0,100.0);
    }
    impl_->previous_cpu=cpu;
    if(const auto process=read_process_cpu_and_rss()) {
        result.process_memory_bytes=process->second;
        if(impl_->previous_process_ticks) {
            const auto now=std::chrono::steady_clock::now();
            const auto elapsed=std::chrono::duration<double>(now-impl_->previous_at).count();
            const auto ticks=process->first-*impl_->previous_process_ticks;
            const auto hz=static_cast<double>(::sysconf(_SC_CLK_TCK));
            const auto logical=std::max(1u,std::thread::hardware_concurrency());
            if(elapsed>0.0&&hz>0.0) result.process_cpu_percent=std::clamp(100.0*static_cast<double>(ticks)/hz/elapsed/logical,0.0,100.0);
        }
        impl_->previous_process_ticks=process->first;
    }
    std::ifstream mem("/proc/meminfo");std::string key,unit;std::uint64_t value=0,total_kb=0,available_kb=0;
    while(mem>>key>>value>>unit) {
        if(key=="MemTotal:") total_kb=value;
        else if(key=="MemAvailable:") available_kb=value;
        if(total_kb&&available_kb) break;
    }
    if(total_kb>=available_kb&&total_kb) result.memory_used_bytes=(total_kb-available_kb)*1024;
#endif
    result.cpu_frequency_mhz=read_cpu_frequency_mhz();
    result.temperature_c=read_temperature();
    result.package_power_watts=impl_->package_power();
    result.on_battery=read_on_battery();
    impl_->previous_at=std::chrono::steady_clock::now();
    return result;
}
} // namespace unique_rx::telemetry
