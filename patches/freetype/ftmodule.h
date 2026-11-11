// The FreeType modules nitropdf needs (FT_CONFIG_MODULES_H, see the Makefile): the fonts a PDF
// embeds - TrueType / OpenType (sfnt), Type 1, CFF (bare and CID-keyed), CID Type 1 - read for
// their glyphs' outlines only: no hinter, no rasterizer (plutovg draws them), no compression.
FT_USE_MODULE( FT_Driver_ClassRec, tt_driver_class )
FT_USE_MODULE( FT_Driver_ClassRec, t1_driver_class )
FT_USE_MODULE( FT_Driver_ClassRec, cff_driver_class )
FT_USE_MODULE( FT_Driver_ClassRec, t1cid_driver_class )
FT_USE_MODULE( FT_Module_Class, psaux_module_class )
FT_USE_MODULE( FT_Module_Class, psnames_module_class )
FT_USE_MODULE( FT_Module_Class, sfnt_module_class )
