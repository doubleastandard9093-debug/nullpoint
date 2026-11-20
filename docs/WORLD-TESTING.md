# World testing (universe, travel, ships, economy, NPCs, territory)

This is the in-editor test guide for the systems built on top of M1. `M1-TESTING.md` still covers flight, combat and controls.

**No map is needed.** Open any empty level (File → New Level → Empty Level, or the default level), make sure **World Settings → GameMode Override** is empty or `HDGameMode` (it is the project default), and press Play. The game mode spawns the system director, which generates and spawns the start system around you. If a level has Player Starts they are ignored for placement: pilots are always moved into their system.

Console: press `` ` `` in PIE. Every command starts with `HD`. Results show bottom-left on screen and in the Output Log (filter `Nullpoint` or `LogHelios`).

## 1. Universe (milestone 1)
**What it is.** A seeded galaxy of 5,000 systems (config `[HeliosDrift.Universe]` in `Config/DefaultGame.ini`, or `-UniverseSeed=` / `-SystemCount=` on the command line). Server and clients generate the same galaxy from the same seed; the game state replicates the seed and clients regenerate if theirs differs.
- **Layout:** a four-armed disk about 400 ly across, ~4 ly between neighbours, in regions ("... Reach") each owned by an NPC faction.
- **Security bands** by distance from the core: Secured (core), Restricted, Borderland, Frontier / The Black (rim).
- **Gate network:** connects every non-Frontier system (spanning tree plus loops). About 7% of hub-to-hub gates are **large gates**, the only standard gates capitals fit through.
- **Frontier has no gates.** Small ships get there only through **wormholes**: two stable ones per Frontier region from the nearest Borderland space, plus a weekly set of transient ones (Frontier↔Frontier and Frontier↔Borderland/Restricted). All wormholes are hidden until scanned.
- **Stellar anomalies:** hidden long-throw links (15–45 ly), any band.
- **Galactic gates (ruins):** a few inert ruins at the outer rim of The Black.
- **Each system:** a star; 3–10 planets (rocky, desert, lava, terran, water, ice, gas giants) with moons; large asteroids; asteroid belts, ice fields, gas clouds; stations in every settled system (Accord civic, faction industrial, corporate modular, pirate kitbash); gas-giant fuel platforms; 4–11 sites (signal fires, contraband drifts, rogue nests, dead signatures, hulks Class 1–4, deep-core anomalies, pirate bases, sentinel vaults, derelict stations); NPC trade lanes in Secured (and some Restricted) systems; instance jump lanes to instanced pockets.
- **Rules baked in:** Class 3–4 hulks never generate in Secured. Generated landable bodies are ≤ 2.0 g. Gas giants are never landable. Gas-giant cloud density re-rolls weekly: Rare 30% / Consistent 50% / Abundant 20%.
- **Scale:** 1 AU = 100,000 km of world space. Planets render at 25% of real radius, stars at 2%. Landing altitudes (25 km / 3 km) are real world distances above the rendered surface.
- **Runtime spawner:** `AHDSystemDirector` puts each occupied system in its own world slot (up to 16 at once, 20 million km apart), spawns its bodies, stations, gates, sites and lanes, streams asteroids in within 1,000 km of a pilot and site contents within 500 km, and tears down systems that have been empty for 2 minutes. Week changes rebuild active systems.

**Galaxy map data API** (`UHDUniverseSubsystem`, C++/Blueprint): `GetSystem`, `GetLinks`, `GetSystemsNear`, `FindGateRoute` (small ships), `FindJumpRoute` (capitals, by range), `FindSystem`, `DistanceLy`, `GetGasDensity`, `GetWeekIndex`.

### Console
| Command | Does |
|---|---|
| `HDSystemInfo` | Lists every body, station, link, site with distance. |
| `HDGalaxy 15` | Systems within 15 ly, with band. |
| `HDFind Kalen` / `HDFind NP-0042` | Finds a system, distance, gate jumps from here. |
| `HDGoto NP-0042` (or a name or number) | Teleports your ship to that system's arrival point. |
| `HDGas` | Gas giants here: this week's density and the next four weeks. |
| `HDWeek 3000` / `HDWeek live` | Overrides the week index; active systems rebuild with new densities and wormholes. |
| `HDRevealAll` | Reveals all hidden signatures in this system (wormholes, sites). |

### Checklist
- [ ] Empty level + Play: log shows `Universe generated: seed 1337, 5000 systems` and `Director: activated ...`. You spawn 20 km from the start station, with three drones ahead and one 20 km to your right.
- [ ] HUD top line shows `NP-xxxx Name [Secured]`.
- [ ] The station, planets and the star are visible (planets are big spheres, the star is bright). The ship is lit from the star's direction.
- [ ] `HDSystemInfo` lists bodies, stations, gates, sites. `HDGalaxy 10` lists neighbours.
- [ ] `HDGoto` to a Frontier system (`HDGalaxy 100`, pick a "Black" one): it has no gates in `HDSystemInfo`, only hidden wormholes/anomalies.
- [ ] `HDGas` in a system with a gas giant, then `HDWeek <n>` with a few values: densities change, roughly 3 in 10 Rare.
- [ ] Two PIE clients in different systems (`HDGoto` on one): neither sees the other's system objects. Both return fine to the start system.
- [ ] Wait 2+ minutes after leaving a system: log shows `Director: deactivated system`.
- [ ] Fly within 1,000 km of a belt (positions in `HDSystemInfo`): rocks stream in.

## 2. Travel (milestone 2)

**What's in:** `UHDNavComponent` on every ship (fuel, docking, landing, lanes, jumps, scanning) and travel modes in `UHDShipMovementComponent`. Everything runs on the server and is predicted on the owning client.

| Mode | How | Notes |
|---|---|---|
| Normal (sub space) | default | Thrusters. Gravity pulls near planets (below 50 km altitude). |
| Slipstream | J / tap Y | The M1 spool-and-engage, short in-system hops. Burns 0.01 t fuel/s. |
| Cruise | C / hold Y | Rate throttle (stick or W/S changes speed). The top speed falls inside gravity wells: 0.1 × altitude per second, min 1 km/s, max 20,000 km/s. Burns 0.02 t/s. Out of fuel drops you out. |
| Glide | automatic | Cruise below 25 km over a landable body with pitch between −5° and −60° and speed under 4 km/s → glide at 2.5 km/s. Below 3 km → Normal surface flight. Too steep or fast → **emergency drop** (250 damage). Too shallow, or outside the landing corridor → bounced back to 25 km+. Gas giants drop you to sub space at the cloud tops. Non-landable bodies → interdiction-style drop. |
| Trade lane | G at a ring | 10,000 km/s × lane integrity to the far ring. Any hit drops you out. Player lanes charge their toll (the toll never counts toward the Defense Budget). |
| Docked / Landed | G or H / L | Within 5 km of a station under 200 m/s; land below 1.5 km and under 100 m/s on bodies up to 2.0 g (authored up to 5 g). On foot is disabled above 2.7 g (interiors come later). |
| Jumping | G / B near a gate | 3 s gate spool, 10 s capital drive charge. |

**Landing corridors (developed worlds):** Administered worlds use four corridors that rotate every 3 minutes of UTC time (25° cones). Hardened worlds have one fixed corridor (12° cone). Undeveloped worlds are open everywhere. The HUD shows the active corridor, how far off you are and when it rotates.

**Jump rules:**
- Fighters through battleships need a **gate**, **trade lane** (in-system), **wormhole** or **stellar anomaly**. Capital-size hulls are too big for standard gates. They can use **large gates**, wormholes and anomalies, or their own drive.
- Wormholes and anomalies are hidden until scanned (K / hold X). Stellar anomalies have a 25% chance of throwing you to a random system within 45 ly. Galactic ruins are inert.
- **Instance jump lanes** take you to the site's instanced pocket (same system, far out).
- **Capital self-jumps:**

  | Hull | Range | Fuel |
  |---|---|---|
  | Capital | 3.5 ly | 30 t/ly |
  | Supercapital | 3 ly | 30 t/ly |
  | Jump freighter / capital industrial | 5 ly | 90 t/ly |

  Skill `jump_range` adds +20% per level. Skill `jump_fuel` cuts fuel 10% per level (up to 50%). Drive strain after a jump is `min(300, max(10·(1+ly), strain·(1+ly)))` minutes, decaying in real time. The spool-down is `min(30, max(strain/10, 1+ly))` minutes. Jump freighters and capital industrials count 10% of the distance for strain.
- **Fuel:** ship fuel (60 t on small hulls, 400 t on capitals) is bought at any station for 40 cr/t. Jump fuel is sold only at gas-giant fuel platforms, for 120 cr/t (`refuel`).

**Maps:** M / hold View opens the galaxy map, which shows nearby systems by band, visible gate links (large gates in white, wormholes in purple), your route and, on capitals, your jump-range ring. Select a system to plot a route: gate hops for small ships, drive hops within range for capitals. On capitals this also sets the jump target to the next hop. Gate ships get the next gate auto-targeted. Press M again for the system map, a top-down view with a list of everything visible; select an entry to target it. Press M a third time to close it. The HUD shows the route's next hop.

### Console (`HDCmd <verb> <arg>`, or the listed Exec commands)
| Command | Does |
|---|---|
| `HDMap` | Cycle the map. |
| `HDRoute NP-0042` | Plot a route without the map. |
| `HDCmd cruise` / `interact` / `dock` / `land` / `jump` / `lane` / `scan` | Same as the keys. |
| `HDCmd jumptarget NP-0042` | Set the capital jump target (empty clears it). |
| `HDCmd refuel 1` | Refuel to a fraction of capacity while docked. |
| `HDCmd hull Capital` | **Test:** switch hull class (Fighter … Battleship, Capital, Supercapital, JumpFreighter, CapitalIndustrial). Fills the tanks. |
| `HDCmd fuel` | **Test:** fill the tanks. |
| `HDCmd strain` / `HDCmd strain reset` | Show or clear drive strain. |
| `HDCmd skill jump_range 5` | **Test:** set a skill level. |
| `HDCmd navstatus` | Hull, fuel, range, strain, nearest body and altitude. |

Test commands are compiled out of Shipping builds.

### Checklist
- [ ] **Dock:** fly to the start station (T to target it, or the system map). Within 5 km, the prompt reads `Dock at …`. G docks you, the HUD says DOCKED and lists services, and the ship stops. G again undocks you 3 km out.
- [ ] **Cruise:** C, then W to build speed; the HUD SPEED reads km/s. Fly at a planet: the cap drops as you get closer. Fuel ticks down.
- [ ] **Landing:** cruise to a landable rocky body (`HDSystemInfo` shows `landable`). Nose down 20–40° and come in under 25 km. You get `Glide:` and then `Surface flight` below 3 km. Get under 1.5 km and slow below 100 m/s: the prompt reads `Land on …`, and L sets you down. L lifts off.
- [ ] Try it flat (0° pitch): you bounce back to 25 km. Try it at −80°: emergency drop and hull damage. Try a gas giant: you drop to sub space at the cloud tops.
- [ ] On a developed world (Secured systems mostly), approach outside the corridor: `Outside landing corridor`. Follow the HUD's `deg off` to find it.
- [ ] **Gate:** `HDSystemInfo` lists gates; target one, cruise there, drop out, G. Three seconds later you arrive at the far system's matching gate, and the HUD system line changes.
- [ ] **Capital:** `HDCmd hull Capital`, then the galaxy map; a green 3.5 ly ring appears. Select a system inside it and press G (or B): the drive charges for 10 s, then you jump. `HDCmd strain` shows ≥ 10·(1+ly) min and the spool-down. Jumping again before the spool-down ends is refused. `HDCmd hull Capital` at a standard gate is refused. `HDCmd skill jump_range 5` doubles the ring.
- [ ] **Scan:** in a Restricted or Frontier system, K a few times: `Scan: n resolved` and wormholes and sites appear (they're hidden before). Take a wormhole with G.
- [ ] **Trade lane:** in a Secured system with lanes, fly to a lane ring (system map: `Trade lane (Accord)`) and press G; you ride to the far ring.
- [ ] **Xbox:** hold Y cruise, tap Y slipstream, hold B interact, tap B flight assist, hold X scan, tap X equalize, hold View map; the D-pad moves the map cursor and A selects.

## 3. Ships and modules (milestone 3)

**What's in:** a data catalog (`UHDCatalogSubsystem`) loaded from `Content/Data/hulls.json`, `modules.json`, `items.json` and `recipes.json`. The files are generated by `tools/gen_catalog.py` and can be hand-edited, and there is a built-in fallback if they're missing. Each ship also carries a fitting component (`UHDFittingComponent`) and a hold (`UHDCargoComponent`).

- **Hulls (34):** fighter → corvette → frigate → destroyer → cruiser → battlecruiser → battleship → capital / supercapital / jump freighter / capital industrial. There is a mining line (Pick, Burrow, Quarry, Tunneler, Hearthforge) and a hauler line (Mule, Satchel, Drayman, Longhaul).
- **Supply tiers (doc 14):**
  - **Baseline:** every corvette and frigate, the basic cruiser line, and one line battlecruiser per faction. Sold bare by NPC stations; cruisers and up need a shipyard station.
  - **Faction:** one signature hull per faction with a quirk. These need standing 3 with that faction, can only be bought at its stations, and can't be sold.
  - **Advanced (player-built only):** destroyers and up, covert ops, industrial command, battleships, and all capitals, which are Tier 3. Build them with `HDCmd build` (M4).
- **Slots:** weapon, utility, support, engine and hold. Each hull has a maximum module size: 1 S, 2 M, 3 L, 4 Capital.
- **Power and stability:** every module draws MW from the hull's reactor (auxiliary reactors add MW) and uses up stability. A fit fails if either goes over its limit. Low remaining stability costs up to 30% turn rate.
- **Visible fit:** weapons render on dorsal hardpoints, utility modules as masts, support modules as side pods that widen the silhouette. A hold module replaces the rear section with a bulkier block, and engine nozzles follow the drive. Other pilots see all of this. The hull size scales with class, from a 12 m fighter to a 1.5 km supercapital.
- **Weapons:** all fitted guns fold into one hitscan battery fired with the fire button, with DPS summed, the longest range and the fastest interval. Mining lasers and salvagers fire on the same trigger at rocks and wrecks (M4).
- **Capitals need a `jump_drive` module to self-jump**, plus `jump_fuel_bay` modules for more fuel.
- **NPC stations sell only small Tier 1 modules** (`buymodule`), at a 25% markup as a soft floor. Everything else is built or salvaged.
- **Starter:** a Lance frigate (pulse laser, mining laser, scanner, shield extender, drive, ore hold). The first station you dock at gets a starter kit in storage.

### Console
| Command | Does |
|---|---|
| `HDCmd fitting` | Hull, stats and every slot with its index. |
| `HDCmd catalog hulls` / `catalog modules laser` / `catalog items Ore` | Browse the catalog (with an optional filter). |
| `HDCmd fit 2 autocannon_s` | Docked: fit from station storage (or the hold). The old module goes to storage. |
| `HDCmd unfit 2` | Docked: remove a module to storage. |
| `HDCmd storage` / `store <item> [n]` / `retrieve <item> [n]` | Station storage and hold transfers. |
| `HDCmd cargo` | Hold contents. |
| `HDCmd buymodule pulse_laser_s 2` | NPC Tier 1 S/M modules. |
| `HDCmd buyhull burrow` | Buy a bare baseline or faction hull into the hangar here. |
| `HDCmd hangar` / `board <n>` / `sellhull <n>` | Ships parked at stations. Swapping moves the hold to storage. |
| `HDCmd hull longhaul` / `hull Capital` | **Test:** become that hull with a suggested fit. |
| `HDCmd give <item or hull id> [n]` | **Test:** items to the hold, or to storage when docked. Hull ids go to the hangar, fitted. |
| `HDCmd credits 1000000` / `standing halvar_combine 5` / `reloadcatalog` | **Test helpers.** |

### Checklist
- [ ] On spawn the HUD shows `SHIP Lance power 44/100 MW stability 61/100`. From outside (a second PIE client), the Lance has two guns on top, a mast, a side pod and a bulky rear hold.
- [ ] Dock: `Starter kit delivered`. `HDCmd storage` lists it. `HDCmd fit 2 autocannon_s` works, and the HUD power goes up. `HDCmd give reactor_s`, then `HDCmd fit 4 reactor_s` is refused (slot 4 is a utility slot; `fitting` shows support slots 5–6, where `fit 6 reactor_s` works).
- [ ] Overfit power: `HDCmd give railgun_m 3` won't fit on a Lance (too large). `hull harrier`, then fit three railguns: power or stability refuses the last one.
- [ ] `HDCmd buyhull lance`, then `HDCmd board 0`: you're in a bare Lance (no drive, so it's slow). `board 0` again brings back the old ship.
- [ ] `HDCmd buyhull ironvale` at a Halvar station: refused without standing. After `HDCmd standing halvar_combine 5` it works, and `sellhull` on it is refused.
- [ ] `HDCmd hull monolith`: a huge ship, jump drive fitted, `HDCmd navstatus` shows 3.5 ly range. `HDCmd unfit` on the jump-drive slot while docked, then the galaxy map: no range ring, and jumping is refused.

## 4. Economy (milestone 4)

**What's in:** `UHDEconomySubsystem` (game instance, ticked by the system director), `AHDWreckActor`, mining / salvage / gas on the fire trigger, and the economy console verbs in `HDEconomyCommands.cpp`.

- **Mining (doc 16):** aim the mining laser at a rock within 3 km and fire. Belts need a stable orbit (ship speed under 50 m/s); ice needs a near-stationary hold (under 10 m/s) and cuts at 0.6×. Deep-core rocks yield 1.5×. Ore, ice and gas go to the ore hold first. Rocks mine out and vanish.
- **Regional consumption:** every unit mined counts against its region. Richness = 1 − mined / 40 000 (floor 0.2), recovering with a 12 h time constant. Belts stream in with `RocksPerField × richness` rocks, and mined-out rocks come back at about one per minute × richness.
- **Industrial command boost:** a ship with `command_burst_m/l` within 50 km boosts its corp-mates' mining and gas rates by 25% / 40%. It doesn't boost itself.
- **Gas:** a fitted `gas_siphon` works automatically while you're slow (<50 m/s) in normal flight, either inside a gas-cloud pocket (finite) or within 50 km of a gas giant's cloud tops (unlimited, × this week's density: Rare 0.4, Consistent 1, Abundant 1.8).
- **Fuel platforms (doc 19):** NPC gas-giant platforms regenerate stock with the week's density. Jump fuel costs 120 cr/t × (Rare 1.6, Consistent 1, Abundant 0.7). Ship fuel is 25 cr/t at platforms and 40 elsewhere. `buyfuel` loads jump fuel as cargo to haul; `loadfuel` moves it into a capital's jump tank.
- **NPC markets (doc 03):** prices derive from catalog reference prices.
  - Raw goods: NPCs buy at 60% everywhere and sell at 130% only at refineries and industrial stations.
  - Refined goods: bought at 70%, sold at 120%.
  - Components: bought at 75% (players make them).
  - Salvage: bought at 70%. Modules: bought back at 30%.
  - Trade goods: each station deterministically produces (cheap), consumes (expensive) or ignores each good.
  - Contraband: only at pirate stations. Pirate tags are bought at Accord stations (+Accord standing).
  - Pirate stations buy anything and sell goods marked stolen.
  - Stock drift moves prices between 0.6× and 1.8× and relaxes back over time. A lane cut in the system raises trade-good prices by 25%.
  - Sales tax is 2% (1% at Accord stations, 0 at pirate stations); the broker fee for placing an order is 1%.
- **Player orders:** placed at a station, visible region-wide (`regionorders`), filled only at that station. Sell orders escrow the goods; buy orders escrow the credits. `buy` / `sell` fill the best player orders first when they beat the NPC price. Sellers are paid the price minus tax; offline sellers are paid at their next login.
- **Industry:** `recipes` / `refine` / `build` run jobs at the right facility. Refinery recipes need `bRefinery`, shipyard recipes need a shipyard, and factory recipes need an industrial or corporate station (or a shipyard). Inputs come from storage here, then the hold. Outputs go to storage at that station; built hulls go to the hangar as bare ships.
- **Wrecks (doc 16):**
  - Every ship kill leaves a graded wreck. Grade follows overkill: a clean kill is G-A, a massive overkill is G-E. The wreck keeps the fit and 40–90% of the hold as loot.
  - Hulk sites stream in 1–3 class-matched hulks at G-B…G-D. Derelict stations and sentinel vaults hold ancient G-D/G-E derelicts.
  - Hulks need a breach flag allowed by their class: 1 Solo/Corp, 2 any, 3 Open/Corp, 4 Open only. The flag is broadcast within 12 AU and starts a collapse clock (Solo 40 min, Corp 2 h, Open 4 h). Open pays 2× materials and can drop relics; Corp pays 1.4×.
  - Each salvage pass (8 × size × (1 + 0.5 × class) work units) recovers modules (G-A 80–95%, B 60%, C 40%, D 20%, E 5%) and materials, then drops one grade. Untouched wrecks also drop a grade per 24 h.
  - Picked-clean hulk sites stay cleared until the weekly re-roll.
- **Rebuild:** tow a G-A…G-D wreck with a tractor beam (size limit 2 / 4 / 6 for S / M / L) to a shipyard and `rebuild` it. It costs 30 / 45 / 60 / 80% of the hull price, and the ship comes back to the hangar with a `recovered` tag and its surviving modules.

### Console
| Command | Does |
|---|---|
| `HDCmd market [filter]` | Docked: NPC buy/sell and best player orders per item. |
| `HDCmd buy <item> [qty] [maxprice]` / `sell <item> [qty\|all]` | Trade from the hold (overflow goes to storage). |
| `HDCmd order buy\|sell <item> <qty> <price>` / `orders` / `cancelorder <id>` | Player orders at this station. |
| `HDCmd regionorders <item>` | Orders for an item anywhere in your region. |
| `HDCmd recipes [filter]` / `refine <recipe> [runs]` / `build <recipe or output> [runs]` / `jobs` | Industry. |
| `HDCmd fuelprice` / `buyfuel <t>` / `loadfuel [t]` | Fuel platform prices, jump fuel as cargo, filling the jump tank. |
| `HDCmd salvageinfo` / `breach solo\|open\|corp` / `loot` / `tow` / `release` / `rebuild` | Wrecks (target one or be within 3 km). |
| `HDCmd spawnwreck [hull] [grade] [class]` | **Test:** a wreck 2 km ahead. |
| `HDCmd econ` / `mined <units>` | **Test:** regional richness readout / simulate heavy mining. |

### Checklist
- [ ] Fly into a belt, stop, aim at a rock and fire: `ore_*` fills the ore hold (`HDCmd cargo`). Moving faster than 50 m/s shows `Match the rock`. Mine a small rock out and it disappears.
- [ ] `HDCmd mined 30000`, then `econ`: richness ≈ 0.25. Leave the belt (stream-out) and come back: fewer rocks.
- [ ] Dock at a refinery or industrial station: `market ore` shows NPC sells and buys. `sell ore_ferrite all` pays about 60% of 12 cr each, minus 2% tax.
- [ ] `order sell ore_crystal 10 60`: the ore leaves the hold and `orders` lists it. From a second PIE client docked at the same station, `buy ore_crystal 10` fills it, and the first client gets `Market: +588 cr`.
- [ ] `cancelorder <id>` returns goods to storage, or credits for a buy order.
- [ ] `recipes refinery`, then `give ore_ferrite 100`, `store ore_ferrite 100`, `refine <recipe>`: `jobs` counts down, and the output lands in `storage`.
- [ ] `hull siphon`, then fly to within 50 km of a gas giant's cloud tops and slow down: `gas_nebular` accumulates. A Rare week is visibly slower.
- [ ] At a fuel platform: `fuelprice` changes with the gas giant's weekly density. `hull monolith`, `buyfuel 50`, `loadfuel` fills the jump tank.
- [ ] `spawnwreck lance 0`, then aim the salvager (`hull gleaner`) and fire: a pass completes with modules and salvage, and the grade drops to G-B. `loot` takes 5 alloy.
- [ ] `spawnwreck lance 1 1`: `salvageinfo` says unbreached (Solo/Corp allowed); `breach open` is refused; `breach solo` broadcasts. A second client is told someone else holds it.
- [ ] `give tractor_beam_s`, fit it to a utility slot, `spawnwreck lance 0`, `tow`, fly to a shipyard, dock, `rebuild`: the hangar shows a recovered Lance.
- [ ] Kill a ship (the target drone or a second client): a wreck appears where it died.

## 5. NPCs, antagonists, encounters and security (milestone 5)

**What's in:** `AHDNpcController` is a server-only AController that flies a normal `AHDShipPawn` through the same movement, laser and fitting components players use. `UHDNpcSubsystem` is a world subsystem ticked per active system by the director. Commands live in `HDNpcCommands.cpp`.

- **Populations (doc 18):**
  - Lawful: **Accord** security, **faction patrols** around industrial stations, **traders** that hop between stations carrying real cargo.
  - Antagonists:
    - **Rogues** prey on traders and small pilot hulls (they ignore anything over about 6,000 HP) and flee under 40% hull.
    - **Pirates** fly as a leader plus a wing. Wingmen fly formation and take the leader's target, and the wing breaks and flees when the leader dies.
    - **Sentinels** guard and never pursue or retreat.
    - **Shadows** stay hidden until a pilot comes within 15 km, then reveal and offer a **shadow contract** paid in standing and information.
- **NPC hardware ceiling:** NPC guns deal 0.8× (Accord 2.5×). NPC wrecks are never rebuildable. Pirates and rogues drop `pirate_tags`, which sell at Accord stations for standing. Sentinels may drop a `sentinel_core` (a researcher bounty).
- **Spawn pressure is economic (doc 18):** pressure = 0.5 + regional trade volume (credits traded in the region, 6 h decay) / 250k + 0.15 × pilots in system, clamped to 0.5–2.5. Targets per band:
  - Secured: 3 Accord, 3 traders, about 1×P rogues.
  - Restricted: 1 Accord, 2 patrols, 3 traders, 1.5×P rogues, and a pirate wing when P > 1.2.
  - Borderland: 1 patrol, 2 traders, 2×P rogues, 2×P+1 pirates.
  - Frontier: 1 trader, 2×P rogues, 3×P+1 pirates, 1 shadow.
  - The census runs every 15 s with at most one spawn per population per tick. The cap is 18 ambient NPCs per system. Ambient NPCs more than 2,000 km from every pilot are culled.
- **Encounter anomalies (doc 20), spawned when a pilot streams the site in:**
  - **Signal fire:** two damaged traders under attack by rogues (Secured/Restricted) or a pirate wing (Borderland+). Save at least one to get 3k × (1 + 2 × band) cr plus 1k per survivor and Accord standing.
  - **Contraband drift:** containers of contraband and stolen-marked goods with a fence. `loot` them (and risk scans) or `report` them for a smaller legal payout plus standing.
  - **Rogue nest:** 3 + 2 × band rogues that fight to the death. In Frontier, 25% field a capital-scale NPC (Monolith). A cleared nest pays out.
  - **Dead signature:** ancient buoys with `signal_data`, components and sometimes a **star chart**: 5% Secured/Restricted, 15% Borderland, 40% Frontier. Looting a chart resolves the system's hidden links (wormholes, ruins). Borderland+ buoys may hide a shadow ship.
  - Guarded sites: pirate bases (leader + 3 + band), sentinel vaults (2 + band sentinels, 50 km leash), deep-core guards, and the occasional rogue scavenger at Borderland+ hulks.
- **PVP is a property of place (doc 15):**
  - Secured: no PVP; shots do nothing ("Weapons safe") unless both pilots have `duel on`.
  - Restricted: only between corps at war (48 h notice, existing `AHDGameState::Wars`).
  - Borderland: at war, or inside a claimed volume (the territory hook arrives in M6).
  - Frontier: open.
  - Criminals are legal targets everywhere.
- **Crimes:**
  - Attacking lawful NPCs in Secured or Restricted space: criminal for 30 / 15 min and −0.5 Accord standing. The **Accord responds** in 8 s (3 Sentry cruisers) in Secured or 25 s (2) in Restricted, hunts you through the system, then leaves.
  - Looting another pilot's wreck in Secured or Restricted is theft (criminal for 15 min).
- **Contraband scans:** in Secured and Restricted space, being within 30 km of an Accord or patrol ship gets your hold scanned (every 5 min at most). Contraband and stolen-marked goods are seized with a 25% fine; if you can't pay, you're flagged criminal.
- **Standing:** kills adjust it. Rogues give +0.02 Accord, pirates +0.05 Accord and −0.2 with their faction; patrols cost −1 with their faction, traders −0.3 Accord, Accord ships −2.

### Console
| Command | Does |
|---|---|
| `HDCmd npcs` | NPC contacts within 500 km: label, faction, state, hostile / targeting you. |
| `HDCmd security` | Band rules, raider pressure, your criminal / duel status. |
| `HDCmd duel on\|off` | Opt-in duel flag. |
| `HDCmd report` | Report contraband containers within 10 km to the Accord. |
| `HDCmd shadow` / `shadow deliver` | Your shadow contract. |
| `HDCmd spawnnpc <pop> [hull\|-] [count]` | **Test:** spawn 15 km ahead (`accord patrol trader rogue pirate sentinel shadow`). |
| `HDCmd crime [s]` / `pardon` / `clearnpcs` | **Test:** trigger the Accord response, clear the flag, remove all NPCs. |

### Checklist
- [ ] Start in Secured and wait about 30 s: `npcs` lists Accord ships and traders near the station. Target one with T; the HUD shows `Accord security Harrier`.
- [ ] `spawnnpc rogue - 2`: they close in and fire. Damage one below 40% and it runs.
- [ ] `spawnnpc pirate - 4`: the wing follows the leader. Kill the leader and the rest flee. Their wrecks have `pirate_tags` (`loot`); `sell pirate_tags all` at an Accord station works.
- [ ] Shoot a trader in Secured: `CRIMINAL`, then `Accord security responding in 8 s`; three Sentries arrive and engage. `pardon` and `clearnpcs` reset.
- [ ] Two PIE clients in Secured: shots do nothing (`Weapons safe`). Both `duel on`: damage lands.
- [ ] Carry `give contraband_narcotics 5` near an Accord ship: within about 10 s it's seized and you're fined.
- [ ] Scan down a signal fire (`scan`) and fly in: traders are under attack. Kill the raiders and you get a payout plus Accord standing.
- [ ] A dead signature in Frontier (try a few): `loot` a buoy and sometimes get `Star chart: N hidden link(s) resolved`.
- [ ] `spawnnpc shadow`: invisible until you're within 15 km, then a contract offer. `shadow deliver` at the named site resolves hidden signatures.
- [ ] Spawn pressure: `security` shows the pressure rise after heavy trading in the region (`sell` large amounts).
