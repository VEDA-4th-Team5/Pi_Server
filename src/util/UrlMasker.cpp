#include "util/UrlMasker.hpp"

namespace util {

std::string hideUrlForLog(const std::string& url) {
    std::string marker = "://";
    std::size_t scheme_pos = url.find(marker);

    if (scheme_pos == std::string::npos) {
        return "[hidden]";
    }

    std::size_t auth_start = scheme_pos + marker.size();
    std::size_t at_pos = url.find('@', auth_start);

    if (at_pos == std::string::npos) {
        return url;
    }

    return url.substr(0, auth_start) + "***:***" + url.substr(at_pos);
}

}
