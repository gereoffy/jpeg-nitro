// SPDX-License-Identifier: MIT
// Copyright (c) 2026 A'rpi - part of jpeg-nitro (https://github.com/gereoffy/jpeg-nitro)
//
// nitroai: Adobe Illustrator 1.0-8.0 documents (the PostScript ones, also as EPS: Illustrator 9 and
// later save a PDF, which nitropdf reads as it is; their EPS: the document in this format that it keeps
// at its end), turned into a PDF of one page for nitropdf.
#pragma once
#include <stddef.h>
#include <string>

// Whether the data looks like such a document (its first bytes are enough).
bool nitroai_is(const void *data, size_t len);
// The document as a PDF; empty if it is not one.
std::string nitroai_pdf(const void *data, size_t len);
