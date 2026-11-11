// FreeType's options for nitropdf (FT_CONFIG_OPTIONS_H, see the Makefile): its own defaults, less
// what reading outlines doesn't use - zlib (WOFF fonts), the TrueType bytecode interpreter (the
// glyphs are loaded unhinted) - so that nothing outside the modules of ftmodule.h is linked.
#include <freetype/config/ftoption.h>
#undef FT_CONFIG_OPTION_USE_ZLIB
#undef TT_CONFIG_OPTION_BYTECODE_INTERPRETER
#undef TT_CONFIG_OPTION_SUBPIXEL_HINTING
