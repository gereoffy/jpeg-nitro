# nitrodio: mi honnan jön a draw.io-ból

Állapot: 2026-11-20 (a db0bf9d commit után). A számok a `samples_drawio/reference-tools/files.txt` 1094 fájljából
jönnek, minden oldal minden cellastílusát összeszámolva. Ebből 785 fájl a draw.io saját példája
(`downloaded*/jgraph`, és a gallery-* SVG-k beágyazott rajzai), **309 fájl más forrásból** jön. Ahol két szám
szerepel, a második ez a 309-es, „külső” rész.

## Felépítés

A nitrodio nem futtatja és nem fordítja le az mxGraph-ot. A rajzolás lépéseit írja újra C++-ban, és a draw.io
JS-forrásaiból azt portolja át, ami a képhez kell. Az egész egy fájl: `src/nitrodio.cpp` (~4800 sor).

1. **Beolvasás.** Az `<mxfile>` / `<diagram>` oldalak kicsomagolása (base64, raw deflate, URL-kódolás),
   libxml2. Utána a cellák, a `mxGeometry` és a stílusstringek (`readCells`, `parseStyle`), a draw.io
   `default.xml` stíluslapjának alapértékeivel (`applyNamed`).
2. **Builder.** Végigmegy a cellákon rajzolási sorrendben. A `vertex()` a csúcsokat, az `edge()` és a
   `drawEdge()` az éleket készíti el, közben számolja az abszolút koordinátákat, a rétegeket és a
   láthatóságot.
3. **Display list.** A végeredmény `Item`-ek listája: path, kitöltés vagy körvonal, gradient, szaggatás,
   címke, kép.
4. **Rajzolás.** A `drawItem()` ezt a listát rajzolja ki plutovg-vel, tetszőleges nagyításban.

A draw.io három rétegből rajzol, és mindháromból van átvett rész:

| Réteg | Forrás | Mi van benne |
|---|---|---|
| mxGraph mag | mxgraph JS könyvtár | modell, routing, perimeterek, markerek, címkék, 16 alapalakzat, stencil-értelmező |
| draw.io | Graph.js, Shapes.js | ~100 további alakzat, markerek, élstílusok, line jump, sketch |
| `mxgraph.*` családok | sidebar JS-fájlok (mxBasic.js, mxBPMN.js, mxMockup.js…) és stencil-XML | ikon- és szakterületi alakzatok |

## 1. Az mxGraph magja

Ez nem csak „az alapdolgok”: a nehéz rész nagyobbik fele itt van. **Az egész megvan** (2026-11-20 óta a
lent felsoroltak is).

- **Élvezetés** (`mxEdgeStyle`): OrthConnector, SegmentConnector, ElbowConnector (vertical / horizontal),
  EntityRelation, Loop. A SideToSide és a TopToBottom a Graph.js-ben felül van írva, ezeket onnan vettem át.
  `curved=1`, `rounded=1`, `orthogonalLoop`, `portConstraint`, `jettySize`.
- **Perimeterek** (`mxPerimeter`): rectangle, ellipse, rhombus, triangle, hexagon, hexagon2, trapezoid,
  parallelogram, step, center, orthogonal, lifeline, backbone, callout. Port- és constraint-pontok
  (`entryX/exitX…`, `getConnectionPoint`, `getPerimeterPoint`).
- **Markerek** (`mxMarker`): classic, block, open, oval, diamond, és a `…Thin` változatok.
- **Címkék** (`mxText`, `mxCellRenderer.getLabelBounds`, `mxSvgCanvas2D`): igazítás, `labelPosition` /
  `verticalLabelPosition`, `spacing*`, 1,2-es sorköz, tördelés (`whiteSpace=wrap`; szóköznél, betűk közötti kötőjel után és CJK írásjelek között (záró jel előtt és nyitó után nem), mint a böngésző), `overflow`,
  `horizontal=0`, `textDirection` (vertical-lr / -rl), `labelBackgroundColor`, `labelBorderColor` /
  `labelBorderWidth`, `noLabel`, az alapvonal helye; él-címkék a `rotation`-jükkel elforgatva,
  `labelAutoRotate=1`: a legközelebbi élszakasz irányába, olvasható állásban (Graph.getLabelAutoRotation).
- **mxShape**: `direction`, `flipH/V`, `rotation`, `shadow`, `glass` (téglalapon és a swimlane fejlécén), gradient
  (`gradientDirection` north/south/east/west/radial), `dashed` / `dashPattern` / `fixDash`, átlátszóságok,
  `arcSize` / `absoluteArcSize`.
- **mxSwimlane**: fejléc és törzs külön körvonallal, `rounded`, `swimlaneHead` / `swimlaneBody` /
  `swimlaneLine`, `swimlaneFillColor`, `footerSize`, `separatorColor`, kép a fejlécben, glass.
- **mxLabel**: a draw.io alapértelmezett csúcs-alakzata (`default.xml`: shape=label), így az alakzat nélküli
  csúcsoké is: kép (`image`, `imageWidth/Height`, `imageAlign`, `imageVerticalAlign`) és indikátor
  (`indicatorShape`, `indicatorColor`, `indicatorGradientColor`, `indicatorImage`).
- **A 16 alapalakzat**: rectangle, ellipse, doubleEllipse, rhombus, cylinder, actor, triangle, hexagon, cloud,
  line, arrow, arrowConnector, swimlane, image, label, connector.
- **mxStencil**: a stencil-XML értelmezője (szöveg, dash, gradient a stencilben is), és az
  `mxStencilRegistry` névképzése.

A 2026-11-20-án pótolt elemek (fájlok száma: összes / külső): `labelBorderColor` (11 / 10), `noLabel=1` (16 / 1),
`fixDash=1` (14 / 0), a swimlane a draw.io szerint (`rounded=1`: 25 / 0, `swimlaneHead/Body=0`: 14 / 0,
`swimlaneLine=0`: 7 / 0), `gradientDirection=radial` (3 / 0), a `label` alakzat képe és indikátora. Ellenőrzés:
a referenciarenderek ellen 0 romlás, egy saját tesztábra (minden elem) a draw.io-val egyezik.

A draw.io rétegéből (Graph.js) hiányzik még:

| Mi | Mit csinál | Fájl |
|---|---|---|
| `jumpStyle` (arc / gap / sharp / line) | ívecske vagy rés, ahol két él keresztezi egymást | 35 / 6 (bonyolult, kihagyva) |
| `sequenceEdgeStyle` | szekvenciadiagram üzenetélei (Shapes.js) | 4 / 0 |
| `shadowColor`, `shadowBlur`, `shadowOffset*` | egyedi árnyék | 4 / 0 |

A szerkesztőnek szóló kulcsokkal (`resizable`, `collapsible`, `container`, `childLayout`, `autosize`, …) nem
kell foglalkozni: a geometria már a fájlban van.

## 2. Shapes.js (draw.io)

A Shapes.js 99 alakzatot, élet és markert regisztrál. **57 megvan**:

- **Csúcsok:** process, note, note2, card, cube, tape, document, step, parallelogram, trapezoid, folder,
  component, datastore, dataStorage, delay, offPageConnector, umlActor, umlLifeline, umlFrame, umlDestroy,
  seqActorStick, table, tableRow, partialRectangle, singleArrow, crossbar, curlyBracket, cylinder3, isoCube,
  isoCube2, isoRectangle, lollipop, module, or, xor, plus, corner, startState, endState, dimension, message,
  smileyFace, switch, waypoint, hexagon; 2026-11-20 óta callout (a csúcsa kerekítés nélkül, a címkemargó az iránnyal
  együtt fordul), loopLimit, collate, umlBoundary, umlControl (az új projektmintákban 1-3 projekt használja őket).
- **Élek:** flexArrow, link, wire, pipe, filledEdge, taperedArrow, mermaidSankeyLink.
- **Extra markerek** (Shapes.js / Graph.js): async, openAsync, box, circle, circlePlus, cross, dash, halfCircle, doubleBlock, baseDash,
  ER-végek (ERone, ERmandOne, ERmany, ERoneToMany, ERzeroToMany, ERzeroToOne).
- **Egyebek:** IsometricConnector élstílus, callout perimeter.

**42 hiányzik**, de a mintákban szinte egyik sem fordul elő:

| Alakzat | Fájl (mind draw.io-példa) |
|---|---|
| gitMergeCommit, gitTag | 2 |
| ishikawaHead | 2 |
| ext, requiredInterface, umlState | 1 |
| a többi 36 (wedgeCallout, manualInput, internalStorage, display, sortShape, tapeData, process2, doubleArrow, cross, umlEntity, providedRequiredInterface, seq\*, mermaid\*, mindmapBang, zigzag, …) | 0 |

A folyamatábrák gyakori alakzatai (manual input, display, collate…) a mintákban `mxgraph.flowchart.*` stencilként
szerepelnek, és azok megvannak (lásd 3.).

## 3. `mxgraph.*` családok

A `shape=mxgraph.<család>.<név>` alakzatok két helyről jöhetnek:

- **Stencil-XML** (draw.io `js/stencils.min.js`). Beépítve a 12 általános célú könyvtár, 1455 alakzat:
  basic, arrows, flowchart, bpmn, lean_mapping, eip, floorplan, cabinets, networks, electrical, pid, mockup.
  A fejlécet a `scripts/get-deps.sh` generálja (`scripts/gen-drawio-stencils.py`), más készlet a
  `DRAWIO_STENCILS=…` változóval állítható (BUILDING.md). A felhő- és gyártói ikonkészletek (gcp2, office,
  aws4, azure, cisco, signs, ios7…) szándékosan maradtak ki. Helyettük (és a draw.io `img/lib/...` képei,
  pl. azure2, ibm, clip_art helyett) a `missingIcon()` egy enyhén lekerekített dobozt rajzol: a stílus
  `fillColor`-jával (gradienttel), ha nincs, a család színével (`iconFamilyColour()` táblája), a stílus
  körvonalával, benne az ikon olvashatóvá alakított nevével (`iconName()`: `route_53` → „route 53”,
  `Function_Apps.svg` → „Function Apps”; aws4 resourceIcon / productIcon: a `resIcon` / `prIcon` kulcsból,
  networks2.icon: a `network2Icon`-ból).
  Csak ikonokra: legfeljebb 200 px (a wireframe telefon- és tabletkeretei nagyobbak), nem konténer, nem aws4
  group; a kódból rajzolt aws3d és a `world` térkép-körvonalai maradnak keretek. A fájlban nem tárolt többi kép
  (webcím, más útvonal) helyén halványszürke keret, benne a fájl neve a címből (`pictureFrame()`:
  `.../jira-icon-512x512-z7na7dot.png` → „jira icon 512x512 z7na7dot”).
- **Kódból rajzolt alakzatok** a sidebar JS-fájlokban (718 regisztrált `mxgraph.*` név). Ezekből csak egy
  maroknyi van átírva:
  - basic: arc, pie, partConcEllipse;
  - floorplan: wall, wallCorner, wallU, room, doorLeft/Right/Double, window, stairs;
  - arrows2.arrow;
  - infographic: cylinder, shadedCube, ribbonSimple;
  - gcp2.doubleRect;
  - mockup.markup.curlyBrace;
  - sysml.x (az áthúzás: két átló, kitöltés nélkül);
  - aws4: group és resourceIcon csak keretként.

### A hiányzó kódból rajzolt családok

| Család | Fájl (összes / külső) | Leggyakoribb hiányzók |
|---|---|---|
| mockup | 25 / 3 | forms.searchBox, graphics.iconGrid, graphics.simpleIcon, forms.comboBox, graphics.pieChart, containers.browserWindow |
| bpmn | 24 / 0 | shape (15), event (8), gateway2 (8), task (5), data |
| ios7ui | 12 / 0 | horLines, phone, downloadBar, appBar |
| basic | 11 / 0 | rect, obtuse_triangle, rectCallout, roundRectCallout, drop, polygon |
| lean_mapping | 9 / 0 | outside_sources, electronic_info_flow_edge, inventory_box, physical_pull |
| arrows2 | 8 / 1 | uTurnArrow, stylisedArrow, twoWayArrow, jumpInArrow |
| infographic | 8 / 0 | bannerSingleFold, pyramidStep, circularCallout2, barCallout |
| sysml | 8 / 0 | package (7), actFinal, … |
| rackGeneral | 7 / 0 | container, rackCabinet3, horCableDuct |
| bootstrap | 6 / 0 | rrect, anchor, checkbox, topButton, radioButton |
| electrical | 6 / 0 | logic_gates.logic_gate, signal_sources.source |
| networks | 6 / 0 | comm_link_edge, bus |
| archimate3 / 4 | 5 / 0 | application, service, tech, actor, element |
| c4 | 4 / 3 | person2, person |
| aws3d | 4 / 0 | ~30 féle 3D AWS-ikon |
| ios, aws4, floorplan, eip, pid2\*, sap, er, dfd, gmdl, networks2, android, cisco_safe | 1-3 / 0 | |

A kódból rajzolt családok szinte csak a draw.io saját példáiban és sablonjaiban fordulnak elő. A külső
mintákban csak a mockup (3), a c4 (3) és az arrows2 (1) jelenik meg.

**BPMN** = Business Process Model and Notation, az OMG szabványos jelölése üzleti folyamatokra (feladatok,
események, átjárók/elágazások, adatobjektumok). A stencilkönyvtár csak a jelek kis részét adja; a
feladat-, esemény- és átjáró-alakzatokat a draw.io kódból rajzolja (mxBPMN.js: `shape` a körvonalakkal és
~40 belső szimbólummal, `event`, `gateway2`, `task` a taskMarkerrel).

## Hol vannak az alakzatok a kódban

Nincs registry: az alakzat nevét if-láncok vizsgálják, és az mxGraph-, Shapes.js- és `mxgraph.*`-alakzatok egymás
mellett vannak ugyanazokban a függvényekben (`src/nitrodio.cpp`):

| Hol | Mit dönt el |
|---|---|
| `shapePath()` | A legtöbb csúcs-alakzat körvonala és a ráhúzott belső vonalai egy `if (shape == "…") … else if` láncban (64 ág). Ismeretlen névre a keret. |
| `turnsByDirection()` | Névlista: mely alakzatokat forgatja a `direction`. |
| `vertex()` | Az elején a nem egyetlen path-ból álló alakzatok (swimlane / table, stencil, beépített stencil-könyvtár, partialRectangle, umlLifeline, umlFrame, waypoint, image); az általános ág után az alakzatfüggő rátétek (shadedCube lapjai, infographic.cylinder fénye, glass, cube sötétítése, a label képe); a végén a címke helye alakzatonként (cube, arrows2.arrow, folder, umlFrame, swimlane). |
| `swimlane()`, `labelForeground()`, `glass()` | Az mxSwimlane, az mxLabel képe / indikátora és a glass-fény. |
| `drawEdge()` | Az él-alakzatok: flexArrow / link (`arrowConnector()`), taperedArrow, mermaidSankeyLink, wire / pipe, filledEdge. |
| `marker()` | A nyílvégek, szintén if-lánccal. |
| `libraryStencil()` | Az egyetlen valódi tábla: a beépített stencilek `unordered_map`-ben név szerint (adat, nem kód). |

Előnye: egyszerű, és egy csúcsra néhány tucat string-összehasonlítás jut, ez a rajzolás mellett elhanyagolható.
Hátránya: egy új alakzat három-négy helyet érinthet (`shapePath`, `turnsByDirection`, esetleg `vertex` és a
címkehely), és ha egyet kihagyunk, a forgatás vagy a címke helye csendben rossz lesz.

Ennél a mennyiségnél nem érdemes átírni. Ha egy nagyobb kódból rajzolt család jön (például a BPMN ~40 belső
szimbólummal), akkor érdemes egy táblát bevezetni (név → path-függvény, forog-e, címkedoboz), amibe egy alakzat
egy sor, és a mostaniak fokozatosan átkerülhetnek.

## Saját megoldások (nem portok)

- A HTML-címkék egyszerűsített böngészős táblázat-layoutja (`flowTable`, `flowHtml`).
- Betűtípus-fallback karakterenként (`nf_glyph_face`).
- A `sketch=1` kézzel rajzolt kinézet: a rough.js mintájára, de saját véletlenszámokkal, így hasonló, de nem
  pixelre azonos (`roughPath`, `fillStyle` hachure / cross-hatch).
- A beágyazott PNG, JPEG és SVG képek dekódolása (stb_image, a projekt saját dekóderei).

## Referencia és ellenőrzés

A draw.io saját renderje minden minta mellett `<fájl>.png` / `<fájl>.page<N>.png`. Az összehasonlító eszközök a
`samples_drawio/reference-tools/` mappában vannak (README.txt ott). Egy változtatás után `round.sh` az előző
`scores.tsv` ellen, és minden romló oldalt meg kell nézni.

## HTML a címkékben

`html=1` címkéknél a nitrodio a böngésző egyszerűsített változatát utánozza, két rétegben.

**Szöveg (`parseLabel`):**

- **Betűstílus:** `<b>` / `<strong>`, `<i>` / `<em>`, `<u>` / `<ins>`, `<s>` / `<strike>` / `<del>`; `<span>` /
  `<font>` style-jából `font-weight: bold`, `font-style: italic`, `text-decoration` (underline, line-through); a
  cella `fontStyle`-ja is.
- **Betűméret:** `style="font-size: …"` (px, pt, %, em), `<font size="1..7">`, `<small>` / `<big>`, `<sub>` /
  `<sup>` (kisebb, lejjebb / feljebb tolva).
- **Szín:** `<font color>`, `style="color: …"` egymásba ágyazva (a legbelső számít); inline `background-color`
  kiemelésként.
- **Sortörés és blokkok:** `<br>` és a szövegbeli `\n` (a draw.io is `<br>`-ré alakítja); `<div>`, `<p>`,
  `<h1>`–`<h6>`, `<li>` új sort kezd; `<p>`, címsorok, listák margója a böngészőé szerint, `margin` /
  `margin-top` / `margin-bottom` felülírja. Az első blokk felső és az utolsó alsó margója is a címke magasságába
  számít (a draw.io címkéje inline-block), így a `<p>`-be csomagolt felirat középen marad; levágott címkénél
  (`overflow=hidden`) az utolsó alsó margó kimarad, ahogy a referencián.
- **Igazítás:** `text-align` és `align` blokkonként, `margin-left` behúzás.
- **Listák:** `<ul>` pöttyel, `<ol>` számozva, egymásba ágyazva is.
- **`<hr>`:** vízszintes vonal.
- **Entitások:** `&nbsp;` `&amp;` `&lt;` `&gt;` `&quot;` `&apos;`, a HTML 4 névtáblája (247 név: ékezetek, görög
  betűk, jelek), számos `&#…;` / `&#x…;`.
- **Sormagasság:** CSS `line-height`: szám (a sor betűméretének szorzója, így öröklődik), hossz vagy % (a tag
  saját betűméretéből, px-ként öröklődik), `normal` (1,2). A szöveges sorok kapják, a blokkmargók helye nem.
- **Szóközök:** összevonva, mint a böngészőben.

**Blokkok (`flowHtml`, `flowTable`)**, ha a címkében `<table>` vagy háttérszínes `<div>` van:

- **Táblázatok:** `colspan` / `rowspan`, `cellpadding` / `cellspacing`, `border-collapse`; cellánként `width` /
  `height` (px, %), `padding`, `text-align`, `vertical-align`; háttér (`background-color`, `background`,
  `bgcolor`) és keret (`border`); egymásba ágyazott táblák is, korláttal a futásidő miatt.
- **Háttérszínes `<div>`:** saját doboz, `width`-tel és `padding`-dal.

**Hiányzik:**

| Mi | Mit jelent | Megjegyzés |
|---|---|---|
| `font-family` a HTML-ben (`<font face>`, span `font-family`) | csak a cella `fontFamily`-je számít | egyszerű: a betűtípus-választás megvan |
| `<a>` linkek | a draw.io kéken, aláhúzva mutatja, nálunk sima szöveg | a CSV-importos org-chart mintákon látszik („Email”) |
| `<img>` és beillesztett `<svg>` a címkében | kimarad | a TP.drawio egyik sora a MathJax kész SVG-kimenete |
| `<pre>` / `white-space: pre`, `letter-spacing` | nincs kezelve | |

## Matematika (`math="1"`)

Ha az oldal `mxGraphModel`-jén `math="1"` áll, a draw.io MathJax-szal szedi a címkék képleteit: `$$…$$` és
`\(…\)` TeX, `` `…` `` AsciiMath. A nitrodio nem szed képletet, hanem olvasható szöveggé írja át, mielőtt a
címke-layout megkapja (`mathText()`, `texText()`, `asciiMathText()`, hívja a `readModel()`):

- **Jelek Unicode-ként:** görög betűk (`\alpha` → α, `\Omega` → Ω), nyilak (`\leftarrow` → ←, `\Rightarrow` → ⇒,
  `\mapsto` → ↦), műveleti és relációjelek (`\times` ×, `\cdot` ⋅, `\pm` ±, `\le` ≤, `\ne` ≠, `\approx` ≈,
  `\in` ∈, `\subseteq` ⊆, `\infty` ∞, `\partial` ∂, `\nabla` ∇, `\sum` ∑, `\int` ∫ …), `\mathbb{R}` → ℝ.
- **Index:** `^` és `_` HTML-címkében `<sup>` / `<sub>`, sima címkében `^x` / `_(xy)`.
- **Egyebek:** `\frac{a}{b}` → a/b (összetettnél (a)/(b)), `\sqrt{x}` → √x, `\text{…}` és társai szó szerint,
  `\hat`, `\bar`, `\dot`, `\ddot`, `\tilde` latin betűn előre összerakott betűként (Ŵ, Ā, Ñ, Ż, Ä), máshol és a `\vec`
  kombináló ékezettel (az ékezetet a betűrajzolás nem igazítja: nagybetűn és magas kisbetűn rácsúszna a betűre),
  `\\` sortörés, `&` (mátrix) szóköz,
  `\left` / `\right` / `\big` és a környezetnevek (`\begin{pmatrix}`) eldobva, más parancs a nevével
  (`\sin`, `\log`, `\lim`).
- **AsciiMath:** ugyanazok a szavak (alpha, sum …) és `->`, `=>`, `<=`, `!=`, `xx`, `+-`; `^` / `_` zárójeles
  csoporttal.

Valódi képletszedés (tört egymás alatt, gyökjel a kifejezés fölött, mátrix rácsban) nincs; a mintákban
11 fájl, 19 oldal használ matematikát.
