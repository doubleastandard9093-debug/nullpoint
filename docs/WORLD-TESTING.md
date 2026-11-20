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
