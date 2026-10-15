#pragma once

#include <cstddef>

// Fonts from third_party/, embedded into the binary at build time
// (see netviz_embed_file in cmake/EmbedFile.cmake).
extern const unsigned char inter_ttf[]; // UI text
extern const std::size_t inter_ttf_size;
extern const unsigned char jetbrains_mono_ttf[]; // console / monospace
extern const std::size_t jetbrains_mono_ttf_size;
extern const unsigned char lucide_ttf[]; // icons
extern const std::size_t lucide_ttf_size;

// UTF-8 strings for Lucide icons, usable directly in ImGui labels.
// Codepoints come from third_party/lucide/codepoints.json.
namespace netviz::icons {

inline constexpr const char* kOpen = "\xee\x83\x97";      // folder     U+E0D7
inline constexpr const char* kReload = "\xee\x85\x85";    // refresh-cw U+E145
inline constexpr const char* kZoomIn = "\xee\x86\xb6";    // zoom-in    U+E1B6
inline constexpr const char* kZoomOut = "\xee\x86\xb7";   // zoom-out   U+E1B7
inline constexpr const char* kFile = "\xee\x83\x8c";      // file-text       U+E0CC
inline constexpr const char* kModules = "\xee\x8b\x90";   // boxes           U+E2D0
inline constexpr const char* kPointer = "\xee\x87\x83";   // mouse-pointer-2 U+E1C3
inline constexpr const char* kZoom = "\xee\x85\x91";      // search          U+E151
inline constexpr const char* kMemory = "\xee\x91\x85";    // memory-stick    U+E445
inline constexpr const char* kTimer = "\xee\x87\xa0";     // timer           U+E1E0
inline constexpr const char* kCancel = "\xee\x86\xb2";    // x         U+E1B2
inline constexpr const char* kResetView = "\xee\x89\x97"; // scan       U+E257

} // namespace netviz::icons
