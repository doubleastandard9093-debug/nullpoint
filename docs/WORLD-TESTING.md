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
