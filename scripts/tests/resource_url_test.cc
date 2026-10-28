#include "resource_url.h"
#include <cassert>

int main() {
    const auto lan = ivy::ResourceOrigin("http://192.168.199.162:8003/ivy/ota/");
    const auto wan = ivy::ResourceOrigin("https://xiaozhi.zillionairetravel.com/ivy/ota/?v=1");
    assert(lan == "http://192.168.199.162:8003");
    assert(wan == "https://xiaozhi.zillionairetravel.com");
    for (const auto* path : {"/api/study/card", "/api/study/course/find?text=FaceFonics",
                             "/api/study/gif?key=kc-001", "/api/music/next?count=1"}) {
        assert(ivy::JoinResourceUrl(lan, path) == lan + path);
        assert(ivy::JoinResourceUrl(wan, path) == wan + path);
    }
    assert(ivy::JoinResourceUrl("", "/api/music/next").empty());
    assert(ivy::JoinResourceUrl(wan + "/wrong", "/api/music/next").empty());
    assert(ivy::JoinResourceUrl(wan, "relative").empty());
    assert(ivy::ResourceOrigin("https://user:secret@host/ota/").empty());
    assert(ivy::ResourceOrigin("file:///tmp/ota").empty());
    assert(ivy::JoinResourceUrl(lan, "https://audio.example/song.mp3") ==
           "https://audio.example/song.mp3");
}
