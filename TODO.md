# TODO: további formátumok a nitroview-ba

Javaslatok 2026-11-15-ről: kép- és vektoros formátumok, amik vagy nyílt specifikációból néhány ezer
sorban megírhatók, vagy van hozzájuk kompatibilis licencű könyvtár. A sorszámok becslések.

Javasolt sorrend: először az „ingyen" csoport, aztán a HPGL és az EMF+, majd a librevenge-család a
Publisherrel és a FreeHanddel.

Kész: betűtípus-fájlok mintalapja (nitrotype, 2026-11-15: TTF, OTF, TTC, WOFF, WOFF2 a Brotli dekóderével, EOT,
Type 1). Kimaradt a tömörített EOT (MicroType Express: a libeot MPL-2.0 licencű, vagy 600–900 sor saját kód; a
mintákból 13-ból 9 ilyen, de „annyira nem fontos"). A színes glyphek közül az SVG-táblásak megjelennek (külön
lapokon a körvonalak után); a rétegezett (COLR) és a bitképes (sbix, CBDT) fajta nem.

Kész: JPEG 2000 képfájlok (JP2, J2K) a nitrojpx-szel (2026-11-16); gzip-be csomagolt rajzok (SVGZ, EMZ, WMZ).
Kész: DjVu (2026-11-16): a maszk (JB2: nitrodjvu), a háttérkép, a fényképek és az előtér színei (IW44, FGbz:
nitroiw44). Kimaradt: a JPEG-kódolású rétegek (BGjp, FGjp: a mintákban nincs), a rejtett szöveg, a tartalomjegyzék,
a külön fájlokból álló (indirect) dokumentumok.

Kész: JPEG XL (2026-11-17, nitrojxl): VarDCT és Modular, alfa, 8–32 bites egész és lebegőpontos minták, átkódolt JPEG-ek
(YCbCr), ICC; a Lightroom-exportok pixelpontosak a libjxl-hez képest (a HDR tónusleképezve). Kimaradt: patch-ek, spline-ok,
felskálázás, keverő rétegek / animációs képkockák (a csempézett, egymás mellé tett képkockák mennek), LF-képkockák (progresszív DC), a zaj kirajzolása; a DICOM-ba
ágyazott JPEG XL. Sebesség (16 szál, a libjxl-hez képest): a nagy veszteséges Lightroom-export 1,0–1,1×, a kisebb
XYB-s VarDCT és az átkódolt JPEG 1,6–1,9×, a veszteségmentes 1,1–1,7× lassabb; az ImageIO-nál mindenhol gyorsabb.
Vektoros IDCT (kifejtett 8/16/32 pontos, 8×8-as blokkos transzponálás) / szűrők / színkonverzió / króma-felskálázás,
csempés renderelés a csoportokkal átfedve, újrahasznosított pufferek, gyors ANS-út, csatornánként metszett, kétszintes
MA-fa, a gradiens-fás LF-kép táblából (libjxl is_gradient_only). Hátravan: a súlyozott prediktor (WP) és a
tulajdonságok számítása a veszteségmentesnél; az LF-csoportok (6 db egy 24 MP-es képen) átfedése a HF-dekódolással.

Kész: DICOM (2026-11-16, nitrodicom): tömörítetlen, RLE, JPEG, veszteségmentes JPEG, JPEG 2000; szürke az ablakkal,
paletta, RGB / YBR; E / Shift+E világosít. Kimaradt: JPEG-LS (külön dekóder, ~700 sor), a színes 12 bites veszteséges JPEG
(a szürkét 2026-11-17 óta a nitrojpeg 12 bites IDCT-je viszi: nj_decode_planes16), HTJ2K, JPEG XL (a nitrojxl már megvan), a deflate-tel tömörített fájl, a szakaszos (segmented) paletta, a modality
lookup table, az overlay-ek. A többképes fájl képei között a [ / ] lép: egy közös beállítás (mint az E-nél az
expozíció), a kép újradekódolódik, gyorsítótár nélkül (NvDecoder.frame, NvImage.frames). Ugyanez az út jó lesz az
animált GIF-hez és APNG-hez is.

## Szinte ingyen (a meglévő olvasókra épül)

| Formátum | Mi kell hozzá |
|---|---|
| **Illustrator `.ai`** (9-es verziótól) | belül PDF: kiterjesztés és fejléc-ellenőrzés, a nitropdf olvassa |
| **MPO** | több JPEG egy fájlban (3D és telefonos képek): lapozás a meglévő JPEG-olvasóval |
| **ICO / CUR, ICNS** | BMP, PNG, illetve JP2 van bennük: 200–300 sor |

## Saját olvasó nyílt specifikációból

| Formátum | Becsült méret | Megjegyzés |
|---|---|---|
| **HPGL / PLT** | ~1000 sor | plotter- és vágófájlok, a DXF és CDR mellé illik; a HP-GL/2 leírása nyilvános |
| **EMF+** | ~2000 sor | a nitrometa most kihagyja; a csak EMF+ rekordot tartalmazó fájlok üresen jelennek meg ([MS-EMFPLUS]) |
| **Régi Illustrator és AI-EPS** (8-asig) | 1000–1500 sor | dokumentált PostScript-részhalmaz, az operátorai szinte a PDF-éi; clipart-gyűjteményekben gyakori |
| **XPS / OXPS** | ~2000 sor | ZIP + XML, SVG-szerű útvonalak, beágyazott betűk (ECMA-388); saját ZIP-olvasó kell hozzá |
| **CBZ**, **ORA / KRA** | 200 sor + ZIP | képek ZIP-ben lapozva, illetve a rétegezett fájl kész egyesített PNG-je |
| **QOI, Netpbm / PFM, Radiance HDR** | 100–300 sor | egyszerűek, de ritkák |
| **Gerber + Excellon** | ~1500 sor | nyáktervek; a Ucamco specifikációja nyílt |
| **Excalidraw** | ~800 sor | JSON, a draw.io közönségének; JSON-olvasó kell |
| **ODG** | 2000–3000 sor | LibreOffice Draw; a szövegtördelés a nehéz része |

## Kész könyvtár kompatibilis licenccel

| Formátum | Könyvtár, licenc | Megjegyzés |
|---|---|---|
| **Publisher, FreeHand, Zoner Draw, WordPerfect Graphics, PageMaker, QuarkXPress** | libmspub, libfreehand, libzmf, libwpg, libpagemaker, libqxp (MPL-2.0, mint a libcdr és a libvisio) | ugyanarra a librevenge rajzoló felületre adnak ki, amit a nitrocdr már fogad: formátumonként néhány tucat sor ragasztókód és a könyvtár fordítása; az ICU-függést valószínűleg ki kell venni, mint a libcdr-nél |
| **OpenEXR** | tinyexr vagy OpenEXRCore (BSD-3) | HDR |
| **JPEG XR** (`.jxr`, `.wdp`) | jxrlib (BSD-2) | az XPS képeihez és a Windows HDR képernyőképeihez kell |
| **DDS / KTX** | bcdec (MIT) | játékfejlesztői textúrák |

## Fényképezőgép-nyers formátumok

Fotónézőként ezek érnék a legtöbbet, de hivatalos specifikáció nincs, csak nyílt forrású dekóderekből
ismert leírás.

| Formátum | Becsült méret | Megjegyzés |
|---|---|---|
| **ORF, RW2, PEF** | egyenként néhány száz sor | TIFF-alapúak, csak a tömörítésük saját |
| **CR3** | 2000–3000 sor | a Canon mai formátuma: ISO BMFF + saját kodek |
| (mind) | LibRaw (LGPL / CDDL kettős licenc) | mindet tudja, de nem illik a „saját, több magon futó dekóder" elvhez |
