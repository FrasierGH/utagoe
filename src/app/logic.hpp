// Non-visual parts of the main window (TForm1).
#pragma once

#include <string>

namespace utagoe {

// 0x4058a8: find the instrumental next to `path` and name the output file.
// Empty strings when nothing is found / the feature is off.
void auto_names(const std::wstring& path, bool kname_flg, bool vname_flg, const std::wstring& suffix,
                std::wstring* inst, std::wstring* out);

// 0x40c450: strip characters that are not allowed in file names.
std::wstring sanitize_suffix(const std::wstring& text);

}  // namespace utagoe
