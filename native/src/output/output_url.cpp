#include <string>

#include "output/output.hpp"

namespace mixit::output {

// Stream keys and passwords must not reach logs or error messages
// (CLAUDE.md §16.F). Keeps scheme, host and port; hides user info, the last
// path segment of RTMP urls (the stream key) and every query value.
std::string redact_url(const std::string& url) {
    const std::size_t scheme_end = url.find("://");
    if (scheme_end == std::string::npos) return url;  // a file path
    std::string scheme = url.substr(0, scheme_end);
    for (char& c : scheme) c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    std::string rest = url.substr(scheme_end + 3);
    std::string query;
    if (const std::size_t q = rest.find('?'); q != std::string::npos) {
        std::string params = rest.substr(q + 1);
        rest.resize(q);
        std::size_t pos = 0;
        while (pos <= params.size()) {
            std::size_t end = params.find('&', pos);
            if (end == std::string::npos) end = params.size();
            const std::string kv = params.substr(pos, end - pos);
            const std::size_t eq = kv.find('=');
            if (!kv.empty()) query += (query.empty() ? "?" : "&") + (eq == std::string::npos ? kv : kv.substr(0, eq) + "=***");
            pos = end + 1;
        }
    }
    std::string authority = rest;
    std::string path;
    if (const std::size_t slash = rest.find('/'); slash != std::string::npos) {
        authority = rest.substr(0, slash);
        path = rest.substr(slash);
    }
    if (const std::size_t at = authority.rfind('@'); at != std::string::npos) authority = "***@" + authority.substr(at + 1);
    if ((scheme == "rtmp" || scheme == "rtmps") && !path.empty()) {
        const std::size_t last = path.rfind('/');
        if (last != std::string::npos && last + 1 < path.size()) path = path.substr(0, last + 1) + "***";
    }
    return scheme + "://" + authority + path + query;
}

}  // namespace mixit::output
