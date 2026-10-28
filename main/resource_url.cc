#include "resource_url.h"

#include "settings.h"

std::string ivy::ResourceUrl(const std::string& path) {
    if (!ResourceOrigin(path).empty())
        return path;
    Settings settings("resources", false);
    auto origin = settings.GetString("base_url");
    return JoinResourceUrl(origin, path);
}
