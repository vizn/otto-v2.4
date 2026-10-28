#pragma once

#include <string>

namespace ivy {
inline std::string ResourceOrigin(const std::string& url) {
    size_t start = url.rfind("https://", 0) == 0 ? 8 : url.rfind("http://", 0) == 0 ? 7 : 0;
    if (!start)
        return "";
    auto end = url.find_first_of("/?#", start);
    auto authority = url.substr(start, end == std::string::npos ? end : end - start);
    if (authority.empty() || authority.find_first_of("@ \r\n\t") != std::string::npos)
        return "";
    return url.substr(0, start) + authority;
}

std::string ResourceUrl(const std::string& path);

inline std::string JoinResourceUrl(const std::string& origin, const std::string& path) {
    if (!ResourceOrigin(path).empty())
        return path;
    if (origin.empty() || ResourceOrigin(origin) != origin || path.empty() || path.front() != '/')
        return "";
    return origin + path;
}
}  // namespace ivy
