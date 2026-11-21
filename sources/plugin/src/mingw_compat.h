// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jérôme Soler — JSG Guitar
#pragma once
/*
    Compilation avec MinGW / zig seulement (pas avec Visual Studio) : la bibliothèque C de MinGW
    n'a pas aligned_alloc, utilisé par un fichier du kit VST3 (dataexchange, que JSG Guitar
    n'utilise pas). On le fournit pour que le kit compile.
*/
#if defined(__MINGW32__)
 #include <stddef.h>
 #include <malloc.h>
 static inline void* aligned_alloc (size_t alignment, size_t size) { return _aligned_malloc (size, alignment); }
#endif
