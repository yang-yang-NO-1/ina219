#pragma once
#include "battery_soc.h"
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace battery {
// Persist ONLY initialized estimates. A small, versioned text format keeps
// this independent of external JSON libraries. Path should be a local trusted file.
inline bool parseSocState(const std::string& data, double expectedCapacity,
                          double& remainingMah, bool& uncertain) {
    std::istringstream lines(data);
    std::string line, capacityText, remainingText, uncertainText;
    if (!std::getline(lines, line) || line != "INA219_SOC_V1") return false;
    while (std::getline(lines, line)) {
        size_t p = line.find('=');
        if (p == std::string::npos) return false;
        std::string key=line.substr(0,p), value=line.substr(p+1);
        if (key=="capacity_mah") capacityText=value;
        else if (key=="remaining_mah") remainingText=value;
        else if (key=="uncertain") uncertainText=value;
        else if (key=="saved_at_unix_ms") {} // metadata only
        else return false;
    }
    auto parse = [](const std::string& s, double& out) {
        if (s.empty()) return false;
        errno=0;
        char* tail=nullptr;
        out = std::strtod(s.c_str(), &tail);
        return !errno && tail && *tail=='\0' && std::isfinite(out);
    };
    double cap = 0;
    if (!parse(capacityText, cap) || !parse(remainingText, remainingMah)) return false;
    if (std::fabs(expectedCapacity-cap) > 0.001 || remainingMah < 0 || remainingMah > cap) return false;
    if (uncertainText != "0" && uncertainText != "1") return false;
    uncertain = uncertainText=="1";
    return true;
}
inline bool loadSocState(const std::string& path, double capacity, double& remainingMah, bool& uncertain) {
    FILE* f = std::fopen(path.c_str(), "r");
    if (!f) return false;
    char buf[512];
    size_t count=std::fread(buf, 1, sizeof(buf), f);
    int over=std::fgetc(f);
    bool ok=std::ferror(f)==0 && over==EOF && count>0;
    std::fclose(f);
    return ok && parseSocState(std::string(buf, count), capacity, remainingMah, uncertain);
}
inline std::string formatSocState(const SocInfo& info, uint64_t savedAtUnixMs) {
    char buf[256];
    std::snprintf(buf, sizeof(buf),
      "INA219_SOC_V1\ncapacity_mah=%.6f\nremaining_mah=%.6f\nuncertain=%d\nsaved_at_unix_ms=%llu\n",
      info.nominal_capacity_mah, info.remaining_capacity_mah, info.uncertain?1:0,
      static_cast<unsigned long long>(savedAtUnixMs));
    return buf;
}
inline bool saveSocState(const std::string& path, const SocInfo& info, uint64_t savedAtUnixMs) {
    if (path.empty() || !info.available) return false;
    std::string tmp=path+".tmp.XXXXXX";
    int fd=::mkstemp(&tmp[0]);
    if (fd<0) return false;
    (void)::fchmod(fd, 0600);
    const std::string body=formatSocState(info, savedAtUnixMs);
    size_t pos=0;
    while(pos<body.size()) {
        ssize_t n=::write(fd, body.data()+pos, body.size()-pos);
        if(n<0 && errno==EINTR) continue;
        if(n<=0) { ::close(fd); ::unlink(tmp.c_str()); return false; }
        pos+=static_cast<size_t>(n);
    }
    bool ok=(::fsync(fd)==0);
    if (::close(fd)!=0) ok=false;
    if (ok) ok = (::rename(tmp.c_str(), path.c_str())==0);
    if (!ok) ::unlink(tmp.c_str());
    if (ok) {
        const std::string parent = path.find('/') == std::string::npos ? "." :
            (path.find_last_of('/') == 0 ? "/" : path.substr(0,path.find_last_of('/')));
        int dirfd=::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (dirfd>=0) { (void)::fsync(dirfd); ::close(dirfd); }
    }
    return ok;
}
} // namespace battery
