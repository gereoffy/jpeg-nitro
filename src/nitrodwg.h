// SPDX-License-Identifier: MIT
// Copyright (c) 2026 A'rpi - part of jpeg-nitro (https://github.com/gereoffy/jpeg-nitro)
//
// nitrodwg: AutoCAD DWG drawings (R13 - R2018, not R2007) for nitrodxf (internal, C++). The
// objects are decoded into the group code / value pairs a DXF of the same drawing would have, in
// a DXF's order (header, tables, blocks, entities, layouts), so nitrodxf reads them as it reads a
// DXF.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string>
#include <vector>

struct nitrodwg_pairs {
    std::vector<int> code;       // (0: a record's type)
    std::vector<uint32_t> end;   // the values: text[end[i - 1], end[i])
    std::string text;            // (a real: '\1' and its 8 bytes, as nitrodxf's num() reads it)
};

// false if it is not a DWG of a version read here
bool nitrodwg_read(const void *data, size_t len, nitrodwg_pairs &out);
