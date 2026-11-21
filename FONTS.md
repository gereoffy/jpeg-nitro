# Betűtípusok a vektoros olvasókban

A `nitro_font` közös rendszerfont-indexet és Plutovg face-cache-t ad a vektoros olvasóknak. Két
döntést kezel. Először a dokumentumban kért fontot keresi meg, szükség esetén egy teljes felirathoz
választ helyettesítőt. Ha a kiválasztott face-ből hiányzik egy karakter, arra külön keres fontot.
A dekóder adja át a fájlban tárolt nevet, stílust és karaktereket, és megtartja a formátum saját
szövegpozícióit. [Felület](src/nitro_font.h) · [Megvalósítás](src/nitro_font.cpp)

## A rendszerfontok indexe

Az index az első kereséskor épül fel, és a folyamat végéig él. A szokásos rendszer- és felhasználói
fontkönyvtárakban `.ttf`, `.otf`, `.ttc` és `.otc` fájlokat keres. macOS-en az első megtalált Office-app
saját fontkönyvtárát is bejárja. A gyűjtemény minden indexelt face-e önálló jelölt.

Windows 10-től az index a DirectWrite rendszer-fontkészletéből (`GetSystemFontSet`) épül: a nevek, a
súly, a szélesség, a dőlés, a fájl és a gyűjteményen belüli sorszám a Windows fontgyorsítótárából
jön, fontfájl megnyitása nélkül. A szimulált félkövér és dőlt változatok és a nem helyi fontok
kimaradnak. Ha a DirectWrite nem elérhető, a fontkönyvtárat olvassa be; a felhasználói fontkönyvtár
mindig átnézésre kerül, az onnan már ismert fájlokat kihagyja. A sok fontos, lassú lemezű gépeken
így az első keresés nem olvas be száz vagy ezer fájlt.

- A `name` táblából a családnevek (ID 1 és 16), a teljes nevek (ID 4) és a PostScript-nevek (ID 6)
  kerülnek az indexbe, a beolvasható lokalizált változatokkal együtt. A keresés az ASCII betűknél
  figyelmen kívül hagyja a kis- és nagybetűt. A fájlnév külön keresőkulcs.
- A súly és szélesség az `OS/2` táblából jön. A dőlést és a hiányzó súlyhoz a bold jelzőt a
  `head.macStyle` adja. Egy külön családként tárolt szélességváltozat, például az Arial Narrow,
  az alapcsaládhoz is bekerülhet, ha a név és a szélességosztály összhangban van.
- Egy családon belül a dőlés egyezése számít először, aztán a kért szélességosztálytól, végül
  a kért súlytól való távolság. Az alapértelmezett szélesség 5 (normál). Súlyban azonos távolságnál
  400 alatt a könnyebb, 400-tól a nehezebb face kerül előre.

## Az elsődleges font kiválasztása

A natív rajzformátumok név szerinti belépési pontja az `nf_find_face(name, weight, italic, kind, file)`.

1. **A betűtípus saját nevei.** A feloldó leveszi a PDF részhalmaz-előtagját, felismeri többek közt
   a PostScript-stílusutótagokat, a `Semi Bold`, `Light`, `Condensed` típusú jelöléseket, a régi
   `CE`-szerű végződéseket és a Bitstream rövidítéseit. A teljes név is a jelöltek között marad,
   így például egy telepített `Arial-BoldMT` közvetlenül megtalálható. A névváltozatok után a
   feloldott családnév következik.
2. **A dokumentum fájlneve.** Ha a név alapján nincs találat, a külön átadott fájlnevet is próbálja,
   például egy DXF `STYLE` rekordból. A keresés az indexelt fájlok alapneve alapján történik.
   Néhány gyakori régi fájlnévhez család- és stílustérkép is van. Az AutoCAD SHX stroke fontjainál
   a rendszerfontos feloldó a sans-serif ágra jut, ha a formátum más családtípust nem ad meg.
3. **Név szerinti helyettesítők.** Az `nf_substitute()` táblája legfeljebb két lépésben ad
   másik családot. A `Swis721 → Arial → Liberation Sans` lánc erre példa. Álló szövegnél a
   saját és helyettesítő családnevek keresése átugorja a csak dőlt face-eket kínáló családot.
4. **Tartalék font a család típusa szerint.** Az `NF_SERIF`, `NF_MONO` és `NF_SANS` a formátum
   ismert családtípusát viszi át. `NF_ANY` esetén a feloldó a névből ismerhet fel talpas vagy
   fix szélességű fontot. A megfelelő alapcsaládok után a sans-serif lista következik. Ezen az
   utolsó ágon a 600-as kérés 500-ként keres, és köztes súlynál a Helvetica Neue is jelölt.
   Ha egyik jelölt sem tölthető be, a hívás `nullptr`-t adhat.

CSS vagy HTML `font-family` értékéhez az `nf_css_face_list()` kapja meg a teljes, vesszővel
elválasztott listát. A neveket sorrendben oldja fel, a generikus családokat és a név szerinti
helyettesítőket is figyelembe véve. A lista végén a platform alapértelmezett sans-serif keresése
fut, amely végső esetben bármely betölthető indexelt face-t elfogad. Egyetlen CSS név feloldására
az `nf_css_face_weight()` használható. Feloldatlan névre `nullptr`-t ad, így a hívó továbbmehet
a lista következő elemére. A LunaSVG ezt használja egy családhoz, az üres nevet pedig az alapfont
kéréséhez adja át. [SVG-bekötés](src/nitrosvg.cpp) · [draw.io-címkék](src/nitrodio.cpp)

## Karakterenkénti helyettesítés

Az `nf_has_glyph(face, cp)` Unicode-kódpontot vár. Ugyanazt a Plutovg glyph-index lekérdezést
használja, amelyből a rajzoló dolgozik. Ha a kódpont nincs az elsődleges face-ben, a natív
`nf_find_glyph_face()` az eredeti név és stílus alapján keres. A CSS-es út ugyanehhez az
`nf_css_face_list()` negyedik paramétereként kapja meg a kódpontot.

A keresés először a kért családok és helyettesítőik olyan face-eit próbálja, amelyek tartalmazzák
a karaktert. Utána a közös tartaléklista következik, benne CJK- és szimbólumfontokkal. A közös
listán családonként egy, a stílushoz legközelebbi betölthető face-t ellenőriz, így egy hiányzó
karakter miatt sem kell a nagy gyűjtemények minden stílusát betölteni. Ha nincs találat, a hívó
megtarthatja az elsődleges face hiányjelét, vagy felhasználhatja a fájlban tárolt előrelépést
akkor is, ha a konkrét glyph nem rajzolható meg.

## Gyorsítótár és szálak

- **Index és fontadat.** A könyvtárbejáráskor a név- és stílustáblákat olvassuk. A font teljes
  bájtsora az első sikeres face-betöltéskor marad a cache-ben, fájlútvonal szerint. Egy TTC/OTC
  face-ei ugyanazt a bájtsort használják, saját gyűjteményindexszel. Sikertelen betöltés után
  a keresés a következő jelölttel folytatódik, a sikertelen face-t a folyamat megjegyzi.
- **Tulajdonjog.** A feloldóktól kapott `plutovg_font_face_t *` kölcsönzött mutató. Egy dokumentum
  elteheti a saját szövegstílusában vagy display listájában. A cache referenciája a folyamat
  végéig életben tartja a face-t. Ha a hívó külön saját referenciát vesz fel, azt ő engedi el.
- **Zárolás.** Az indexhez, a face-ek betöltéséhez és a helyettesítő kereséséhez a hívó fogja az
  `nf_lock()` mutexet. A már kiválasztott face-ekkel, saját canvast használva, a Plutovg
  glyph-cache-e mellett a renderelés futhat párhuzamosan. A cache találatai zárolás nélkül
  olvashatók, a hiányzó glyph-eket a Plutovg face-enkénti mutex alatt tölti be. Az `nf_lock()`
  nem rekurzív. Engedd el, mielőtt olyan olvasót hívsz, amely maga veszi fel, például egy
  beágyazott SVG megnyitásakor.

## Bekötés új olvasóba

| A formátumból elérhető adat | Használandó belépési pont |
| --- | --- |
| Fontnév, súly, dőlés, esetleg családtípus és fájlnév | `nf_find_face()`, hiányzó karakterhez `nf_find_glyph_face()` |
| CSS-es családlista és számszerű súly | `nf_css_face_list()`, hiányzó karakterhez ugyanez a kódponttal |
| Egyetlen CSS-es családnév, a listát a hívó járja be | `nf_css_face_weight()`. A `bool` súlyt tároló régi hívóknak `nf_css_face()` |
| Már összeállított, nullával lezárt családlista | `nf_first_face()`, karakterhez `nf_glyph_face()` |
| Beágyazott font vagy a dokumentum saját glyph-azonosítói | A formátum saját font- és karakterleképezése. Rendszerfonthoz a közös feloldó akkor használható, ha van hozzá karakterleképezés vagy azonosítható az eredeti face. |

1. **Tartsd meg az eredeti kérést a szövegstílusban.** Add át a teljes fontnevet vagy családlistát
   és a formátum súlyát. A szokásos alapérték 400, a csak félkövér jelzőt adó formátumoknál a
   félkövér 700. Az `NfKind` a fájlból ismert családtípushoz való. DXF-hez hasonló formátumnál
   add át külön a családnevet és a fájlnevet. A közös névértelmezést és a helyettesítő táblát a
   `nitro_font`-ban bővítsd, ha egy új névalak több olvasónál is használható.
2. **Oldd fel az elsődleges face-t stílusonként, a karaktereket pedig tördelés előtt.** A hiányzó
   glyph keresésekor ugyanazt a súlyt, dőlést és teljes eredeti fontkérést add át. A szomszédos,
   azonos face-hez tartozó karaktereket érdemes egy futamba fogni. A méréshez és a rajzoláshoz
   ugyanazt a face-t használd. Ha a fájl saját pozíciókat vagy előrelépést ad, azok alapján
   helyezd el a helyettesítő glyph-et is.
3. **Tartsd a karakterkódolást és a glyph-azonosítókat a formátum olvasójában.** Az örökölt
   szövegkódolásokat Unicode-kódpontokra kell alakítani a karakterenkénti keresés előtt. A fájl
   saját glyph-sorszáma egy konkrét face-hez tartozik. A PDF-olvasó a beágyazott fontot
   FreeType-pal nyitja meg. Nem beágyazott `CIDFontType2` esetén, `/ToUnicode` nélkül,
   az `nf_face_file()` csak egyező PostScript-névnél ad fájlnevet és gyűjteményindexet a
   glyph-sorszámos ághoz. [PDF fontkezelés](src/nitropdf.cpp)

Példa egy név alapú szövegfutam feloldására. A `text` már dekódolt Unicode-szöveg.

```cpp
#include "nitro_font.h"
#include <string_view>

std::vector<plutovg_font_face_t *> resolve_text_faces(
    std::u32string_view text, const std::string &name, const std::string &file,
    int weight, bool italic, NfKind kind)
{
    std::vector<plutovg_font_face_t *> faces;
    faces.reserve(text.size());

    std::lock_guard<std::mutex> lock(nf_lock());
    auto *primary = nf_find_face(name.c_str(), weight, italic, kind, file.c_str());
    for (uint32_t cp : text) {
        auto *face = primary;
        if (!nf_has_glyph(face, cp)) {
            if (auto *fallback = nf_find_glyph_face(name.c_str(), weight, italic, cp, file.c_str()))
                face = fallback;
        }
        faces.push_back(face);
    }
    return faces;
}
```

A DXF-olvasóban a `TStyle` tárolja az eredeti kérést, a feloldott face-t és a hiányzó
karakterekhez már megtalált glyph-eket. A körvonal-cache annak a face-nek a mutatóját használja
kulcsként, amelyből a glyph ténylegesen származik, a kódponttal együtt. A draw.io az azonos
face-ű futamokat a címke tördelésénél alakítja ki. [DXF szövegstílus](src/nitrodxf.cpp) ·
[draw.io szövegtördelés](src/nitrodio.cpp)

A Plutovg glyph-index lekérdezése és a LunaSVG súly- és karakterfeloldó callbackjei a LunaSVG
forkunkban vannak, egy-egy commitként; a `scripts/get-deps.sh` ezt a rögzített commitot tölti le.
[LunaSVG fork](https://github.com/gereoffy/lunasvg)
