// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jérôme Soler — JSG Guitar
#pragma once
/* Chemins de fichiers <-> texte UTF-8 (C++20 : u8string() donne des char8_t). */
#include <filesystem>
#include <string>

namespace jsg
{
inline std::string toUtf8 (const std::filesystem::path& p)
{
    const auto s = p.u8string();
    return std::string (s.begin(), s.end());
}

inline std::filesystem::path fromUtf8 (const std::string& s)
{
    return std::filesystem::path (std::u8string (s.begin(), s.end()));
}
}
