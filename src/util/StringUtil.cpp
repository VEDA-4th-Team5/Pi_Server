#include "util/StringUtil.hpp"

#include <sstream>

namespace util {

std::string jsonEscape(const std::string& input) {
    std::ostringstream oss;

    for (char c : input) {
        switch (c) {
            case '\\': oss << "\\\\"; break;
            case '"':  oss << "\\\""; break;
            case '\n': oss << "\\n"; break;
            case '\r': oss << "\\r"; break;
            case '\t': oss << "\\t"; break;
            default:   oss << c; break;
        }
    }

    return oss.str();
}

}
