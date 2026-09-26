<p align="center"><img src="docs/logo.webp" alt="NitroView – JPEG viewer for macOS" width="800"></p>

# jpeg-nitro

[English](README.md) | **Magyar**

**nitroview**: nagyon gyors képnéző macOS-re, és **nitrojpeg**: párhuzamos baseline
JPEG-dekóder, ami egyetlen képet is az összes CPU-magon dekódol. Egy 24 MP-es JPEG-et
~11 ms alatt dekódol, a libjpeg-turbo egy szálon ~107 ms. Így a 24 MP-es fotósorozatok
(pl. timelapse) teljes felbontásban, valós időben „lejátszhatók”, akár 60 kép/s-mal.
A név utalás: versenyautókban a turbót nitróval gyorsítják tovább.

Intel x86-64 Macen (i9, 13. gen., 8 mag / 16 szál, AMD RX 580) fejlesztve és mérve.
Apple Silicon-on nincs kipróbálva (az AVX2 helyett ott a sima C-s IDCT fordul).

```
./nitroview [-f] [-s ms] [-j szálak] kep1.jpg kep2.jpg ... | konyvtar/
```

| billentyű | funkció |
|---|---|
| PgDn / Space / → / ↓ | következő kép |
| PgUp / Backspace / ← / ↑ | előző kép |
| Home / End | első / utolsó |
| F / Enter | teljes képernyő be/ki (`-f`: indításkor teljes képernyő) |
| P | diavetítés szüneteltetése / folytatása |
| Esc / Q | kilépés (Esc teljes képernyőn: vissza ablakba) |

Az ablakcím mutatja a fájlnevet, a méretet és a dekódolási időt. A stdout-ra
minden képnél kiíródik: olvasás, dekódolás, és a billentyű → képernyő idő.
Az EXIF-orientációt figyelembe veszi.

**Diavetítés:** `-s <ms>` (vagy `--slideshow <ms>`): a következő kép akkor jön, amikor az
aktuális már `ms` ezredmásodperce a képernyőn van (pl. `-s 2000` klasszikus diavetítés,
`-s 40` ~25 kép/s). Ha egy kép betöltése tovább tart, kivárja, így egy kép sem marad ki.
Az utolsó képnél megáll. P szünetelteti, a lapozó gombok közben is működnek, és onnan
folytatja. Az időzítés a monitor frissítéséhez igazodik (60 Hz-en ±8 ms).
Paraméterek nélkül (vagy `-h`) a program kiírja az összes kapcsolót.

A `-j N` a dekóder szálainak számát korlátozza (alapból mind a 16 logikai szál). 8 szálon
a dekódolás ~15.4 ms/kép a 11.8 helyett, de kevésbé melegíti a CPU-t.

Egyéb módok:
- `--bench fájlok…`: ablak nélkül betölti az összeset (olvasás, dekódolás, GPU), és időt mér.
- `--selftest fájlok…`: a GPU-s színkonverziót összeveti a libjpeg-turbo kimenetével.
- `--auto <ms>`: tesztelési mód: adott időközönként lapoz (akkor is, ha a kép még nem jelent
  meg), a végén kilép. Késleltetésméréshez.

## Eredmény (50 minta, zömmel 6000×4000, átlag 17 MB)

**Teljes betöltés a nézőben: ~23 ms/kép** (fájlolvasás 3.5 + dekódolás 12.7 + GPU 7).
Lapozáskor az előtöltött kép **7–20 ms** alatt kerül a képernyőre (1 képkocka 60 Hz-en).

### Dekóderek összehasonlítása (`bench/bench`, csak dekódolás, memóriából)

| dekóder | ms/kép |
|---|---|
| Apple ImageIO (CGImageSource) → BGRA | 274 |
| Apple ImageIO, saját puffer | 249 |
| stb_image | 175 |
| Wuffs | 159 |
| ImageIO thumbnail 3000 px (DCT-skálázás) | 159 |
| VideoToolbox (JPEG; ezen a gépen nincs HW JPEG, szoftveres) | 157 |
| libjpeg-turbo 3.1.2 (AVX2), BGRX | 124 |
| libjpeg-turbo, fast DCT + fast upsampling | 112 |
| libjpeg-turbo → YUV síkok (nincs színkonverzió) | 107 |
| libjpeg-turbo 1/2 skála | 100 |
| libjpeg-turbo 1/4 skála | 96 |
| nitrojpeg, saját motor, **1 szál** → YUV | 103 |
| nitrojpeg, régi motor (libjpeg-turbo dekódolja a sávokat) → YUV | 20.6 |
| **nitrojpeg, saját motor, párhuzamos → YUV síkok** (ezt használja a néző) | **11.7** |

A 1/4-es skálázás alig gyorsít, tehát az idő nagy része a Huffman-dekódolás,
ami a JPEG-ben eredendően soros. A mintaképek többsége nem tartalmaz restart
markert, ezért a libjpeg-turbo egy képen belül nem tud párhuzamosítani.
Az RX 580-hoz nincs hardveres JPEG-dekóder.

### nitrojpeg: hogyan párhuzamosít egyetlen képen belül?

A saját motor (alapértelmezés) a Huffman-folyamot **egyszer** dekódolja, libjpeg-turbo nélkül:

1. **Előkészítés (párhuzamos, ~1 ms):** megkeresi az adat végét, eltávolítja a
   byte-stuffinget (0xFF 0x00) és a restart markereket, így egy „tiszta” bitfolyam lesz.
2. **Marker nélküli fájlok: spekulatív teljes dekódolás.** A bitfolyamot ~512 darabra
   vágja (`32 × CPU`, darabonként ~200 KB együttható, ez befér az L2 cache-be). Minden
   darabot egy szál egy tetszőleges bájtpozíción kezd dekódolni, és az együtthatókat
   rögtön kiírja. A Huffman-kódok néhány MCU után „önszinkronizálnak” a valódi úttal.
3. **Összefűzés, sorrendben, futószalagon.** Ha a valódi út egy olyan bitpozíción kezd
   MCU-t, amit a következő darab is feljegyzett, onnantól azonosak. A darab MCU-sorszáma
   és komponensenkénti DC-eltolása így kiderül, a szinkron előtti pár MCU eldobva.
4. **Dekvantálás + IDCT** saját AVX2 kóddal. Az algoritmus a libjpeg „ISLOW” IDCT-je
   (jidctint.c), ezért bitre azonos kimenetet ad. Egy szál, amint egy általa dekódolt
   darab összefűződött, azonnal IDCT-zi, amíg az együtthatók még a cache-ben vannak.
5. **Restart markeres fájlok:** nincs spekuláció, az intervallumokat párhuzamosan
   dekódolja, és MCU-nként rögtön IDCT-zi.
6. A kimenet Y/Cb/Cr síkok, közvetlenül egy Metal (shared) pufferbe. A felmintázást
   és a YCbCr→RGB konverziót egy GPU compute kernel végzi, a mipmapeket
   a GPU generálja, a megjelenítés trilineáris szűréssel skáláz.

Mért szinkronizáció (`bench/verify`, 512 darab, képenként az első 256 szinkronpont,
40 kép = 10240 eset): átlag 2.40 „szemét” MCU (~922 bit, ~115 bájt) után minden darab
rááll a helyes útra. Az esetek 40%-ában 1 MCU után, a leglassabb 21 MCU (8754 bit) volt,
és egyetlen esetben sem maradt ki a szinkron.

**Pontosság:** a párhuzamos kimenet **bitre azonos** az egyszálú libjpeg-turbo
kimenetével, mind az 50 képen (`bench/verify`, skalár és AVX2 IDCT-vel is). A GPU-s
konverzió legfeljebb ±1 eltérést ad (kerekítés, `--selftest`).
Nem támogatott formátumoknál (progresszív, CMYK, RGB, >16384 px) és sérült fájloknál
a néző automatikusan a sima TurboJPEG-re vált.

### A fejlődés lépései (ms/kép, 16 szál)

| változat | ms/kép |
|---|---|
| libjpeg-turbo, 1 szál | 107 |
| spekulatív átugrás + libjpeg-turbo dekódolja a 128 „kamu” JPEG sávot (Huffman kétszer) | 19.9 |
| ugyanez, de egyszálú Huffman-bejárás a spekuláció helyett (`NJ_SEQ=1`, régi motor) | 67.2 |
| saját motor: egyszeri Huffman, együtthatók a memóriában, utána IDCT | 15.7 |
| + egy-lookupos AC-dekódolás, futószalag: IDCT amíg cache-ben van | **11.7** |

Az egyszálú Huffman-bejárás azért lassú, mert soros függőségi lánc: minden kód hosszát
ismerni kell, mielőtt a következőt olvasni lehetne (~12 órajel/szimbólum). A futószalag
nélkül az IDCT memória-korlátos volt: a 96 MB-os együtthatótömb kiírása és visszaolvasása
~45 GB/s-nál telítődött, 8 szál fölött már nem gyorsult.

Darabszám a saját motorban (`NJ_CHUNKS`), ms/kép:

| darabok | 64 | 128 | 256 | 512 | 1024 | 2048 |
|---|---|---|---|---|---|---|
| ms/kép | 12.3 | 12.0 | 11.9 | **11.4** | 12.1 | 14.7 |

A régi motor (libjpeg-turbo sávok, `nj_set_engine(1)`, `NJ_ENGINE=1`) mérései a
`bench/results/matrix.txt` (darabok × szálak) és `bench/results/bands.txt` (sávszám) fájlokban vannak.
Ott a HT 8 → 16 szálon ~26%-ot hozott, és a sok kis sáv volt a jobb.

### Mérési módszer: órajel és hőmérséklet

A gép (hackintosh, i9-13. gen.) órajele 4 és 5.5 GHz között ugrálhat, 16 szálas terhelésnél
a CPU gyorsan 90 °C fölé melegszik, ezért egy-egy mérés ±10%-ot is tévedhet.

- `bench/freqprobe [szálak] [másodperc]`: root nélkül méri a valódi órajelet (egymástól
  függő összeadások lánca: 1 összeadás/órajel). Mért értékek: egy szálon, pihenő gépről
  4.48 GHz (az ütemező lassan emeli az órajelet), 16 szálon ~4.95 GHz, 8 szálon ~5.3 GHz.
- `bench/sustain SEC SZÁLAK fájlok…`: folyamatos dekódolás, félmásodpercenként sebesség +
  órajel. 30 másodpercig 16 szálon nincs lefelé tartó trend (10.1–11.9 ms/kép, ±10%
  ingadozás), tehát a hűtés tartja; a szórást az órajel-ugrálás okozza, nem fojtás.
- `bench/ab.sh ISMÉTLÉS SZÜNET -- "név:ENV=érték" …`: a változatokat felváltva futtatja,
  minden futás előtt hűlési szünettel és órajelméréssel, mediánt/min/max-ot ad.
  Így a min–max tartomány jellemzően ±3%-on belül marad.

Újramérve ezzel a módszerrel (5 ismétlés, 3 s szünet, ms/kép, `bench/results/ab_results.txt`):

| összehasonlítás | medián (min–max) |
|---|---|
| saját motor / régi motor (libjpeg-turbo sávok) | **11.8** (11.8–11.9) / 19.8 (19.5–20.8) |
| 4 / 8 / 12 / 16 szál | 27.0 / 15.4 / 12.3 / **11.8** |
| 256 / 512 / 1024 darab | 11.9 / **11.7** / 12.2 (átfedő tartományok: a különbség zajon belül) |

A következtetések kitartanak: a saját motor 1.7× gyorsabb a réginél, a HT 8 → 16 szálon
~30%-ot hoz, a darabszám 256–1024 között lényegtelen. (Minden futás külön folyamat, hideg
pufferkészlettel, ezért ~1 ms-mal lassabb a nézőben mért meleg állapotnál.)

### Hibás fájlok

Ha a dekóder hibát lát (érvénytelen Huffman-kód, túlfutó együtthatóindex, az utolsó
MCU a bitfolyam vége után ér véget, eltér a restart markerek száma stb.), nem próbálkozik
tovább. Ilyenkor a néző a sima TurboJPEG-re vált, ami a sérült képből is megmutatja, ami
menthető (pl. egy félig letöltött fájl felső részét). Ha az sem tudja dekódolni (nem JPEG,
sérült fejléc, üres fájl), a címsorban `CANNOT DECODE` jelenik meg, és a lapozás folytatható.

`make bench/robust && bench/robust samples/*`: minden mintából ~90 rontott változatot
készít (csonkítás 0 bájttól a teljes hossz −1-ig, véletlen bájt- és bithibák, nullázott vagy
0xFF-fel kitöltött blokkok, adatba szúrt markerek, fejléchibák, kivágott/duplázott szakaszok),
plusz szemét- és szövegfájlokat. Mindegyiket a néző dekódolási útján viszi végig,
AddressSanitizerrel és UBSan-nal fordítva: 4372 eset, 0 hiba. A futószalag szinkronizációját
ThreadSanitizer is ellenőrizte (ép és sérült fájlokon).

### Megjelenítés, előtöltés

- A háttérszál a lapozás irányában 3, visszafelé 2 képet dekódol előre.
  A szomszéd képeket előre fel is tölti a GPU-ra és konvertálja.
- A Metal puffereket és textúrákat újrahasznosítja. A 128 MB-os textúra minden képnél
  újbóli lefoglalása ~6 ms volt.
- Shared (rendszermemóriás) puffer + a síkokat közvetlenül olvasó kernel volt a
  leggyorsabb (8 ms, szemben a managed pufferes / textúrás 10 ms-mal).
- A dekóder nagy pufferei (tiszta bitfolyam, együtthatók) képről képre újrahasznosulnak.
  Hideg készlettel (az első 50 kép) a dekódolás 11.9 ms/kép, bemelegedve 10.6–10.8 ms/kép.

## Build

Előfeltétel: Xcode Command Line Tools.

```
make                 # néző; ha megvan a libjpeg-turbo, tartalék dekódernek beépíti
make TURBOJPEG=0     # teljesen önálló néző, libjpeg-turbo nélkül (~105 KB)
make tools           # bench/bench, bench/verify, bench/robust (referenciának kell a libjpeg-turbo)
```

**A libjpeg-turbo opcionális.** A dekóder (`src/nitrojpeg.c`) önállóan dekódolja a baseline
JPEG-eket (8 bit, Huffman, 1 vagy 3 komponens, YCbCr/szürke, bármilyen mintavételezés,
restart markerrel vagy anélkül). Ehhez csak C, pthreads, GCD és AVX2 kell.

- **`TURBOJPEG=1`** (alapértelmezés, ha a `third_party/ljt` létezik): a progresszív, CMYK, RGB,
  16384 px-nél nagyobb és a sérült fájlokat a TurboJPEG dekódolja. A sérült képekből is
  megmutatja, ami menthető. A `--selftest` is ebben a módban érhető el.
- **`TURBOJPEG=0`**: ezeknél a fájloknál a címsorban `CANNOT DECODE` jelenik meg, és
  tovább lehet lapozni. Az ép baseline képek sebessége ugyanaz.

A nitrojpeg-ben a libjpeg-turbót igénylő részek `#ifdef NJ_REFERENCE` mögött vannak: a régi
motor, ahol a libjpeg-turbo dekódolja a sávokat, a CPU-s BGRX kimenet és az egyszálas
bejárás kísérlete. Ezeket csak a mérő- és tesztprogramok használják
(`build/nitrojpeg_ref.o`), a néző nem. A nézőben a tartalék dekóder `#ifdef NV_TURBOJPEG`
mögött van.

A libjpeg-turbo és a benchmark többi függősége (stb_image, Wuffs) letöltése és fordítása
a `third_party/` mappába. Semmit nem telepít a rendszerbe, kb. fél perc:

```
scripts/get-deps.sh
```

A mérő- és tesztprogramok a `samples/` mappában keresik a képeket, ide a saját JPEG-eidet
tedd (vagy symlinket). A mérések 50 saját fotón készültek, ezek nincsenek a repóban.

A `-march=native` miatt a bináris a fordító gép CPU-jára optimalizált.

## Fájlok

- `src/nitrojpeg.c/.h`: önálló párhuzamos JPEG-dekóder (saját Huffman + AVX2 IDCT);
  `NJ_REFERENCE`-szel a libjpeg-turbós összehasonlító részek is
- `src/nitroview.m`: Cocoa + Metal néző; `NV_TURBOJPEG`-gel TurboJPEG-tartalékkal
- `scripts/get-deps.sh`: az opcionális függőségek letöltése és fordítása
- `bench/bench.m`: dekóder-benchmark, `bench/verify.c`: bitpontosság + szinkronstatisztika,
  `bench/robust.c`: hibatűrési teszt, `bench/freqprobe.c`: órajelmérés,
  `bench/sustain.c`: tartós terhelés, `bench/ab.sh`: zajtűrő A/B összehasonlítás
- `bench/results/`: mért eredmények (`results.txt`, `ab_results.txt`, `matrix.txt`, `bands.txt`, `sync.txt`)

## Licenc

MIT, lásd [LICENSE](LICENSE). Az IDCT a Independent JPEG Group libjpeg-jének jidctint.c
algoritmusát valósítja meg: ez a szoftver részben az Independent JPEG Group munkáján alapul.
