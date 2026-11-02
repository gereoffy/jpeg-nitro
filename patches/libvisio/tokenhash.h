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

#define TOTAL_KEYWORDS 310
#define MIN_WORD_LENGTH 1
#define MAX_WORD_LENGTH 31
#define MIN_HASH_VALUE 4
#define MAX_HASH_VALUE 708
/* maximum key range = 705, duplicates = 0 */

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
      709, 709, 709, 709, 709, 709, 709, 709, 709, 709,
      709, 709, 709, 709, 709, 709, 709, 709, 709, 709,
      709, 709, 709, 709, 709, 709, 709, 709, 709, 709,
      709, 709, 709, 709, 709, 709, 709, 709, 709, 709,
      709, 709, 709, 709, 709, 709, 709, 709, 709, 165,
       77, 127, 102,  47,  40,   1, 709, 709,   0, 709,
      709, 709, 709, 709, 709, 183, 101,  85, 180,  82,
       19,  95, 196, 127,   0,  30,  80, 194, 161,  90,
       10, 125, 116,  32,  18,  22, 158,  83,  90,  95,
      709, 709, 709, 709, 709, 709, 709,   0, 127,  43,
        3,   0,  25,  22, 168,   0, 108,  11,   2,  76,
        0,  33,  95, 709,   8,  20,   1,  35,   0, 178,
       96,  37,   4, 709, 709, 709, 709, 709, 709, 709,
      709, 709, 709, 709, 709, 709, 709, 709, 709, 709,
      709, 709, 709, 709, 709, 709, 709, 709, 709, 709,
      709, 709, 709, 709, 709, 709, 709, 709, 709, 709,
      709, 709, 709, 709, 709, 709, 709, 709, 709, 709,
      709, 709, 709, 709, 709, 709, 709, 709, 709, 709,
      709, 709, 709, 709, 709, 709, 709, 709, 709, 709,
      709, 709, 709, 709, 709, 709, 709, 709, 709, 709,
      709, 709, 709, 709, 709, 709, 709, 709, 709, 709,
      709, 709, 709, 709, 709, 709, 709, 709, 709, 709,
      709, 709, 709, 709, 709, 709, 709, 709, 709, 709,
      709, 709, 709, 709, 709, 709, 709, 709, 709, 709,
      709, 709, 709, 709, 709, 709, 709, 709, 709, 709,
      709, 709, 709, 709, 709, 709
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
#line 239 "tokens.gperf"
    {"a:ea",XML_A_EA},
    {(char*)0, 0},
#line 257 "tokens.gperf"
    {"a:ln",XML_A_LN},
#line 256 "tokens.gperf"
    {"a:lin",XML_A_LIN},
#line 275 "tokens.gperf"
    {"a:tint",XML_A_TINT},
    {(char*)0, 0},
#line 255 "tokens.gperf"
    {"a:latin",XML_A_LATIN},
#line 316 "tokens.gperf"
    {"vt:varStyle",XML_VT_VARSTYLE},
#line 286 "tokens.gperf"
    {"dc:title",XML_DC_TITLE},
#line 315 "tokens.gperf"
    {"vt:varColor7",XML_VT_VARCOLOR7},
#line 305 "tokens.gperf"
    {"vt:lineStyle",XML_VT_LINESTYLE},
#line 285 "tokens.gperf"
    {"dc:template",XML_DC_TEMPLATE},
#line 283 "tokens.gperf"
    {"dc:language",XML_DC_LANGUAGE},
#line 144 "tokens.gperf"
    {"Print",XML_PRINT},
    {(char*)0, 0}, {(char*)0, 0},
#line 282 "tokens.gperf"
    {"dc:description",XML_DC_DESCRIPTION},
    {(char*)0, 0},
#line 137 "tokens.gperf"
    {"Para",XML_PARA},
#line 317 "tokens.gperf"
    {"vt:variationClrScheme",XML_VT_VARIATIONCLRSCHEME},
#line 319 "tokens.gperf"
    {"vt:variationStyleScheme",XML_VT_VARIATIONSTYLESCHEME},
#line 71 "tokens.gperf"
    {"Font",XML_FONT},
    {(char*)0, 0},
#line 318 "tokens.gperf"
    {"vt:variationClrSchemeLst",XML_VT_VARIATIONCLRSCHEMELST},
#line 320 "tokens.gperf"
    {"vt:variationStyleSchemeLst",XML_VT_VARIATIONSTYLESCHEMELST},
#line 62 "tokens.gperf"
    {"Fill",XML_FILL},
#line 287 "tokens.gperf"
    {"dcterms:created",XML_DCTERMS_CREATED},
#line 288 "tokens.gperf"
    {"dcterms:modified",XML_DCTERMS_MODIFIED},
#line 267 "tokens.gperf"
    {"a:satMod",XML_A_SATMOD},
    {(char*)0, 0},
#line 74 "tokens.gperf"
    {"Foreign",XML_FOREIGN},
#line 306 "tokens.gperf"
    {"vt:lineStyles",XML_VT_LINESTYLES},
#line 130 "tokens.gperf"
    {"Page",XML_PAGE},
    {(char*)0, 0},
#line 75 "tokens.gperf"
    {"ForeignData",XML_FOREIGNDATA},
#line 143 "tokens.gperf"
    {"Position",XML_POSITION},
#line 178 "tokens.gperf"
    {"Size",XML_SIZE},
    {(char*)0, 0},
#line 251 "tokens.gperf"
    {"a:gradFill",XML_A_GRADFILL},
#line 246 "tokens.gperf"
    {"a:fillStyleLst",XML_A_FILLSTYLELST},
#line 67 "tokens.gperf"
    {"FillPattern",XML_FILLPATTERN},
#line 184 "tokens.gperf"
    {"SplineKnot",XML_SPLINEKNOT},
#line 185 "tokens.gperf"
    {"SplineStart",XML_SPLINESTART},
#line 258 "tokens.gperf"
    {"a:lnStyleLst",XML_A_LNSTYLELST},
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
#line 314 "tokens.gperf"
    {"vt:varColor6",XML_VT_VARCOLOR6},
#line 142 "tokens.gperf"
    {"Pos",XML_POS},
    {(char*)0, 0}, {(char*)0, 0},
#line 65 "tokens.gperf"
    {"FillForegnd",XML_FILLFOREGND},
#line 294 "tokens.gperf"
    {"vt:fillProps",XML_VT_FILLPROPS},
#line 295 "tokens.gperf"
    {"vt:fillStyles",XML_VT_FILLSTYLES},
#line 313 "tokens.gperf"
    {"vt:varColor5",XML_VT_VARCOLOR5},
    {(char*)0, 0},
#line 72 "tokens.gperf"
    {"FontScale",XML_FONTSCALE},
#line 240 "tokens.gperf"
    {"a:effectLst",XML_A_EFFECTLST},
#line 241 "tokens.gperf"
    {"a:effectStyle",XML_A_EFFECTSTYLE},
#line 235 "tokens.gperf"
    {"a:clrScheme",XML_A_CLRSCHEME},
#line 249 "tokens.gperf"
    {"a:font",XML_A_FONT},
#line 252 "tokens.gperf"
    {"a:gs",XML_A_GS},
#line 271 "tokens.gperf"
    {"a:srgbClr",XML_A_SRGBCLR},
#line 270 "tokens.gperf"
    {"a:solidFill",XML_A_SOLIDFILL},
#line 284 "tokens.gperf"
    {"dc:subject",XML_DC_SUBJECT},
#line 250 "tokens.gperf"
    {"a:fontScheme",XML_A_FONTSCHEME},
#line 132 "tokens.gperf"
    {"PageProps",XML_PAGEPROPS},
#line 281 "tokens.gperf"
    {"dc:creator",XML_DC_CREATOR},
#line 133 "tokens.gperf"
    {"PageScale",XML_PAGESCALE},
#line 134 "tokens.gperf"
    {"PageSheet",XML_PAGESHEET},
    {(char*)0, 0},
#line 187 "tokens.gperf"
    {"Style",XML_STYLE},
#line 136 "tokens.gperf"
    {"Pages",XML_PAGES},
#line 66 "tokens.gperf"
    {"FillForegndTrans",XML_FILLFOREGNDTRANS},
    {(char*)0, 0}, {(char*)0, 0},
#line 248 "tokens.gperf"
    {"a:folHlink",XML_A_FOLHLINK},
#line 188 "tokens.gperf"
    {"StyleSheet",XML_STYLESHEET},
#line 168 "tokens.gperf"
    {"Section",XML_SECTION},
#line 109 "tokens.gperf"
    {"Line",XML_LINE},
#line 279 "tokens.gperf"
    {"cp:keywords",XML_CP_KEYWORDS},
#line 68 "tokens.gperf"
    {"Flags",XML_FLAGS},
#line 236 "tokens.gperf"
    {"a:cs",XML_A_CS},
#line 263 "tokens.gperf"
    {"a:minorFont",XML_A_MINORFONT},
#line 310 "tokens.gperf"
    {"vt:varColor2",XML_VT_VARCOLOR2},
#line 301 "tokens.gperf"
    {"vt:fontProps",XML_VT_FONTPROPS},
#line 302 "tokens.gperf"
    {"vt:fontStyles",XML_VT_FONTSTYLES},
#line 292 "tokens.gperf"
    {"vt:color",XML_VT_COLOR},
#line 272 "tokens.gperf"
    {"a:sysClr",XML_A_SYSCLR},
#line 141 "tokens.gperf"
    {"PolylineTo",XML_POLYLINETO},
#line 32 "tokens.gperf"
    {"Cell",XML_CELL},
#line 186 "tokens.gperf"
    {"Strikethru",XML_STRIKETHRU},
#line 107 "tokens.gperf"
    {"Leader",XML_LEADER},
#line 290 "tokens.gperf"
    {"tp",XML_TP},
#line 280 "tokens.gperf"
    {"cp:lastModifiedBy",XML_CP_LASTMODIFIEDBY},
#line 76 "tokens.gperf"
    {"Frame",XML_FRAME},
#line 112 "tokens.gperf"
    {"LinePattern",XML_LINEPATTERN},
#line 189 "tokens.gperf"
    {"StyleSheets",XML_STYLESHEETS},
#line 243 "tokens.gperf"
    {"a:ext",XML_A_EXT},
#line 233 "tokens.gperf"
    {"a:alpha",XML_A_ALPHA},
#line 33 "tokens.gperf"
    {"Char",XML_CHAR},
#line 244 "tokens.gperf"
    {"a:extLst",XML_A_EXTLST},
#line 304 "tokens.gperf"
    {"vt:lineEx",XML_VT_LINEEX},
#line 129 "tokens.gperf"
    {"Overline",XML_OVERLINE},
#line 31 "tokens.gperf"
    {"Case",XML_CASE},
#line 34 "tokens.gperf"
    {"Character",XML_CHARACTER},
    {(char*)0, 0},
#line 24 "tokens.gperf"
    {"Bullet",XML_BULLET},
#line 247 "tokens.gperf"
    {"a:fmtScheme",XML_A_FMTSCHEME},
#line 312 "tokens.gperf"
    {"vt:varColor4",XML_VT_VARCOLOR4},
#line 180 "tokens.gperf"
    {"SortKey",XML_SORTKEY},
#line 25 "tokens.gperf"
    {"BulletFont",XML_BULLETFONT},
#line 245 "tokens.gperf"
    {"a:extraClrSchemeLst",XML_A_EXTRACLRSCHEMELST},
#line 183 "tokens.gperf"
    {"SpLine",XML_SPLINE},
#line 26 "tokens.gperf"
    {"BulletFontSize",XML_BULLETFONTSIZE},
#line 199 "tokens.gperf"
    {"Text",XML_TEXT},
    {(char*)0, 0},
#line 27 "tokens.gperf"
    {"BulletStr",XML_BULLETSTR},
#line 156 "tokens.gperf"
    {"Rel",XML_REL},
#line 261 "tokens.gperf"
    {"a:lumMod",XML_A_LUMMOD},
    {(char*)0, 0},
#line 212 "tokens.gperf"
    {"TxtPinX",XML_TXTPINX},
    {(char*)0, 0}, {(char*)0, 0},
#line 56 "tokens.gperf"
    {"EndTrigger",XML_ENDTRIGGER},
#line 253 "tokens.gperf"
    {"a:gsLst",XML_A_GSLST},
#line 213 "tokens.gperf"
    {"TxtPinY",XML_TXTPINY},
#line 169 "tokens.gperf"
    {"Shape",XML_SHAPE},
#line 59 "tokens.gperf"
    {"ExtraInfo",XML_EXTRAINFO},
#line 277 "tokens.gperf"
    {"cp:category",XML_CP_CATEGORY},
#line 232 "tokens.gperf"
    {"a:accent6",XML_A_ACCENT6},
#line 63 "tokens.gperf"
    {"FillBkgnd",XML_FILLBKGND},
#line 113 "tokens.gperf"
    {"LineTo",XML_LINETO},
#line 104 "tokens.gperf"
    {"Layer",XML_LAYER},
#line 311 "tokens.gperf"
    {"vt:varColor3",XML_VT_VARCOLOR3},
#line 276 "tokens.gperf"
    {"cp",XML_CP},
#line 35 "tokens.gperf"
    {"Color",XML_COLOR},
#line 231 "tokens.gperf"
    {"a:accent5",XML_A_ACCENT5},
#line 300 "tokens.gperf"
    {"vt:fmtSchemeLineStyles",XML_VT_FMTSCHEMELINESTYLES},
#line 106 "tokens.gperf"
    {"LayerMember",XML_LAYERMEMBER},
    {(char*)0, 0},
#line 192 "tokens.gperf"
    {"TELU",XML_TELU},
#line 242 "tokens.gperf"
    {"a:effectStyleLst",XML_A_EFFECTSTYLELST},
#line 23 "tokens.gperf"
    {"BottomMargin",XML_BOTTOMMARGIN},
#line 291 "tokens.gperf"
    {"vt:bkgnd",XML_VT_BKGND},
#line 162 "tokens.gperf"
    {"ResizeMode",XML_RESIZEMODE},
#line 293 "tokens.gperf"
    {"vt:connectorFontStyles",XML_VT_CONNECTORFONTSTYLES},
    {(char*)0, 0},
#line 172 "tokens.gperf"
    {"Shapes",XML_SHAPES},
#line 38 "tokens.gperf"
    {"Colors",XML_COLORS},
#line 296 "tokens.gperf"
    {"vt:fmtConnectorScheme",XML_VT_FMTCONNECTORSCHEME},
#line 99 "tokens.gperf"
    {"Invisible",XML_INVISIBLE},
#line 278 "tokens.gperf"
    {"cp:coreProperties",XML_CP_COREPROPERTIES},
#line 95 "tokens.gperf"
    {"IndFirst",XML_INDFIRST},
#line 195 "tokens.gperf"
    {"TIBT",XML_TIBT},
    {(char*)0, 0},
#line 179 "tokens.gperf"
    {"SolutionXML",XML_SOLUTIONXML},
#line 260 "tokens.gperf"
    {"a:lt2",XML_A_LT2},
#line 73 "tokens.gperf"
    {"FontSchemeIndex",XML_FONTSCHEMEINDEX},
#line 98 "tokens.gperf"
    {"InfiniteLine",XML_INFINITELINE},
#line 49 "tokens.gperf"
    {"E",XML_E},
#line 182 "tokens.gperf"
    {"SpBefore",XML_SPBEFORE},
#line 19 "tokens.gperf"
    {"BegTrigger",XML_BEGTRIGGER},
#line 145 "tokens.gperf"
    {"Properties",XML_PROPERTIES},
    {(char*)0, 0}, {(char*)0, 0},
#line 28 "tokens.gperf"
    {"C",XML_C},
#line 228 "tokens.gperf"
    {"a:accent2",XML_A_ACCENT2},
#line 238 "tokens.gperf"
    {"a:dk2",XML_A_DK2},
#line 114 "tokens.gperf"
    {"LineWeight",XML_LINEWEIGHT},
#line 36 "tokens.gperf"
    {"ColorEntry",XML_COLORENTRY},
#line 273 "tokens.gperf"
    {"a:theme",XML_A_THEME},
#line 309 "tokens.gperf"
    {"vt:varColor1",XML_VT_VARCOLOR1},
#line 64 "tokens.gperf"
    {"FillBkgndTrans",XML_FILLBKGNDTRANS},
#line 204 "tokens.gperf"
    {"TextPosAfterBullet",XML_TEXTPOSAFTERBULLET},
#line 219 "tokens.gperf"
    {"VerticalAlign",XML_VERTICALALIGN},
#line 222 "tokens.gperf"
    {"X",XML_X},
#line 111 "tokens.gperf"
    {"LineColor",XML_LINECOLOR},
#line 308 "tokens.gperf"
    {"vt:themeScheme",XML_VT_THEMESCHEME},
#line 164 "tokens.gperf"
    {"Rounding",XML_ROUNDING},
#line 298 "tokens.gperf"
    {"vt:fmtConnectorSchemeLineStyles",XML_VT_FMTCONNECTORSCHEMELINESTYLES},
#line 52 "tokens.gperf"
    {"Ellipse",XML_ELLIPSE},
    {(char*)0, 0},
#line 254 "tokens.gperf"
    {"a:hlink",XML_A_HLINK},
#line 197 "tokens.gperf"
    {"Tabs",XML_TABS},
#line 125 "tokens.gperf"
    {"NoFill",XML_NOFILL},
#line 225 "tokens.gperf"
    {"Y",XML_Y},
#line 289 "tokens.gperf"
    {"pp",XML_PP},
#line 154 "tokens.gperf"
    {"QuickStyleType",XML_QUICKSTYLETYPE},
#line 139 "tokens.gperf"
    {"PinX",XML_PINX},
#line 269 "tokens.gperf"
    {"a:shade",XML_A_SHADE},
#line 262 "tokens.gperf"
    {"a:majorFont",XML_A_MAJORFONT},
#line 230 "tokens.gperf"
    {"a:accent4",XML_A_ACCENT4},
#line 155 "tokens.gperf"
    {"QuickStyleVariation",XML_QUICKSTYLEVARIATION},
#line 198 "tokens.gperf"
    {"Template",XML_TEMPLATE},
#line 206 "tokens.gperf"
    {"ThemeIndex",XML_THEMEINDEX},
    {(char*)0, 0}, {(char*)0, 0},
#line 17 "tokens.gperf"
    {"B",XML_B},
#line 140 "tokens.gperf"
    {"PinY",XML_PINY},
#line 274 "tokens.gperf"
    {"a:themeElements",XML_A_THEMEELEMENTS},
#line 151 "tokens.gperf"
    {"QuickStyleLineColor",XML_QUICKSTYLELINECOLOR},
#line 149 "tokens.gperf"
    {"QuickStyleFontColor",XML_QUICKSTYLEFONTCOLOR},
#line 147 "tokens.gperf"
    {"QuickStyleFillColor",XML_QUICKSTYLEFILLCOLOR},
#line 105 "tokens.gperf"
    {"LayerMem",XML_LAYERMEM},
#line 208 "tokens.gperf"
    {"TxtAngle",XML_TXTANGLE},
#line 153 "tokens.gperf"
    {"QuickStyleShadowColor",XML_QUICKSTYLESHADOWCOLOR},
#line 15 "tokens.gperf"
    {"Angle",XML_ANGLE},
    {(char*)0, 0}, {(char*)0, 0},
#line 14 "tokens.gperf"
    {"Alignment",XML_ALIGNMENT},
#line 43 "tokens.gperf"
    {"DblUnderline",XML_DBLUNDERLINE},
#line 138 "tokens.gperf"
    {"Paragraph",XML_PARAGRAPH},
#line 96 "tokens.gperf"
    {"IndLeft",XML_INDLEFT},
#line 21 "tokens.gperf"
    {"BeginX",XML_BEGINX},
    {(char*)0, 0},
#line 13 "tokens.gperf"
    {"Address",XML_ADDRESS},
#line 229 "tokens.gperf"
    {"a:accent3",XML_A_ACCENT3},
    {(char*)0, 0},
#line 22 "tokens.gperf"
    {"BeginY",XML_BEGINY},
#line 209 "tokens.gperf"
    {"TxtHeight",XML_TXTHEIGHT},
#line 87 "tokens.gperf"
    {"HideText",XML_HIDETEXT},
    {(char*)0, 0},
#line 200 "tokens.gperf"
    {"TextBkgnd",XML_TEXTBKGND},
#line 119 "tokens.gperf"
    {"Master",XML_MASTER},
#line 115 "tokens.gperf"
    {"LocPinX",XML_LOCPINX},
#line 60 "tokens.gperf"
    {"FaceName",XML_FACENAME},
#line 210 "tokens.gperf"
    {"TxtLocPinX",XML_TXTLOCPINX},
#line 170 "tokens.gperf"
    {"ShapeShdwOffsetX",XML_SHAPESHDWOFFSETX},
#line 177 "tokens.gperf"
    {"ShdwPattern",XML_SHDWPATTERN},
#line 116 "tokens.gperf"
    {"LocPinY",XML_LOCPINY},
#line 202 "tokens.gperf"
    {"TextBlock",XML_TEXTBLOCK},
#line 211 "tokens.gperf"
    {"TxtLocPinY",XML_TXTLOCPINY},
#line 171 "tokens.gperf"
    {"ShapeShdwOffsetY",XML_SHAPESHDWOFFSETY},
#line 131 "tokens.gperf"
    {"PageHeight",XML_PAGEHEIGHT},
#line 159 "tokens.gperf"
    {"RelLineTo",XML_RELLINETO},
    {(char*)0, 0},
#line 120 "tokens.gperf"
    {"Masters",XML_MASTERS},
#line 37 "tokens.gperf"
    {"ColorSchemeIndex",XML_COLORSCHEMEINDEX},
    {(char*)0, 0},
#line 53 "tokens.gperf"
    {"EllipticalArcTo",XML_ELLIPTICALARCTO},
#line 174 "tokens.gperf"
    {"ShdwForegnd",XML_SHDWFOREGND},
#line 126 "tokens.gperf"
    {"NoLine",XML_NOLINE},
#line 44 "tokens.gperf"
    {"Default",XML_DEFAULT},
#line 81 "tokens.gperf"
    {"Geometry",XML_GEOMETRY},
#line 268 "tokens.gperf"
    {"a:schemeClr",XML_A_SCHEMECLR},
#line 122 "tokens.gperf"
    {"MoveTo",XML_MOVETO},
#line 61 "tokens.gperf"
    {"FaceNames",XML_FACENAMES},
#line 297 "tokens.gperf"
    {"vt:fmtConnectorSchemeEx",XML_VT_FMTCONNECTORSCHEMEEX},
#line 307 "tokens.gperf"
    {"vt:schemeID",XML_VT_SCHEMEID},
#line 97 "tokens.gperf"
    {"IndRight",XML_INDRIGHT},
#line 181 "tokens.gperf"
    {"SpAfter",XML_SPAFTER},
    {(char*)0, 0},
#line 265 "tokens.gperf"
    {"a:outerShdw",XML_A_OUTERSHDW},
#line 79 "tokens.gperf"
    {"GURU",XML_GURU},
#line 227 "tokens.gperf"
    {"a:accent1",XML_A_ACCENT1},
    {(char*)0, 0},
#line 46 "tokens.gperf"
    {"Description",XML_DESCRIPTION},
#line 51 "tokens.gperf"
    {"EffectSchemeIndex",XML_EFFECTSCHEMEINDEX},
    {(char*)0, 0},
#line 234 "tokens.gperf"
    {"a:bgFillStyleLst",XML_A_BGFILLSTYLELST},
#line 303 "tokens.gperf"
    {"vt:fontStylesGroup",XML_VT_FONTSTYLESGROUP},
#line 110 "tokens.gperf"
    {"LineCap",XML_LINECAP},
    {(char*)0, 0},
#line 57 "tokens.gperf"
    {"EndX",XML_ENDX},
#line 201 "tokens.gperf"
    {"TextBkgndTrans",XML_TEXTBKGNDTRANS},
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
#line 196 "tokens.gperf"
    {"Tab",XML_TAB},
    {(char*)0, 0},
#line 40 "tokens.gperf"
    {"ConnectorSchemeIndex",XML_CONNECTORSCHEMEINDEX},
#line 214 "tokens.gperf"
    {"TxtWidth",XML_TXTWIDTH},
#line 58 "tokens.gperf"
    {"EndY",XML_ENDY},
#line 216 "tokens.gperf"
    {"VIET",XML_VIET},
#line 218 "tokens.gperf"
    {"VariationStyleIndex",XML_VARIATIONSTYLEINDEX},
#line 157 "tokens.gperf"
    {"RelCubBezTo",XML_RELCUBBEZTO},
    {(char*)0, 0},
#line 80 "tokens.gperf"
    {"Geom",XML_GEOM},
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
#line 223 "tokens.gperf"
    {"XForm",XML_XFORM},
#line 217 "tokens.gperf"
    {"VariationColorIndex",XML_VARIATIONCOLORINDEX},
#line 205 "tokens.gperf"
    {"TextXForm",XML_TEXTXFORM},
    {(char*)0, 0},
#line 135 "tokens.gperf"
    {"PageWidth",XML_PAGEWIDTH},
    {(char*)0, 0}, {(char*)0, 0},
#line 152 "tokens.gperf"
    {"QuickStyleLineMatrix",XML_QUICKSTYLELINEMATRIX},
#line 150 "tokens.gperf"
    {"QuickStyleFontMatrix",XML_QUICKSTYLEFONTMATRIX},
#line 148 "tokens.gperf"
    {"QuickStyleFillMatrix",XML_QUICKSTYLEFILLMATRIX},
#line 146 "tokens.gperf"
    {"QuickStyleEffectsMatrix",XML_QUICKSTYLEEFFECTSMATRIX},
#line 69 "tokens.gperf"
    {"FlipX",XML_FLIPX},
#line 39 "tokens.gperf"
    {"Company",XML_COMPANY},
#line 266 "tokens.gperf"
    {"a:prstDash",XML_A_PRSTDASH},
    {(char*)0, 0}, {(char*)0, 0},
#line 121 "tokens.gperf"
    {"Misc",XML_MISC},
    {(char*)0, 0},
#line 264 "tokens.gperf"
    {"a:objectDefaults",XML_A_OBJECTDEFAULTS},
#line 299 "tokens.gperf"
    {"vt:fmtSchemeEx",XML_VT_FMTSCHEMEEX},
#line 203 "tokens.gperf"
    {"TextDirection",XML_TEXTDIRECTION},
#line 70 "tokens.gperf"
    {"FlipY",XML_FLIPY},
#line 108 "tokens.gperf"
    {"LeftMargin",XML_LEFTMARGIN},
#line 20 "tokens.gperf"
    {"BeginArrow",XML_BEGINARROW},
#line 220 "tokens.gperf"
    {"Visible",XML_VISIBLE},
    {(char*)0, 0},
#line 29 "tokens.gperf"
    {"CANS",XML_CANS},
#line 16 "tokens.gperf"
    {"ArcTo",XML_ARCTO},
#line 207 "tokens.gperf"
    {"TopMargin",XML_TOPMARGIN},
    {(char*)0, 0},
#line 163 "tokens.gperf"
    {"RightMargin",XML_RIGHTMARGIN},
#line 90 "tokens.gperf"
    {"Hyperlink",XML_HYPERLINK},
    {(char*)0, 0}, {(char*)0, 0},
#line 167 "tokens.gperf"
    {"SYRC",XML_SYRC},
#line 161 "tokens.gperf"
    {"RelQuadBezTo",XML_RELQUADBEZTO},
    {(char*)0, 0}, {(char*)0, 0},
#line 173 "tokens.gperf"
    {"ShdwBkgnd",XML_SHDWBKGND},
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
#line 78 "tokens.gperf"
    {"GUJR",XML_GUJR},
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
    {(char*)0, 0}, {(char*)0, 0},
#line 259 "tokens.gperf"
    {"a:lt1",XML_A_LT1},
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
    {(char*)0, 0}, {(char*)0, 0},
#line 237 "tokens.gperf"
    {"a:dk1",XML_A_DK1},
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
#line 103 "tokens.gperf"
    {"LAOO",XML_LAOO},
#line 91 "tokens.gperf"
    {"ImgHeight",XML_IMGHEIGHT},
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
    {(char*)0, 0},
#line 41 "tokens.gperf"
    {"D",XML_D},
    {(char*)0, 0}, {(char*)0, 0},
#line 92 "tokens.gperf"
    {"ImgOffsetX",XML_IMGOFFSETX},
    {(char*)0, 0}, {(char*)0, 0},
#line 11 "tokens.gperf"
    {"A",XML_A},
    {(char*)0, 0},
#line 93 "tokens.gperf"
    {"ImgOffsetY",XML_IMGOFFSETY},
#line 48 "tokens.gperf"
    {"DrawingScale",XML_DRAWINGSCALE},
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
#line 190 "tokens.gperf"
    {"SubAddress",XML_SUBADDRESS},
#line 191 "tokens.gperf"
    {"TAML",XML_TAML},
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
    {(char*)0, 0}, {(char*)0, 0},
#line 160 "tokens.gperf"
    {"RelMoveTo",XML_RELMOVETO},
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
    {(char*)0, 0},
#line 86 "tokens.gperf"
    {"Height",XML_HEIGHT},
#line 224 "tokens.gperf"
    {"XForm1D",XML_XFORM1D},
    {(char*)0, 0}, {(char*)0, 0},
#line 84 "tokens.gperf"
    {"HANT",XML_HANT},
#line 88 "tokens.gperf"
    {"Highlight",XML_HIGHLIGHT},
    {(char*)0, 0},
#line 89 "tokens.gperf"
    {"HorzAlign",XML_HORZALIGN},
    {(char*)0, 0}, {(char*)0, 0},
#line 30 "tokens.gperf"
    {"CHER",XML_CHER},
#line 175 "tokens.gperf"
    {"ShdwOffsetX",XML_SHDWOFFSETX},
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
#line 94 "tokens.gperf"
    {"ImgWidth",XML_IMGWIDTH},
#line 176 "tokens.gperf"
    {"ShdwOffsetY",XML_SHDWOFFSETY},
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
#line 77 "tokens.gperf"
    {"GEOR",XML_GEOR},
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
#line 83 "tokens.gperf"
    {"HANS",XML_HANS},
    {(char*)0, 0}, {(char*)0, 0},
#line 221 "tokens.gperf"
    {"Width",XML_WIDTH},
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
#line 158 "tokens.gperf"
    {"RelEllipticalArcTo",XML_RELELLIPTICALARCTO},
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
#line 45 "tokens.gperf"
    {"DefaultTabStop",XML_DEFAULTTABSTOP},
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
    {(char*)0, 0},
#line 123 "tokens.gperf"
    {"NURBSTo",XML_NURBSTO},
    {(char*)0, 0},
#line 54 "tokens.gperf"
    {"EmbellishmentIndex",XML_EMBELLISHMENTINDEX},
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
#line 18 "tokens.gperf"
    {"BENG",XML_BENG},
    {(char*)0, 0}, {(char*)0, 0},
#line 194 "tokens.gperf"
    {"THAI",XML_THAI},
#line 101 "tokens.gperf"
    {"KHMR",XML_KHMR},
    {(char*)0, 0},
#line 55 "tokens.gperf"
    {"EndArrow",XML_ENDARROW},
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
#line 165 "tokens.gperf"
    {"Row",XML_ROW},
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
#line 226 "tokens.gperf"
    {"YIII",XML_YIII},
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
#line 100 "tokens.gperf"
    {"JPAN",XML_JPAN},
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
#line 215 "tokens.gperf"
    {"UIGH",XML_UIGH},
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
#line 85 "tokens.gperf"
    {"HEBR",XML_HEBR},
    {(char*)0, 0}, {(char*)0, 0},
#line 50 "tokens.gperf"
    {"ETHI",XML_ETHI},
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
#line 118 "tokens.gperf"
    {"MONG",XML_MONG},
    {(char*)0, 0},
#line 82 "tokens.gperf"
    {"HANG",XML_HANG},
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
#line 128 "tokens.gperf"
    {"ORYA",XML_ORYA},
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
#line 193 "tokens.gperf"
    {"THAA",XML_THAA},
#line 12 "tokens.gperf"
    {"ARAB",XML_ARAB},
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
    {(char*)0, 0},
#line 127 "tokens.gperf"
    {"NoShow",XML_NOSHOW},
    {(char*)0, 0},
#line 102 "tokens.gperf"
    {"KNDA",XML_KNDA},
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
#line 166 "tokens.gperf"
    {"SINH",XML_SINH},
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
#line 124 "tokens.gperf"
    {"NewWindow",XML_NEWWINDOW},
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
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
#line 117 "tokens.gperf"
    {"MLYM",XML_MLYM},
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
    {(char*)0, 0},
#line 47 "tokens.gperf"
    {"DoubleStrikethrough",XML_DOUBLESTRIKETHROUGH},
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
    {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0}, {(char*)0, 0},
#line 42 "tokens.gperf"
    {"DEVA",XML_DEVA}
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
#line 321 "tokens.gperf"

