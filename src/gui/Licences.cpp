// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#include "Licences.h"

#include <cstddef>
#include <cstdint>

// The texts, embedded by src/gui/CMakeLists.txt (cmake/Bin2Cpp.cmake).
#define FRUIT_LICENCE(id)                     \
    extern const std::uint8_t kLicence_##id[]; \
    extern const std::size_t kLicence_##id##_size;
FRUIT_LICENCE(fruitslicer)
FRUIT_LICENCE(clipper2)
FRUIT_LICENCE(manifold)
FRUIT_LICENCE(json)
FRUIT_LICENCE(httplib)
FRUIT_LICENCE(miniz)
FRUIT_LICENCE(pugixml)
FRUIT_LICENCE(sdl3)
FRUIT_LICENCE(sdl3_yuv2rgb)
FRUIT_LICENCE(imgui)
FRUIT_LICENCE(glad)
FRUIT_LICENCE(inter)
#undef FRUIT_LICENCE

namespace gui {

namespace {
std::string_view view(const std::uint8_t* p, std::size_t n) { return {reinterpret_cast<const char*>(p), n}; }
}  // namespace

#define FRUIT_TEXT(id) view(kLicence_##id, kLicence_##id##_size)

const std::vector<Licence>& licences() {
    static const std::vector<Licence> all = {
        {"FruitSlicer", "this app", "GNU AGPL 3.0 or later", FRUIT_TEXT(fruitslicer)},
        {"Clipper2", "polygon clipping", "Boost Software License 1.0", FRUIT_TEXT(clipper2)},
        {"Manifold", "joining, cutting and labelling models", "Apache-2.0", FRUIT_TEXT(manifold)},
        {"nlohmann/json", "JSON files", "MIT", FRUIT_TEXT(json)},
        {"cpp-httplib", "sending over Wi-Fi", "MIT", FRUIT_TEXT(httplib)},
        {"miniz", "compression, 3MF and job files", "MIT", FRUIT_TEXT(miniz)},
        {"pugixml", "3MF models", "MIT", FRUIT_TEXT(pugixml)},
        {"SDL 3", "window, input and file dialogs", "zlib", FRUIT_TEXT(sdl3)},
        {"SDL 3: YUV to RGB", "part of SDL, built in with it", "BSD 3-Clause", FRUIT_TEXT(sdl3_yuv2rgb)},
        {"Dear ImGui", "interface", "MIT", FRUIT_TEXT(imgui)},
        {"glad", "OpenGL loader", "(WTFPL or CC0-1.0) and Apache-2.0", FRUIT_TEXT(glad)},
        {"Inter", "typeface", "SIL Open Font License 1.1", FRUIT_TEXT(inter)},
    };
    return all;
}

}  // namespace gui
