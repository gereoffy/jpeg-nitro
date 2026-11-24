# nitrodio: mi van meg a draw.io-ból, és mi hiányzik

## Mérce: helyes rajz, nem pixelpontos

A draw.io fájl nem kész kép, mint a PDF vagy a CDR, hanem egy gráfmodell: blokkok, köztük kapcsolatok, rajtuk
feliratok. A blokkok helye és mérete pontos adat, az élek viszont csak azt tárolják, mit kötnek össze, és legfeljebb
néhány töréspontot. A vonalak útját a draw.io a megnyitáskor számolja ki. A feliratok tördelése a betűtípustól függ,
ezért két gépen maga a draw.io is másképp rajzolhat.

A nitrodio célja ezért a helyes rajz:
- a diagram szerkezete jó;
- a nyilak nem fordulnak meg, és oda mutatnak, ahova kell;
- a feliratok ott vannak, ahova tartoznak.

A vonalvezetésnek nincs más leírása, mint a draw.io kódja, ezért az útvonal-algoritmusokat a forrás alapján követjük.
A néhány pixeles eltérést csak akkor javítjuk, ha ez kevés kódba kerül.

A fájlszámok a mintakészletből jönnek (`samples_drawio/reference-tools/files.txt`, 1094 fájl), minden oldal minden
cellastílusát összeszámolva. Ebből 785 fájl a draw.io saját példája vagy sablonja (`downloaded*/jgraph`, és a
gallery-* SVG-k beágyazott rajzai), 309 fájl más forrásból jön. Ahol két szám szerepel, a második ez a 309-es,
„külső” rész. A `samples_drawio/project_docs/` 184 valódi projektmintája külön készlet, ugyanígy ellenőrizve.

## Felépítés

A nitrodio nem futtatja és nem fordítja le az mxGraph-ot. A rajzolás lépéseit írja újra C++-ban, és a draw.io
JS-forrásaiból azt portolja át, ami a képhez kell. Az egész egy fájl: `src/nitrodio.cpp`.

1. **Beolvasás.** Az `<mxfile>` / `<diagram>` oldalak kicsomagolása (base64, raw deflate, URL-kódolás),
   libxml2. Utána a cellák, a `mxGeometry` (számai milliomodra kerekítve, a draw.io lebegőpontos zaja miatt) és a
   stílusstringek (`readCells`, `parseStyle`), a draw.io `default.xml` stíluslapjának alapértékeivel
   (`applyNamed`).
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

Ez nem csak „az alapdolgok”: a nehéz rész nagyobbik fele itt van. Az egész megvan.

- **Élvezetés** (`mxEdgeStyle`): OrthConnector, SegmentConnector, ElbowConnector (vertical / horizontal),
  EntityRelation, Loop. A SideToSide és a TopToBottom a Graph.js-ben felül van írva, ezek onnan jönnek.
  `curved=1`, `rounded=1`, `orthogonalLoop`, `portConstraint`, `jettySize`. Az OrthConnector a draw.io-éval
  egyezően tizedre kerekíti a bemenetét, a szabad végpontot 1×1-es doboznak veszi, és elforgatott terminálnál a
  forgatott befoglaló dobozzal számol. Az EntityRelation a szabad induló pontot is terminálnak veszi.
- **Élvégek** (`mxGraphView`): `sourcePort` / `targetPort` (a megnevezett port-cella veszi át a véget),
  `perimeterSpacing`, `sourcePerimeterSpacing` / `targetPerimeterSpacing`, `fixedPointSpacing`. Rejtett vagy
  összecsukott (`collapsed`) konténerben lévő terminál helyett a látható őse (getVisibleTerminal).
- **Modell:** rétegek, `visible`, összecsukott konténerek (a tartalmuk nem látszik); `placeholders=1`: a címke
  `%név%` helyőrzői a cella vagy az ősei `UserObject` / `object` adataiból (`%id%`, `%width%`, `%height%` is);
  `transparentBounds=1` konténerek: a doboz a gyerekek köré számolva, `groupPadding`-gel.
- **Színek:** `#rgb`, `#rrggbb`, `#rgba`, `#rrggbbaa`, `rgb()` / `rgba()`, a gyakori nevek, `none`, `default`,
  `light-dark(a, b)` (a világos); más színre hivatkozók (mxCellRenderer.resolveColor): `inherit`,
  `parentFillColor` / `parentStrokeColor`, `swimlane`, `fillColor` / `strokeColor`.
- **Perimeterek** (`mxPerimeter`): rectangle, ellipse, rhombus, triangle, hexagon, hexagon2, trapezoid,
  parallelogram, step, center, orthogonal, lifeline, backbone, callout. Port- és constraint-pontok
  (`entryX/exitX…`, `getConnectionPoint`, `getPerimeterPoint`). A tükrözött (`flipH` / `flipV`) alakzaton a draw.io
  alapértelmezése szerint (`legacyAnchorPoints`) csak a kerületre nem vetített pont tükröződik.
- **Markerek** (`mxMarker`): classic, block, open, oval, diamond, és a `…Thin` változatok. Az oval a vonal végére
  középre kerül, `size` átmérővel. A vonal kitöltött és kitöltés nélküli markernél is a marker tövénél áll meg.
- **Címkék** (`mxText`, `mxCellRenderer.getLabelBounds`, `mxSvgCanvas2D`): igazítás, `labelPosition` /
  `verticalLabelPosition`, `labelWidth` (a címke szélessége, az `align` szerint elhelyezve), `spacing*`, 1,2-es sorköz, tördelés (`whiteSpace=wrap`, lásd a HTML-részt), `overflow`,
  `horizontal=0`, `textDirection` (vertical-lr / -rl), `labelBackgroundColor`, `labelBorderColor` /
  `labelBorderWidth`, `noLabel`, az alapvonal helye. A méret nélküli címkecella (például egy él végén) a pontjához
  igazodik az `align` / `verticalAlign` és a spacing szerint, mint a draw.io-ban (negatív szélességű dobozzal).
- **Él-címkék**: az él mentén a relatív helyükön, a `rotation`-jükkel elforgatva, `horizontal=0`-val függőlegesen,
  `labelAutoRotate=1`-gyel a legközelebbi élszakasz irányába, olvasható állásban (Graph.getLabelAutoRotation).
  Az él saját alakú (kitöltéssel vagy körvonallal rendelkező) gyerek-cellái teljes csúcsként rajzolódnak, a bal
  felső sarkuk az él menti pont plusz az offset (mxGraphView.updateVertexState).
- **mxShape**: `direction`, `flipH/V`, `rotation`, `shadow`, `glass` (téglalapon és a swimlane fejlécén), gradient
  (`gradientDirection` north/south/east/west/radial), `dashed` / `dashPattern` / `fixDash`, átlátszóságok,
  `arcSize` / `absoluteArcSize`.
- **mxSwimlane**: fejléc és törzs külön körvonallal, `rounded`, `swimlaneHead` / `swimlaneBody` /
  `swimlaneLine`, `swimlaneFillColor`, `footerSize`, `separatorColor`, kép a fejlécben, glass.
- **mxLabel**: a draw.io alapértelmezett csúcs-alakzata (`default.xml`: shape=label), így az alakzat nélküli
  csúcsoké is: kép (`image`, `imageWidth/Height`, `imageAlign`, `imageVerticalAlign`) és indikátor
  (`indicatorShape`, `indicatorColor`, `indicatorGradientColor`, `indicatorImage`).
- **mxCylinder**: a fedő magassága `size`-ból vagy a magasság ötödéből (legfeljebb 40); `boundedLbl=1`: a címke a
  fedő alatt.
- **A 16 alapalakzat**: rectangle, ellipse, doubleEllipse, rhombus, cylinder, actor, triangle, hexagon, cloud,
  line, arrow, arrowConnector, swimlane, image, label, connector. Az arrowConnector a draw.io `flexArrow` és `link`
  nevén van meg; a ritka `shape=arrowConnector` név sima vonalként rajzolódik.
- **mxStencil**: a stencil-XML értelmezője (szöveg, dash, gradient, `linecap` / `linejoin` / `miterlimit` a stencilben
  is; a saját szövege az alakzattal együtt fordul és tükröződik, tükörírás nélkül), és az
  `mxStencilRegistry` névképzése.

A draw.io rétegéből (Graph.js) hiányzik:

| Mi | Mit csinál | Fájl |
|---|---|---|
| `jumpStyle` (arc / gap / sharp / line) | ívecske vagy rés, ahol két él keresztezi egymást | 35 / 6 (bonyolult, kihagyva) |
| `sequenceEdgeStyle` | szekvenciadiagram üzenetélei (Shapes.js) | 4 / 0 |
| `shadowColor`, `shadowBlur`, `shadowOffset*` | egyedi árnyék | 4 / 0 |
| `comic=1` | régi kézirajz-stílus: kerek vonalvégek | ritka |

A szerkesztőnek szóló kulcsokkal (`resizable`, `collapsible`, `container`, `childLayout`, `autosize`, …) nem
kell foglalkozni: a geometria már a fájlban van.

## 2. Shapes.js (draw.io)

A Shapes.js 99 alakzatot, élet és markert regisztrál. **58 megvan**:

- **Csúcsok:** process (a címke a két sáv között), note, note2, card, cube, tape, document (`boundedLbl`: a
  hullám fölött), step, parallelogram, trapezoid, folder, component, datastore, dataStorage, delay,
  offPageConnector, umlActor, umlLifeline, umlFrame, umlDestroy, umlBoundary, umlControl, seqActorStick, table,
  tableRow, partialRectangle, singleArrow, crossbar, curlyBracket (`rounded=1`: ívelt sarkok, nyitott
  töröttvonalként, mint mxShape.addPoints), cylinder3 (`boundedLbl`: a fedő alatt), isoCube, isoCube2,
  isoRectangle, lollipop, module, or, xor, plus, cross, corner, startState, endState, dimension, message, smileyFace,
  switch, waypoint, hexagon, callout (a csúcsa kerekítés nélkül, a címkemargó az iránnyal együtt fordul),
  loopLimit, collate.
- **Élek:** flexArrow, link, wire, pipe, filledEdge, taperedArrow, mermaidSankeyLink.
- **Extra markerek** (Shapes.js / Graph.js): async, openAsync, box, circle, circlePlus, cross, dash, halfCircle,
  doubleBlock, baseDash, ER-végek (ERone, ERmandOne, ERmany, ERoneToMany, ERzeroToMany, ERzeroToOne).
- **Egyebek:** IsometricConnector élstílus, callout perimeter; `flowAnimation=1`: a folytonos vonal szaggatottan,
  állóképként (Graph.addFlowAnimationToNode mintázata).

**41 hiányzik**, de a mintákban szinte egyik sem fordul elő:

| Alakzat | Fájl (mind draw.io-példa) |
|---|---|
| gitMergeCommit, gitTag | 2 |
| ishikawaHead | 2 |
| umlState | 1 |
| a többi 34 (wedgeCallout, manualInput, internalStorage, sortShape, tapeData, process2, doubleArrow, umlEntity, providedRequiredInterface, seq\*, mermaid\*, mindmapBang, zigzag, …) | 0 |

A folyamatábrák gyakori alakzatai (manual input, display, internal storage…) a mintákban `mxgraph.flowchart.*` stencilként
szerepelnek, és azok megvannak (lásd 3.).

## 3. `mxgraph.*` családok

A `shape=mxgraph.<család>.<név>` alakzatok két helyről jöhetnek.

**Stencil-XML** (draw.io `js/stencils.min.js`). Beépítve a 12 általános célú könyvtár, 1455 alakzat: basic, arrows,
flowchart, bpmn, lean_mapping, eip, floorplan, cabinets, networks, electrical, pid, mockup. A fejlécet a
`scripts/get-deps.sh` generálja (`scripts/gen-drawio-stencils.py`), más készlet a `DRAWIO_STENCILS=…` változóval
állítható (BUILDING.md). A felhő- és gyártói ikonkészletek (gcp2, office, aws4, azure, cisco, signs, ios7…)
szándékosan nincsenek benne.

**Kódból rajzolt alakzatok** a sidebar JS-fájlokban (718 regisztrált `mxgraph.*` név). Ezekből meg van írva:

- basic: arc, pie, partConcEllipse, diag_snip_rect, rectCallout;
- floorplan: wall, wallCorner, wallU, room, doorLeft/Right/Double, window, stairs;
- arrows2: arrow, sharpArrow2, twoWayArrow, stylisedArrow, uTurnArrow, jumpInArrow;
- a Shapes.js `display`, `requiredInterface` és `ext` (`double=1`: belső kerettel) alakzata;
- infographic: cylinder, shadedCube, ribbonSimple;
- gcp2.doubleRect;
- mockup.markup: curlyBrace, redX;
- sysml.package2;
- sysml.x (áthúzás: két átló, kitöltés nélkül), sysml.actFinal (kör, benne 5-tel beljebb a körvonal színével
  kitöltött kör);
- bpmn.event (körvonal: standard, eventNonint szaggatott, catching / boundInt / boundNonint dupla kör, throwing, end
  háromszoros vastagsággal; mind a 13 jel: message, timer, escalation, conditional, link, error, cancel, compensation,
  signal, multiple, parallelMultiple, terminate, star; dobó és záró eseménynél a jel a körvonal színével kitöltve),
  bpmn.gateway2 (rombusz; exclusive X,
  parallel +, complex * a körvonal színével kitöltve; event-based: az esemény körvonala középen), bpmn.task
  (`rectStyle=rounded` lekerekítve `size`-zal);
- electrical.logic_gates.logic_gate (AND / OR / XOR kapu, `numInputs` bemenet, `negating` kör a kimeneten);
- electrical.signal_sources.source (áram- és feszültségforrás: kör, `elSourceType=dependent` esetén rombusz, benne az
  `elSignalType` jele: ac, dc1 / dc2 / dc3, square, pulse, noise …), electro-mechanical.singleSwitch (`elSwitchState`
  on / off), miscellaneous.batteryStack (cellák: kitöltött lemez és vonal; a cellák közti szaggatott rész nélkül),
  abstract.mux2 (mux / demux trapéz a lábaival, `selectorPins`; a lábszámok nélkül);
- aws4: group és resourceIcon keretként.

**Hiányzó ikonok és képek helyett.** A be nem épített gyártói ikonok, az aws4 resource / product iconok és a
draw.io `img/lib/...` képei (azure2, ibm, clip_art…) helyett a `missingIcon()` egy enyhén lekerekített dobozt
rajzol: a stílus `fillColor`-jával (gradienttel), ha nincs, a család színével (`iconFamilyColour()` táblája), a
stílus körvonalával, benne az ikon olvashatóvá alakított nevével (`iconName()`: `route_53` → „route 53”,
`Function_Apps.svg` → „Function Apps”; a nevet aws4-nél a `resIcon` / `prIcon`, networks2.icon-nál a `network2Icon`
adja). Csak ikonokra: legfeljebb 200 px (a wireframe telefon- és tabletkeretei nagyobbak), nem konténer, nem aws4
group; a kódból rajzolt aws3d és a `world` térkép-körvonalai keretek maradnak. A fájlban nem tárolt többi kép
(webcím, más fájl) helyén halványszürke keret, benne a fájl neve a címből (`pictureFrame()`). Külső képet a
nitrodio szándékosan sosem tölt be, se webről, se más fájlból.

### A hiányzó kódból rajzolt családok

| Család | Fájl (összes / külső) | Leggyakoribb hiányzók |
|---|---|---|
| mockup | 25 / 3 | forms.searchBox, graphics.iconGrid, graphics.simpleIcon, forms.comboBox, graphics.pieChart, containers.browserWindow |
| bpmn | 24 / 0 | shape (15), data; a task markerei (user, service …) |
| ios7ui | 12 / 0 | horLines, phone, downloadBar, appBar |
| basic | 11 / 0 | rect, obtuse_triangle, rectCallout, roundRectCallout, drop, polygon |
| lean_mapping | 9 / 0 | outside_sources, electronic_info_flow_edge, inventory_box, physical_pull |
| infographic | 8 / 0 | bannerSingleFold, pyramidStep, circularCallout2, barCallout |
| sysml | 8 / 0 | package (7), … |
| rackGeneral | 7 / 0 | container, rackCabinet3, horCableDuct |
| bootstrap | 6 / 0 | rrect, anchor, checkbox, topButton, radioButton |
| networks | 6 / 0 | comm_link_edge, bus |
| archimate3 / 4 | 5 / 0 | application, service, tech, actor, element |
| c4 | 4 / 3 | person2, person |
| aws3d | 4 / 0 | ~30 féle 3D AWS-ikon |
| ios, aws4, floorplan, eip, pid2\*, sap, er, dfd, gmdl, networks2, android, cisco_safe | 1-3 / 0 | |

A kódból rajzolt családok szinte csak a draw.io saját példáiban és sablonjaiban fordulnak elő. A külső
mintákban csak a mockup (3) és a c4 (3) jelenik meg.

**BPMN** = Business Process Model and Notation, az OMG szabványos jelölése üzleti folyamatokra (feladatok,
események, átjárók/elágazások, adatobjektumok). A stencilkönyvtár csak a jelek kis részét adja; a
feladat-, esemény- és átjáró-alakzatokat a draw.io kódból rajzolja (mxBPMN.js: `shape` a körvonalakkal és
~40 belső szimbólummal, `event`, `gateway2`, `task` a taskMarkerrel). Ebből az `event`, a `gateway2` és a `task`
alapja megvan (lásd fent, az esemény összes jelével); a `shape` alakzat és a ~40 belső jele nincs.

## Hol vannak az alakzatok a kódban

Nincs registry: az alakzat nevét if-láncok vizsgálják, és az mxGraph-, Shapes.js- és `mxgraph.*`-alakzatok egymás
mellett vannak ugyanazokban a függvényekben (`src/nitrodio.cpp`):

| Hol | Mit dönt el |
|---|---|
| `shapePath()` | A legtöbb csúcs-alakzat körvonala és a ráhúzott belső vonalai egy `if (shape == "…") … else if` láncban. Ismeretlen névre a keret. Segédei: `polygon()` (zárt, kerekíthető sarkokkal), `openPolyline()` (nyitott). |
| `turnsByDirection()` | Névlista: mely alakzatokat forgatja a `direction`. |
| `vertex()` | Az elején a nem egyetlen path-ból álló alakzatok (swimlane / table, stencil, beépített stencil-könyvtár, partialRectangle, umlLifeline, umlFrame, waypoint, image, hiányzó ikon); az általános ág után az alakzatfüggő rátétek (shadedCube lapjai, infographic.cylinder fénye, glass, cube sötétítése, a label képe); a végén a címke helye alakzatonként (cube, cylinder, cylinder3, document, process, callout, umlBoundary, umlControl, arrows2.arrow, folder, umlFrame, swimlane). |
| `swimlane()`, `labelForeground()`, `glass()`, `missingIcon()`, `pictureFrame()` | Az mxSwimlane, az mxLabel képe / indikátora, a glass-fény, a hiányzó ikonok és képek helyettesítője. |
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

- A HTML-címkék egyszerűsített böngészős szöveg- és táblázat-layoutja (`parseLabel`, `layoutLabel`, `flowTable`,
  `flowHtml`).
- Betűtípus-fallback karakterenként (`nf_glyph_face`).
- A `sketch=1` kézzel rajzolt kinézet: a rough.js mintájára, de saját véletlenszámokkal, így hasonló, de nem
  pixelre azonos (`roughPath`, `fillStyle` hachure / cross-hatch). Ívelt alakzatokon (henger teteje, lekerekített
  nyíl) és szaggatott vonalon kevésbé pontos.
- A beágyazott PNG, JPEG és SVG képek dekódolása (stb_image, a projekt saját dekóderei).
- A matematikai képletek szöveggé írása (lásd lent).

## HTML a címkékben

`html=1` címkéknél a nitrodio a böngésző egyszerűsített változatát utánozza, két rétegben.

**Szöveg (`parseLabel`, `layoutLabel`):**

- **Betűstílus:** `<b>` / `<strong>`, `<i>` / `<em>`, `<u>` / `<ins>`, `<s>` / `<strike>` / `<del>`; `<span>` /
  `<font>` style-jából `font-style: italic` és `text-decoration` (underline, line-through); a cella `fontStyle`-ja
  is.
- **Betűtípus:** `<font face>` és `style="font-family: …"` (az első név; `var(…)` és `inherit` nem), egymásba
  ágyazva. Ha a gépen nincs meg: a nitro_font helyettesítője (`nf_css_face`: a megfelelő család, vagy a neve
  szerint fix szélességű / talpas betű, például Lucida Console, Fira Code → Courier New), különben az alapbetű.
- **Vastagság:** `font-weight` bold / bolder / 600 fölött félkövér; normal / lighter / 600 alatt visszavonja a
  befoglaló (címsor) félkövérségét.
- **Betűméret:** `style="font-size: …"` (px, pt, %, em), `<font size="1..7">`, `<small>` / `<big>`, `<sub>` /
  `<sup>` (kisebb, lejjebb / feljebb tolva).
- **Szín:** `<font color>`, `style="color: …"` egymásba ágyazva (a legbelső számít); inline `background-color`
  kiemelésként.
- **Sortörés és blokkok:** `<br>` és a szövegbeli `\n` (a draw.io is `<br>`-ré alakítja); `<div>`, `<p>`,
  `<h1>`–`<h6>`, `<li>` új sort kezd. Az üres sor (`<div><br></div>`) a blokk betűméretének magasságú.
- **Margók:** `<p>`, címsorok, listák margója a böngészőé szerint, `margin` / `margin-top` / `margin-bottom`
  felülírja. Az első blokk felső és az utolsó alsó margója is a címke magasságába számít (a draw.io címkéje
  inline-block), levágott címkénél (`overflow=hidden`) az utolsó alsó margó nem.
- **Sormagasság:** CSS `line-height`: szám (a sor betűméretének szorzója, így öröklődik), hossz vagy % (a tag
  saját betűméretéből, px-ként öröklődik), `normal` (1,2).
- **Tördelés:** szóköznél, betűk vagy számok közötti kötőjel után, és CJK írásjelek között (záró jel előtt és
  nyitó után nem), mint a böngésző. A címke blokkja inline-block: olyan széles, mint a leghosszabb sora, ha az
  elfér, különben a tördelési szélesség, de legalább a leghosszabb szó (flex-elem, `min-width: auto`); ezt a
  blokkot helyezi el az igazítás, a sorok a blokkon belül a saját `text-align`-jük szerint állnak.
- **Igazítás:** `text-align` és `align` blokkonként, `margin-left` behúzás.
- **Listák:** `<ul>` pöttyel, `<ol>` számozva, egymásba ágyazva is.
- **`<hr>`:** vízszintes vonal.
- **Entitások:** `&nbsp;` `&amp;` `&lt;` `&gt;` `&quot;` `&apos;`, a HTML 4 névtáblája (247 név: ékezetek, görög
  betűk, jelek), számos `&#…;` / `&#x…;`.
- **Szóközök:** összevonva, mint a böngészőben.

**Blokkok (`flowHtml`, `flowTable`)**, ha a címkében `<table>`, háttérszínes `<div>` vagy `<pre>` van:

- **Táblázatok:** `colspan` / `rowspan`, `cellpadding` / `cellspacing`, `border-collapse`; cellánként `width`
  (px, %) és `height` (px, a tartalomé, a padding hozzájön), soronként `height` (az egész soré), `padding`,
  `text-align`, `vertical-align`; háttér (`background-color`, `background`, `bgcolor`) és keret (`border`);
  egymásba ágyazott táblák is, korláttal a futásidő miatt.
- **Háttérszínes `<div>`:** saját doboz, `width`-tel és `padding`-dal.
- **`<pre>`:** blokk, a szóközei megmaradnak (tab: 4 szóköz), alapból `monospace` betűvel; háttérrel saját doboz
  `padding`-dal (az em a `<pre>` saját betűméretéből). A kerete és a margója nincs megrajzolva.

**Hiányzik:**

| Mi | Mit jelent | Megjegyzés |
|---|---|---|
| `<a>` linkek | a draw.io kéken, aláhúzva mutatja, nálunk sima szöveg | a CSV-importos org-chart mintákon látszik („Email”) |
| `<img>` és beillesztett `<svg>` a címkében | kimarad | lásd alább |
| `white-space: pre` CSS-ként (`<pre>` nélkül), `letter-spacing` | nincs kezelve | |
| jobbról balra írás (arab, héber) | nincs bidi-rendezés és arab betűalak-formálás | betűformáló könyvtár kellene hozzá |

**Kép a címke szövegében (`<svg>`, `<img>`):** kevés rajz használja. A teljes mintakészletben 6 fájl tartalmaz
beillesztett `<svg>`-t: weboldalról bemásolt ikonokat és egy kész MathJax-képletet (`TP.drawio`). `<img>` 3 fájlban
van: kettőben webes URL, amit elvből nem töltünk le, egyben beágyazott `data:` PNG. Magát az SVG-t a nitrosvg ki
tudná rajzolni (`svgPicture()`, mint a `data:image/svg+xml` képeket), de a címke szövegébe illeszteni bonyolult. A
címkék elrendezése ma csak szöveget és hátteret ismer, a képhez kellene:
- a mérete (`ex` egységben is);
- a sorba illesztése a `vertical-align` szerint;
- a forgatása és a vágása a címkével együtt.

A beillesztett SVG szövegét a nitrosvg ráadásul a saját betűkezelésével rajzolná, a címke betűitől függetlenül.
Ennyi fájlért nem éri meg; ha egy képlet kell, a draw.io `$$...$$`-ként menti, és azt kezeljük.

## Matematika (`math="1"`)

Ha az oldal `mxGraphModel`-jén `math="1"` áll, a draw.io MathJax-szal szedi a címkék képleteit: `$$…$$` és
`\(…\)` TeX, `` `…` `` AsciiMath. A nitrodio nem szed képletet, hanem a MathJax kinézetéhez közeli HTML-szöveggé
írja át, mielőtt a címke-layout megkapja (`mathText()`, `texText()`, `asciiMathText()`, hívja a `readModel()`).
A referencia-renderek MathJax-szal készülnek (`samples_drawio/reference-tools/math/`).

**Megvan:**

- **Betűk:** HTML-címkében talpas betűvel (`serif`), 117 %-ra nagyítva, mint a MathJax, ami a kisbetűk magasságát a
  szövegéhez igazítja. A latin és a kisbetűs görög betűk dőltek; állók a számok, a `\text{…}`, `\mathrm`,
  `\operatorname`, `\mathbf` (félkövér) tartalma és a függvénynevek (`\sin`, AsciiMath-ban `sin`, `log` …).
  `\underline` aláhúz, `\color{c}` / `\textcolor{c}{…}` színez.
- **Térközök:** a képletbeli szóköz elmarad, mint a TeX-ben; a műveleti jelek (+ − × ∪ …) és a relációk (= ≤ → …)
  két oldalán nem törhető szóköz, a kezdő vagy műveleti jel utáni előjel nélküle. A képlet nem tördelődik.
- **Jelek Unicode-ként:** görög betűk (`\alpha` → α, `\Omega` → Ω), nyilak (`\leftarrow` → ←, `\Rightarrow` → ⇒,
  `\mapsto` → ↦), műveleti és relációjelek (`\times` ×, `\cdot` ⋅, `\pm` ±, `\le` ≤, `\ne` ≠, `\approx` ≈,
  `\in` ∈, `\subseteq` ⊆, `\infty` ∞, `\partial` ∂, `\nabla` ∇, `\sum` ∑, `\int` ∫ …), `\mathbb{R}` → ℝ.
- **Index:** `^` és `_` HTML-címkében `<sup>` / `<sub>`, sima címkében `^x` / `_(xy)`.
- **Tört, gyök, szöveg:** `\frac{a}{b}` és `a \over b` → a/b (összetettnél (a)/(b)), `\sqrt{x}` → √x, `\sqrt[3]{x}`
  → ∛x (`[4]`: ∜, más index a jel előtt: HTML-címkében felső indexként, sima címkében `^n`), `\text{…}`
  és társai szó szerint.
- **Ékezetek:** `\hat`, `\bar`, `\dot`, `\ddot`, `\tilde` latin betűn előre összerakott betűként (Ŵ, Ā, Ñ, Ż, Ä);
  máshol és a `\vec` kombináló ékezettel.
- **Szerkezet:** `\\` sortörés, `&` (mátrix) szóköz, `\left` / `\right` / `\big`, `\hline` és a környezetnevek
  (`\begin{pmatrix}`, egy `array` oszlopleírása) eldobva, `\lgroup` / `\rgroup` zárójelként, `\&` és társai
  szó szerint, más parancs a nevével (`\sin`, `\log`, `\lim`).
- **AsciiMath:** ugyanazok a szavak (alpha, sum …) és `->`, `=>`, `<=`, `!=`, `xx`, `+-`; a saját szavai is
  (`oo` ∞, `pi`, `mu`, `in` ∈, `le`, `ge`, `ne`, `to`, `uu` ∪, `nn` ∩, `AA` ∀, `EE` ∃, `RR` ℝ, `del` ∂ …), a
  műveletet vagy relációt jelentő szó a jelével azonos térközzel; `sqrt` → √, `text(…)` szó szerint, a beírt
  TeX-parancsok (`\text{…}`) is; `^` / `_` zárójeles vagy `{…}` csoporttal. A szó egész betűsorozat: `2 pi r` → 2πr,
  de `2pir` betűk marad (az AsciiMath ott is felismerné).

**Hiányzik:**

| Mi | Megjegyzés |
|---|---|
| valódi képletszedés: tört egymás alatt, gyökjel a kifejezés fölött, mátrix rácsban | TeX-motor kellene hozzá |
| kombináló ékezet helyes elhelyezése (pl. f̂, \vec) | a betűrajzolás nem igazítja az ékezetet: magas betűn rácsúszik |
| a MathJax saját (Computer Modern) betűi, az index kisebb mérete és helye | Times-szal, a HTML `<sub>` / `<sup>` méretével |
| matematika sima (nem HTML) címkében | ott a képlet egyszerű szöveg marad, álló betűkkel |
| a MathJax kész SVG-kimenete a címkében | lásd a HTML-részben az `<svg>`-t |

## Referencia és ellenőrzés

A draw.io saját renderje minden minta mellett `<fájl>.png` / `<fájl>.page<N>.png` (a beágyazott rajzoké a
`<forrás>.drawio` mellett). Az összehasonlító eszközök a `samples_drawio/reference-tools/` mappában vannak
(README.txt ott). Egy változtatás után `round.sh` az előző `scores.tsv` ellen, és minden romló oldalt meg kell
nézni. A referenciát a headless Chrome a hálózat elérésével készítette: webes képek ott látszhatnak, nálunk
szándékosan nem.
