/* C++ code produced by gperf version 3.0.3 */
/* Command-line: /Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin/gperf --compare-strncmp -C -m 20 tokens.gperf  */
/* Computed positions: -k'1,3-5,14,$' */

#if !((' ' == 32) && ('!' == 33) && ('"' == 34) && ('#' == 35) \
      && ('%' == 37) && ('&' == 38) && ('\'' == 39) && ('(' == 40) \
      && (')' == 41) && ('*' == 42) && ('+' == 43) && (',' == 44) \
      && ('-' == 45) && ('.' == 46) && ('/' == 47) && ('0' == 48) \
      && ('1' == 49) && ('2' == 50) && ('3' == 51) && ('4' == 52) \
      && ('5' == 53) && ('6' == 54) && ('7' == 55) && ('8' == 56) \
      && ('9' == 57) && (':' == 58) && (';' == 59) && ('<' == 60) \
      && ('=' == 61) && ('>' == 62) && ('?' == 63) && ('A' == 65) \
      && ('B' == 66) && ('C' == 67) && ('D' == 68) && ('E' == 69) \
      && ('F' == 70) && ('G' == 71) && ('H' == 72) && ('I' == 73) \
      && ('J' == 74) && ('K' == 75) && ('L' == 76) && ('M' == 77) \
      && ('N' == 78) && ('O' == 79) && ('P' == 80) && ('Q' == 81) \
      && ('R' == 82) && ('S' == 83) && ('T' == 84) && ('U' == 85) \
      && ('V' == 86) && ('W' == 87) && ('X' == 88) && ('Y' == 89) \
      && ('Z' == 90) && ('[' == 91) && ('\\' == 92) && (']' == 93) \
      && ('^' == 94) && ('_' == 95) && ('a' == 97) && ('b' == 98) \
      && ('c' == 99) && ('d' == 100) && ('e' == 101) && ('f' == 102) \
      && ('g' == 103) && ('h' == 104) && ('i' == 105) && ('j' == 106) \
      && ('k' == 107) && ('l' == 108) && ('m' == 109) && ('n' == 110) \
      && ('o' == 111) && ('p' == 112) && ('q' == 113) && ('r' == 114) \
      && ('s' == 115) && ('t' == 116) && ('u' == 117) && ('v' == 118) \
      && ('w' == 119) && ('x' == 120) && ('y' == 121) && ('z' == 122) \
      && ('{' == 123) && ('|' == 124) && ('}' == 125) && ('~' == 126))
/* The character set is not based on ISO-646.  */
#error "gperf generated tables don't work with this execution character set. Please report a bug to <bug-gnu-gperf@gnu.org>."
#endif

#line 5 "tokens.gperf"
struct xmltoken
{
  const char *name;
  int tokenId;
};

#define TOTAL_KEYWORDS 317
#define MIN_WORD_LENGTH 1
#define MAX_WORD_LENGTH 31
#define MIN_HASH_VALUE 5
#define MAX_HASH_VALUE 847
/* maximum key range = 843, duplicates = 0 */

class Perfect_Hash
{
private:
  static inline unsigned int hash (const char *str, unsigned int len);
public:
  static const struct xmltoken *in_word_set (const char *str, unsigned int len);
};

inline unsigned int
Perfect_Hash::hash (const char *str, unsigned int len)
{
  static const unsigned short asso_values[] =
    {
      848, 848, 848, 848, 848, 848, 848, 848, 848, 848,
      848, 848, 848, 848, 848, 848, 848, 848, 848, 848,
      848, 848, 848, 848, 848, 848, 848, 848, 848, 848,
      848, 848, 848, 848, 848, 848, 848, 848, 848, 848,
      848, 848, 848, 848, 848, 848, 848, 848, 848, 224,
      101,  50,  35,  23,  11,   8, 848, 848,   8, 848,
      848, 848, 848, 848, 848, 215,   2,  98, 188, 227,
       26, 167, 125, 164,   2,   2, 107, 158, 143, 167,
       38, 122, 185,  47,  44,  68, 191,  38,  83, 131,
      848, 848, 848, 848, 848, 848, 848,   8, 131,  32,
       11,   2,  35,  20, 164,   2,  38,  98,   8, 119,
        8,  41, 119, 848,  20,  38,  11, 173,   2, 269,
      128,  65,   2, 848, 848, 848, 848, 848, 848, 848,
      848, 848, 848, 848, 848, 848, 848, 848, 848, 848,
      848, 848, 848, 848, 848, 848, 848, 848, 848, 848,
      848, 848, 848, 848, 848, 848, 848, 848, 848, 848,
      848, 848, 848, 848, 848, 848, 848, 848, 848, 848,
      848, 848, 848, 848, 848, 848, 848, 848, 848, 848,
      848, 848, 848, 848, 848, 848, 848, 848, 848, 848,
      848, 848, 848, 848, 848, 848, 848, 848, 848, 848,
      848, 848, 848, 848, 848, 848, 848, 848, 848, 848,
      848, 848, 848, 848, 848, 848, 848, 848, 848, 848,
      848, 848, 848, 848, 848, 848, 848, 848, 848, 848,
      848, 848, 848, 848, 848, 848, 848, 848, 848, 848,
      848, 848, 848, 848, 848, 848, 848, 848, 848, 848,
      848, 848, 848, 848, 848, 848
    };
  unsigned int hval = len;

  switch (hval)
    {
      default:
        hval += asso_values[(unsigned char)str[13]];
      /*FALLTHROUGH*/
      case 13:
      case 12:
      case 11:
      case 10:
      case 9:
      case 8:
      case 7:
      case 6:
      case 5:
        hval += asso_values[(unsigned char)str[4]];
      /*FALLTHROUGH*/
      case 4:
        hval += asso_values[(unsigned char)str[3]];
      /*FALLTHROUGH*/
      case 3:
        hval += asso_values[(unsigned char)str[2]];
      /*FALLTHROUGH*/
      case 2:
      case 1:
        hval += asso_values[(unsigned char)str[0]];
        break;
    }
  return hval + asso_values[(unsigned char)str[len - 1]];
}

static const struct xmltoken wordlist[] =
  {
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
    {(char*)0, 0},
#line 17 "tokens.gperf"
    {"B",XML_B},
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
#line 246 "tokens.gperf"
    {"a:ea",XML_A_EA},
    {(char*)0, 0}, {(char*)0, 0},
#line 323 "tokens.gperf"
    {"vt:varStyle",XML_VT_VARSTYLE},
#line 312 "tokens.gperf"
    {"vt:lineStyle",XML_VT_LINESTYLE},
    {(char*)0, 0},
#line 264 "tokens.gperf"
    {"a:ln",XML_A_LN},
#line 24 "tokens.gperf"
    {"Bullet",XML_BULLET},
#line 26 "tokens.gperf"
    {"BulletFontSize",XML_BULLETFONTSIZE},
#line 263 "tokens.gperf"
    {"a:lin",XML_A_LIN},
#line 322 "tokens.gperf"
    {"vt:varColor7",XML_VT_VARCOLOR7},
#line 25 "tokens.gperf"
    {"BulletFont",XML_BULLETFONT},
#line 293 "tokens.gperf"
    {"dc:title",XML_DC_TITLE},
#line 321 "tokens.gperf"
    {"vt:varColor6",XML_VT_VARCOLOR6},
    {(char*)0, 0},
#line 292 "tokens.gperf"
    {"dc:template",XML_DC_TEMPLATE},
#line 282 "tokens.gperf"
    {"a:tint",XML_A_TINT},
    {(char*)0, 0},
#line 290 "tokens.gperf"
    {"dc:language",XML_DC_LANGUAGE},
#line 27 "tokens.gperf"
    {"BulletStr",XML_BULLETSTR},
#line 262 "tokens.gperf"
    {"a:latin",XML_A_LATIN},
#line 324 "tokens.gperf"
    {"vt:variationClrScheme",XML_VT_VARIATIONCLRSCHEME},
    {(char*)0, 0}, {(char*)0, 0},
#line 62 "tokens.gperf"
    {"Fill",XML_FILL},
#line 320 "tokens.gperf"
    {"vt:varColor5",XML_VT_VARCOLOR5},
#line 326 "tokens.gperf"
    {"vt:variationStyleScheme",XML_VT_VARIATIONSTYLESCHEME},
#line 185 "tokens.gperf"
    {"Size",XML_SIZE},
    {(char*)0, 0}, {(char*)0, 0},
#line 75 "tokens.gperf"
    {"Font",XML_FONT},
    {(char*)0, 0},
#line 289 "tokens.gperf"
    {"dc:description",XML_DC_DESCRIPTION},
#line 325 "tokens.gperf"
    {"vt:variationClrSchemeLst",XML_VT_VARIATIONCLRSCHEMELST},
#line 63 "tokens.gperf"
    {"FillBkgnd",XML_FILLBKGND},
#line 78 "tokens.gperf"
    {"Foreign",XML_FOREIGN},
#line 137 "tokens.gperf"
    {"Page",XML_PAGE},
#line 319 "tokens.gperf"
    {"vt:varColor4",XML_VT_VARCOLOR4},
#line 327 "tokens.gperf"
    {"vt:variationStyleSchemeLst",XML_VT_VARIATIONSTYLESCHEMELST},
#line 79 "tokens.gperf"
    {"ForeignData",XML_FOREIGNDATA},
    {(char*)0, 0},
#line 313 "tokens.gperf"
    {"vt:lineStyles",XML_VT_LINESTYLES},
#line 294 "tokens.gperf"
    {"dcterms:created",XML_DCTERMS_CREATED},
#line 295 "tokens.gperf"
    {"dcterms:modified",XML_DCTERMS_MODIFIED},
#line 258 "tokens.gperf"
    {"a:gradFill",XML_A_GRADFILL},
#line 151 "tokens.gperf"
    {"Print",XML_PRINT},
    {(char*)0, 0}, {(char*)0, 0},
#line 144 "tokens.gperf"
    {"Para",XML_PARA},
    {(char*)0, 0}, {(char*)0, 0},
#line 242 "tokens.gperf"
    {"a:clrScheme",XML_A_CLRSCHEME},
#line 318 "tokens.gperf"
    {"vt:varColor3",XML_VT_VARCOLOR3},
    {(char*)0, 0},
#line 274 "tokens.gperf"
    {"a:satMod",XML_A_SATMOD},
#line 23 "tokens.gperf"
    {"BottomMargin",XML_BOTTOMMARGIN},
#line 191 "tokens.gperf"
    {"SplineKnot",XML_SPLINEKNOT},
#line 192 "tokens.gperf"
    {"SplineStart",XML_SPLINESTART},
    {(char*)0, 0},
#line 253 "tokens.gperf"
    {"a:fillStyleLst",XML_A_FILLSTYLELST},
#line 65 "tokens.gperf"
    {"FillForegnd",XML_FILLFOREGND},
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
#line 265 "tokens.gperf"
    {"a:lnStyleLst",XML_A_LNSTYLELST},
#line 248 "tokens.gperf"
    {"a:effectStyle",XML_A_EFFECTSTYLE},
#line 189 "tokens.gperf"
    {"SpBefore",XML_SPBEFORE},
#line 301 "tokens.gperf"
    {"vt:fillProps",XML_VT_FILLPROPS},
#line 302 "tokens.gperf"
    {"vt:fillStyles",XML_VT_FILLSTYLES},
#line 71 "tokens.gperf"
    {"FillPattern",XML_FILLPATTERN},
#line 239 "tokens.gperf"
    {"a:accent6",XML_A_ACCENT6},
#line 288 "tokens.gperf"
    {"dc:creator",XML_DC_CREATOR},
#line 247 "tokens.gperf"
    {"a:effectLst",XML_A_EFFECTLST},
#line 76 "tokens.gperf"
    {"FontScale",XML_FONTSCALE},
    {(char*)0, 0},
#line 150 "tokens.gperf"
    {"Position",XML_POSITION},
#line 257 "tokens.gperf"
    {"a:fontScheme",XML_A_FONTSCHEME},
#line 175 "tokens.gperf"
    {"Section",XML_SECTION},
#line 259 "tokens.gperf"
    {"a:gs",XML_A_GS},
#line 256 "tokens.gperf"
    {"a:font",XML_A_FONT},
    {(char*)0, 0},
#line 299 "tokens.gperf"
    {"vt:color",XML_VT_COLOR},
#line 238 "tokens.gperf"
    {"a:accent5",XML_A_ACCENT5},
    {(char*)0, 0},
#line 277 "tokens.gperf"
    {"a:solidFill",XML_A_SOLIDFILL},
#line 278 "tokens.gperf"
    {"a:srgbClr",XML_A_SRGBCLR},
#line 19 "tokens.gperf"
    {"BegTrigger",XML_BEGTRIGGER},
#line 149 "tokens.gperf"
    {"Pos",XML_POS},
#line 140 "tokens.gperf"
    {"PageScale",XML_PAGESCALE},
    {(char*)0, 0},
#line 243 "tokens.gperf"
    {"a:cs",XML_A_CS},
#line 21 "tokens.gperf"
    {"BeginX",XML_BEGINX},
    {(char*)0, 0},
#line 116 "tokens.gperf"
    {"Line",XML_LINE},
#line 237 "tokens.gperf"
    {"a:accent4",XML_A_ACCENT4},
    {(char*)0, 0},
#line 32 "tokens.gperf"
    {"Cell",XML_CELL},
#line 141 "tokens.gperf"
    {"PageSheet",XML_PAGESHEET},
    {(char*)0, 0},
#line 194 "tokens.gperf"
    {"Style",XML_STYLE},
#line 66 "tokens.gperf"
    {"FillForegndTrans",XML_FILLFOREGNDTRANS},
    {(char*)0, 0},
#line 297 "tokens.gperf"
    {"tp",XML_TP},
#line 317 "tokens.gperf"
    {"vt:varColor2",XML_VT_VARCOLOR2},
#line 64 "tokens.gperf"
    {"FillBkgndTrans",XML_FILLBKGNDTRANS},
#line 72 "tokens.gperf"
    {"Flags",XML_FLAGS},
#line 308 "tokens.gperf"
    {"vt:fontProps",XML_VT_FONTPROPS},
#line 309 "tokens.gperf"
    {"vt:fontStyles",XML_VT_FONTSTYLES},
#line 202 "tokens.gperf"
    {"TIBT",XML_TIBT},
#line 236 "tokens.gperf"
    {"a:accent3",XML_A_ACCENT3},
#line 287 "tokens.gperf"
    {"cp:lastModifiedBy",XML_CP_LASTMODIFIEDBY},
#line 143 "tokens.gperf"
    {"Pages",XML_PAGES},
    {(char*)0, 0},
#line 195 "tokens.gperf"
    {"StyleSheet",XML_STYLESHEET},
#line 31 "tokens.gperf"
    {"Case",XML_CASE},
#line 139 "tokens.gperf"
    {"PageProps",XML_PAGEPROPS},
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
#line 33 "tokens.gperf"
    {"Char",XML_CHAR},
    {(char*)0, 0},
#line 187 "tokens.gperf"
    {"SortKey",XML_SORTKEY},
#line 283 "tokens.gperf"
    {"cp",XML_CP},
#line 114 "tokens.gperf"
    {"Leader",XML_LEADER},
    {(char*)0, 0},
#line 284 "tokens.gperf"
    {"cp:category",XML_CP_CATEGORY},
#line 311 "tokens.gperf"
    {"vt:lineEx",XML_VT_LINEEX},
#line 240 "tokens.gperf"
    {"a:alpha",XML_A_ALPHA},
#line 270 "tokens.gperf"
    {"a:minorFont",XML_A_MINORFONT},
    {(char*)0, 0}, {(char*)0, 0},
#line 80 "tokens.gperf"
    {"Frame",XML_FRAME},
#line 34 "tokens.gperf"
    {"Character",XML_CHARACTER},
    {(char*)0, 0},
#line 250 "tokens.gperf"
    {"a:ext",XML_A_EXT},
    {(char*)0, 0},
#line 229 "tokens.gperf"
    {"X",XML_X},
#line 251 "tokens.gperf"
    {"a:extLst",XML_A_EXTLST},
#line 22 "tokens.gperf"
    {"BeginY",XML_BEGINY},
#line 148 "tokens.gperf"
    {"PolylineTo",XML_POLYLINETO},
#line 196 "tokens.gperf"
    {"StyleSheets",XML_STYLESHEETS},
#line 190 "tokens.gperf"
    {"SpLine",XML_SPLINE},
    {(char*)0, 0},
#line 119 "tokens.gperf"
    {"LinePattern",XML_LINEPATTERN},
    {(char*)0, 0},
#line 121 "tokens.gperf"
    {"LineWeight",XML_LINEWEIGHT},
#line 279 "tokens.gperf"
    {"a:sysClr",XML_A_SYSCLR},
    {(char*)0, 0},
#line 285 "tokens.gperf"
    {"cp:coreProperties",XML_CP_COREPROPERTIES},
    {(char*)0, 0},
#line 252 "tokens.gperf"
    {"a:extraClrSchemeLst",XML_A_EXTRACLRSCHEMELST},
    {(char*)0, 0},
#line 176 "tokens.gperf"
    {"Shape",XML_SHAPE},
#line 300 "tokens.gperf"
    {"vt:connectorFontStyles",XML_VT_CONNECTORFONTSTYLES},
#line 219 "tokens.gperf"
    {"TxtPinX",XML_TXTPINX},
#line 254 "tokens.gperf"
    {"a:fmtScheme",XML_A_FMTSCHEME},
    {(char*)0, 0}, {(char*)0, 0},
#line 286 "tokens.gperf"
    {"cp:keywords",XML_CP_KEYWORDS},
#line 235 "tokens.gperf"
    {"a:accent2",XML_A_ACCENT2},
#line 260 "tokens.gperf"
    {"a:gsLst",XML_A_GSLST},
#line 35 "tokens.gperf"
    {"Color",XML_COLOR},
#line 132 "tokens.gperf"
    {"NoFill",XML_NOFILL},
#line 280 "tokens.gperf"
    {"a:theme",XML_A_THEME},
#line 269 "tokens.gperf"
    {"a:majorFont",XML_A_MAJORFONT},
    {(char*)0, 0},
#line 28 "tokens.gperf"
    {"C",XML_C},
#line 206 "tokens.gperf"
    {"Text",XML_TEXT},
    {(char*)0, 0},
#line 255 "tokens.gperf"
    {"a:folHlink",XML_A_FOLHLINK},
#line 94 "tokens.gperf"
    {"HideText",XML_HIDETEXT},
#line 216 "tokens.gperf"
    {"TxtHeight",XML_TXTHEIGHT},
#line 315 "tokens.gperf"
    {"vt:themeScheme",XML_VT_THEMESCHEME},
#line 163 "tokens.gperf"
    {"Rel",XML_REL},
#line 207 "tokens.gperf"
    {"TextBkgnd",XML_TEXTBKGND},
#line 138 "tokens.gperf"
    {"PageHeight",XML_PAGEHEIGHT},
#line 136 "tokens.gperf"
    {"Overline",XML_OVERLINE},
#line 120 "tokens.gperf"
    {"LineTo",XML_LINETO},
    {(char*)0, 0}, {(char*)0, 0},
#line 38 "tokens.gperf"
    {"Colors",XML_COLORS},
    {(char*)0, 0},
#line 60 "tokens.gperf"
    {"FaceName",XML_FACENAME},
#line 249 "tokens.gperf"
    {"a:effectStyleLst",XML_A_EFFECTSTYLELST},
    {(char*)0, 0},
#line 146 "tokens.gperf"
    {"PinX",XML_PINX},
#line 106 "tokens.gperf"
    {"Invisible",XML_INVISIBLE},
    {(char*)0, 0},
#line 111 "tokens.gperf"
    {"Layer",XML_LAYER},
#line 179 "tokens.gperf"
    {"Shapes",XML_SHAPES},
    {(char*)0, 0},
#line 102 "tokens.gperf"
    {"IndFirst",XML_INDFIRST},
#line 105 "tokens.gperf"
    {"InfiniteLine",XML_INFINITELINE},
    {(char*)0, 0},
#line 113 "tokens.gperf"
    {"LayerMember",XML_LAYERMEMBER},
#line 307 "tokens.gperf"
    {"vt:fmtSchemeLineStyles",XML_VT_FMTSCHEMELINESTYLES},
#line 276 "tokens.gperf"
    {"a:shade",XML_A_SHADE},
#line 303 "tokens.gperf"
    {"vt:fmtConnectorScheme",XML_VT_FMTCONNECTORSCHEME},
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
#line 67 "tokens.gperf"
    {"FillGradient",XML_FILLGRADIENT},
#line 220 "tokens.gperf"
    {"TxtPinY",XML_TXTPINY},
#line 267 "tokens.gperf"
    {"a:lt2",XML_A_LT2},
#line 126 "tokens.gperf"
    {"Master",XML_MASTER},
#line 68 "tokens.gperf"
    {"FillGradientAngle",XML_FILLGRADIENTANGLE},
#line 77 "tokens.gperf"
    {"FontSchemeIndex",XML_FONTSCHEMEINDEX},
    {(char*)0, 0},
#line 169 "tokens.gperf"
    {"ResizeMode",XML_RESIZEMODE},
#line 296 "tokens.gperf"
    {"pp",XML_PP},
    {(char*)0, 0},
#line 36 "tokens.gperf"
    {"ColorEntry",XML_COLORENTRY},
    {(char*)0, 0},
#line 118 "tokens.gperf"
    {"LineColor",XML_LINECOLOR},
#line 226 "tokens.gperf"
    {"VerticalAlign",XML_VERTICALALIGN},
#line 69 "tokens.gperf"
    {"FillGradientDir",XML_FILLGRADIENTDIR},
#line 70 "tokens.gperf"
    {"FillGradientEnabled",XML_FILLGRADIENTENABLED},
#line 152 "tokens.gperf"
    {"Properties",XML_PROPERTIES},
#line 281 "tokens.gperf"
    {"a:themeElements",XML_A_THEMEELEMENTS},
#line 61 "tokens.gperf"
    {"FaceNames",XML_FACENAMES},
#line 291 "tokens.gperf"
    {"dc:subject",XML_DC_SUBJECT},
#line 15 "tokens.gperf"
    {"Angle",XML_ANGLE},
#line 129 "tokens.gperf"
    {"MoveTo",XML_MOVETO},
#line 127 "tokens.gperf"
    {"Masters",XML_MASTERS},
#line 204 "tokens.gperf"
    {"Tabs",XML_TABS},
#line 316 "tokens.gperf"
    {"vt:varColor1",XML_VT_VARCOLOR1},
#line 88 "tokens.gperf"
    {"GradientStopPosition",XML_GRADIENTSTOPPOSITION},
#line 298 "tokens.gperf"
    {"vt:bkgnd",XML_VT_BKGND},
#line 145 "tokens.gperf"
    {"Paragraph",XML_PARAGRAPH},
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
#line 232 "tokens.gperf"
    {"Y",XML_Y},
#line 128 "tokens.gperf"
    {"Misc",XML_MISC},
#line 14 "tokens.gperf"
    {"Alignment",XML_ALIGNMENT},
#line 86 "tokens.gperf"
    {"GradientStopColor",XML_GRADIENTSTOPCOLOR},
#line 221 "tokens.gperf"
    {"TxtWidth",XML_TXTWIDTH},
#line 133 "tokens.gperf"
    {"NoLine",XML_NOLINE},
#line 122 "tokens.gperf"
    {"LocPinX",XML_LOCPINX},
    {(char*)0, 0},
#line 142 "tokens.gperf"
    {"PageWidth",XML_PAGEWIDTH},
#line 161 "tokens.gperf"
    {"QuickStyleType",XML_QUICKSTYLETYPE},
#line 275 "tokens.gperf"
    {"a:schemeClr",XML_A_SCHEMECLR},
#line 305 "tokens.gperf"
    {"vt:fmtConnectorSchemeLineStyles",XML_VT_FMTCONNECTORSCHEMELINESTYLES},
#line 208 "tokens.gperf"
    {"TextBkgndTrans",XML_TEXTBKGNDTRANS},
    {(char*)0, 0},
#line 177 "tokens.gperf"
    {"ShapeShdwOffsetX",XML_SHAPESHDWOFFSETX},
    {(char*)0, 0},
#line 314 "tokens.gperf"
    {"vt:schemeID",XML_VT_SCHEMEID},
#line 271 "tokens.gperf"
    {"a:objectDefaults",XML_A_OBJECTDEFAULTS},
    {(char*)0, 0}, {(char*)0, 0},
#line 162 "tokens.gperf"
    {"QuickStyleVariation",XML_QUICKSTYLEVARIATION},
    {(char*)0, 0}, {(char*)0, 0},
#line 43 "tokens.gperf"
    {"DblUnderline",XML_DBLUNDERLINE},
#line 261 "tokens.gperf"
    {"a:hlink",XML_A_HLINK},
#line 215 "tokens.gperf"
    {"TxtAngle",XML_TXTANGLE},
#line 87 "tokens.gperf"
    {"GradientStopColorTrans",XML_GRADIENTSTOPCOLORTRANS},
    {(char*)0, 0},
#line 199 "tokens.gperf"
    {"TELU",XML_TELU},
#line 209 "tokens.gperf"
    {"TextBlock",XML_TEXTBLOCK},
#line 13 "tokens.gperf"
    {"Address",XML_ADDRESS},
    {(char*)0, 0},
#line 158 "tokens.gperf"
    {"QuickStyleLineColor",XML_QUICKSTYLELINECOLOR},
#line 217 "tokens.gperf"
    {"TxtLocPinX",XML_TXTLOCPINX},
#line 46 "tokens.gperf"
    {"Description",XML_DESCRIPTION},
    {(char*)0, 0}, {(char*)0, 0},
#line 205 "tokens.gperf"
    {"Template",XML_TEMPLATE},
#line 154 "tokens.gperf"
    {"QuickStyleFillColor",XML_QUICKSTYLEFILLCOLOR},
#line 103 "tokens.gperf"
    {"IndLeft",XML_INDLEFT},
    {(char*)0, 0},
#line 156 "tokens.gperf"
    {"QuickStyleFontColor",XML_QUICKSTYLEFONTCOLOR},
#line 213 "tokens.gperf"
    {"ThemeIndex",XML_THEMEINDEX},
#line 160 "tokens.gperf"
    {"QuickStyleShadowColor",XML_QUICKSTYLESHADOWCOLOR},
    {(char*)0, 0}, {(char*)0, 0},
#line 203 "tokens.gperf"
    {"Tab",XML_TAB},
    {(char*)0, 0},
#line 20 "tokens.gperf"
    {"BeginArrow",XML_BEGINARROW},
#line 147 "tokens.gperf"
    {"PinY",XML_PINY},
#line 234 "tokens.gperf"
    {"a:accent1",XML_A_ACCENT1},
    {(char*)0, 0}, {(char*)0, 0},
#line 59 "tokens.gperf"
    {"ExtraInfo",XML_EXTRAINFO},
#line 123 "tokens.gperf"
    {"LocPinY",XML_LOCPINY},
#line 73 "tokens.gperf"
    {"FlipX",XML_FLIPX},
#line 241 "tokens.gperf"
    {"a:bgFillStyleLst",XML_A_BGFILLSTYLELST},
    {(char*)0, 0},
#line 112 "tokens.gperf"
    {"LayerMem",XML_LAYERMEM},
#line 37 "tokens.gperf"
    {"ColorSchemeIndex",XML_COLORSCHEMEINDEX},
    {(char*)0, 0},
#line 245 "tokens.gperf"
    {"a:dk2",XML_A_DK2},
#line 178 "tokens.gperf"
    {"ShapeShdwOffsetY",XML_SHAPESHDWOFFSETY},
    {(char*)0, 0},
#line 268 "tokens.gperf"
    {"a:lumMod",XML_A_LUMMOD},
#line 93 "tokens.gperf"
    {"Height",XML_HEIGHT},
#line 115 "tokens.gperf"
    {"LeftMargin",XML_LEFTMARGIN},
    {(char*)0, 0},
#line 98 "tokens.gperf"
    {"ImgHeight",XML_IMGHEIGHT},
#line 56 "tokens.gperf"
    {"EndTrigger",XML_ENDTRIGGER},
    {(char*)0, 0}, {(char*)0, 0},
#line 188 "tokens.gperf"
    {"SpAfter",XML_SPAFTER},
    {(char*)0, 0},
#line 95 "tokens.gperf"
    {"Highlight",XML_HIGHLIGHT},
    {(char*)0, 0},
#line 29 "tokens.gperf"
    {"CANS",XML_CANS},
    {(char*)0, 0},
#line 117 "tokens.gperf"
    {"LineCap",XML_LINECAP},
    {(char*)0, 0}, {(char*)0, 0},
#line 218 "tokens.gperf"
    {"TxtLocPinY",XML_TXTLOCPINY},
    {(char*)0, 0},
#line 214 "tokens.gperf"
    {"TopMargin",XML_TOPMARGIN},
    {(char*)0, 0}, {(char*)0, 0},
#line 180 "tokens.gperf"
    {"ShdwBkgnd",XML_SHDWBKGND},
#line 193 "tokens.gperf"
    {"Strikethru",XML_STRIKETHRU},
    {(char*)0, 0},
#line 166 "tokens.gperf"
    {"RelLineTo",XML_RELLINETO},
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
#line 304 "tokens.gperf"
    {"vt:fmtConnectorSchemeEx",XML_VT_FMTCONNECTORSCHEMEEX},
#line 186 "tokens.gperf"
    {"SolutionXML",XML_SOLUTIONXML},
    {(char*)0, 0},
#line 273 "tokens.gperf"
    {"a:prstDash",XML_A_PRSTDASH},
#line 91 "tokens.gperf"
    {"HANT",XML_HANT},
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
#line 52 "tokens.gperf"
    {"Ellipse",XML_ELLIPSE},
#line 90 "tokens.gperf"
    {"HANS",XML_HANS},
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
#line 225 "tokens.gperf"
    {"VariationStyleIndex",XML_VARIATIONSTYLEINDEX},
#line 227 "tokens.gperf"
    {"Visible",XML_VISIBLE},
    {(char*)0, 0},
#line 97 "tokens.gperf"
    {"Hyperlink",XML_HYPERLINK},
    {(char*)0, 0},
#line 181 "tokens.gperf"
    {"ShdwForegnd",XML_SHDWFOREGND},
    {(char*)0, 0},
#line 41 "tokens.gperf"
    {"D",XML_D},
#line 16 "tokens.gperf"
    {"ArcTo",XML_ARCTO},
#line 96 "tokens.gperf"
    {"HorzAlign",XML_HORZALIGN},
    {(char*)0, 0},
#line 104 "tokens.gperf"
    {"IndRight",XML_INDRIGHT},
    {(char*)0, 0},
#line 40 "tokens.gperf"
    {"ConnectorSchemeIndex",XML_CONNECTORSCHEMEINDEX},
#line 184 "tokens.gperf"
    {"ShdwPattern",XML_SHDWPATTERN},
    {(char*)0, 0}, {(char*)0, 0},
#line 230 "tokens.gperf"
    {"XForm",XML_XFORM},
#line 224 "tokens.gperf"
    {"VariationColorIndex",XML_VARIATIONCOLORINDEX},
    {(char*)0, 0},
#line 310 "tokens.gperf"
    {"vt:fontStylesGroup",XML_VT_FONTSTYLESGROUP},
    {(char*)0, 0},
#line 210 "tokens.gperf"
    {"TextDirection",XML_TEXTDIRECTION},
#line 228 "tokens.gperf"
    {"Width",XML_WIDTH},
#line 212 "tokens.gperf"
    {"TextXForm",XML_TEXTXFORM},
    {(char*)0, 0},
#line 101 "tokens.gperf"
    {"ImgWidth",XML_IMGWIDTH},
    {(char*)0, 0}, {(char*)0, 0},
#line 170 "tokens.gperf"
    {"RightMargin",XML_RIGHTMARGIN},
    {(char*)0, 0}, {(char*)0, 0},
#line 85 "tokens.gperf"
    {"Geometry",XML_GEOMETRY},
    {(char*)0, 0},
#line 159 "tokens.gperf"
    {"QuickStyleLineMatrix",XML_QUICKSTYLELINEMATRIX},
#line 171 "tokens.gperf"
    {"Rounding",XML_ROUNDING},
    {(char*)0, 0},
#line 153 "tokens.gperf"
    {"QuickStyleEffectsMatrix",XML_QUICKSTYLEEFFECTSMATRIX},
#line 57 "tokens.gperf"
    {"EndX",XML_ENDX},
    {(char*)0, 0},
#line 155 "tokens.gperf"
    {"QuickStyleFillMatrix",XML_QUICKSTYLEFILLMATRIX},
    {(char*)0, 0}, {(char*)0, 0},
#line 157 "tokens.gperf"
    {"QuickStyleFontMatrix",XML_QUICKSTYLEFONTMATRIX},
#line 74 "tokens.gperf"
    {"FlipY",XML_FLIPY},
    {(char*)0, 0},
#line 39 "tokens.gperf"
    {"Company",XML_COMPANY},
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
#line 198 "tokens.gperf"
    {"TAML",XML_TAML},
    {(char*)0, 0},
#line 44 "tokens.gperf"
    {"Default",XML_DEFAULT},
#line 211 "tokens.gperf"
    {"TextPosAfterBullet",XML_TEXTPOSAFTERBULLET},
    {(char*)0, 0},
#line 130 "tokens.gperf"
    {"NURBSTo",XML_NURBSTO},
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
    {(char*)0, 0},
#line 11 "tokens.gperf"
    {"A",XML_A},
#line 174 "tokens.gperf"
    {"SYRC",XML_SYRC},
    {(char*)0, 0},
#line 306 "tokens.gperf"
    {"vt:fmtSchemeEx",XML_VT_FMTSCHEMEEX},
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
#line 12 "tokens.gperf"
    {"ARAB",XML_ARAB},
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
#line 167 "tokens.gperf"
    {"RelMoveTo",XML_RELMOVETO},
    {(char*)0, 0},
#line 173 "tokens.gperf"
    {"SINH",XML_SINH},
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
#line 51 "tokens.gperf"
    {"EffectSchemeIndex",XML_EFFECTSCHEMEINDEX},
#line 84 "tokens.gperf"
    {"Geom",XML_GEOM},
    {(char*)0, 0},
#line 197 "tokens.gperf"
    {"SubAddress",XML_SUBADDRESS},
    {(char*)0, 0}, {(char*)0, 0},
#line 49 "tokens.gperf"
    {"E",XML_E},
#line 53 "tokens.gperf"
    {"EllipticalArcTo",XML_ELLIPTICALARCTO},
    {(char*)0, 0},
#line 231 "tokens.gperf"
    {"XForm1D",XML_XFORM1D},
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
#line 99 "tokens.gperf"
    {"ImgOffsetX",XML_IMGOFFSETX},
#line 266 "tokens.gperf"
    {"a:lt1",XML_A_LT1},
#line 48 "tokens.gperf"
    {"DrawingScale",XML_DRAWINGSCALE},
    {(char*)0, 0},
#line 18 "tokens.gperf"
    {"BENG",XML_BENG},
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
    {(char*)0, 0},
#line 222 "tokens.gperf"
    {"UIGH",XML_UIGH},
    {(char*)0, 0}, {(char*)0, 0},
#line 83 "tokens.gperf"
    {"GURU",XML_GURU},
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
#line 92 "tokens.gperf"
    {"HEBR",XML_HEBR},
    {(char*)0, 0}, {(char*)0, 0},
#line 58 "tokens.gperf"
    {"EndY",XML_ENDY},
    {(char*)0, 0}, {(char*)0, 0},
#line 107 "tokens.gperf"
    {"JPAN",XML_JPAN},
    {(char*)0, 0}, {(char*)0, 0},
#line 223 "tokens.gperf"
    {"VIET",XML_VIET},
    {(char*)0, 0}, {(char*)0, 0},
#line 272 "tokens.gperf"
    {"a:outerShdw",XML_A_OUTERSHDW},
    {(char*)0, 0}, {(char*)0, 0},
#line 164 "tokens.gperf"
    {"RelCubBezTo",XML_RELCUBBEZTO},
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
    {(char*)0, 0}, {(char*)0, 0},
#line 100 "tokens.gperf"
    {"ImgOffsetY",XML_IMGOFFSETY},
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
    {(char*)0, 0}, {(char*)0, 0},
#line 108 "tokens.gperf"
    {"KHMR",XML_KHMR},
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
    {(char*)0, 0}, {(char*)0, 0},
#line 168 "tokens.gperf"
    {"RelQuadBezTo",XML_RELQUADBEZTO},
    {(char*)0, 0},
#line 82 "tokens.gperf"
    {"GUJR",XML_GUJR},
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
    {(char*)0, 0}, {(char*)0, 0},
#line 244 "tokens.gperf"
    {"a:dk1",XML_A_DK1},
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
    {(char*)0, 0},
#line 182 "tokens.gperf"
    {"ShdwOffsetX",XML_SHDWOFFSETX},
    {(char*)0, 0}, {(char*)0, 0},
#line 201 "tokens.gperf"
    {"THAI",XML_THAI},
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
    {(char*)0, 0}, {(char*)0, 0},
#line 89 "tokens.gperf"
    {"HANG",XML_HANG},
    {(char*)0, 0}, {(char*)0, 0},
#line 124 "tokens.gperf"
    {"MLYM",XML_MLYM},
    {(char*)0, 0}, {(char*)0, 0},
#line 110 "tokens.gperf"
    {"LAOO",XML_LAOO},
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
#line 109 "tokens.gperf"
    {"KNDA",XML_KNDA},
    {(char*)0, 0}, {(char*)0, 0},
#line 233 "tokens.gperf"
    {"YIII",XML_YIII},
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
#line 183 "tokens.gperf"
    {"ShdwOffsetY",XML_SHDWOFFSETY},
    {(char*)0, 0}, {(char*)0, 0},
#line 125 "tokens.gperf"
    {"MONG",XML_MONG},
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
#line 45 "tokens.gperf"
    {"DefaultTabStop",XML_DEFAULTTABSTOP},
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
    {(char*)0, 0},
#line 134 "tokens.gperf"
    {"NoShow",XML_NOSHOW},
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
#line 54 "tokens.gperf"
    {"EmbellishmentIndex",XML_EMBELLISHMENTINDEX},
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
    {(char*)0, 0},
#line 50 "tokens.gperf"
    {"ETHI",XML_ETHI},
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
#line 200 "tokens.gperf"
    {"THAA",XML_THAA},
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
    {(char*)0, 0},
#line 30 "tokens.gperf"
    {"CHER",XML_CHER},
    {(char*)0, 0}, {(char*)0, 0},
#line 165 "tokens.gperf"
    {"RelEllipticalArcTo",XML_RELELLIPTICALARCTO},
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
    {(char*)0, 0},
#line 81 "tokens.gperf"
    {"GEOR",XML_GEOR},
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
    {(char*)0, 0},
#line 172 "tokens.gperf"
    {"Row",XML_ROW},
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
#line 131 "tokens.gperf"
    {"NewWindow",XML_NEWWINDOW},
    {(char*)0, 0},
#line 135 "tokens.gperf"
    {"ORYA",XML_ORYA},
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
    {(char*)0, 0},
#line 55 "tokens.gperf"
    {"EndArrow",XML_ENDARROW},
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
    {(char*)0, 0}, {(char*)0, 0},
#line 42 "tokens.gperf"
    {"DEVA",XML_DEVA},
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
    {(char*)0, 0},
#line 47 "tokens.gperf"
    {"DoubleStrikethrough",XML_DOUBLESTRIKETHROUGH}
  };

const struct xmltoken *
Perfect_Hash::in_word_set (const char *str, unsigned int len)
{
  if (len <= MAX_WORD_LENGTH && len >= MIN_WORD_LENGTH)
    {
      unsigned int key = hash (str, len);

      if (key <= MAX_HASH_VALUE)
        {
          const char *s = wordlist[key].name;

          if (s && *str == *s && !strncmp (str + 1, s + 1, len - 1) && s[len] == '\0')
            return &wordlist[key];
        }
    }
  return 0;
}
#line 328 "tokens.gperf"

