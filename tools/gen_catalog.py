#!/usr/bin/env python3
"""Generates Content/Data/{hulls,modules,items,recipes}.json. Hand-edit the JSON or tweak and re-run this."""
import json, os
OUT = os.path.join(os.path.dirname(__file__), '..', 'Content', 'Data')

# class: speed m/s, accel m/s2, yaw deg/s, size m (L,W,H), hp, shield/facing, power MW, stability, cargo m3, slots (W,U,S,E,H), max module size, ship fuel t, jump fuel t
CLS = {
 'Fighter':       (450, 220, 90, (12, 8, 3),    300,   150,   40,   60,    2, (2,0,0,1,0), 1,  20,    0),
 'Corvette':      (380, 170, 70, (25, 12, 6),   600,   300,   70,   80,   20, (2,1,1,1,0), 1,  40,    0),
 'Frigate':       (300, 150, 45, (40, 18, 8),  1000,   500,  100,  100,   60, (3,2,2,1,1), 1,  60,    0),
 'Destroyer':     (260, 120, 35, (70, 26, 12), 1800,   900,  160,  140,  120, (5,2,3,1,1), 2,  90,    0),
 'Cruiser':       (220, 90,  25, (150, 50, 25),3500,  1600,  260,  200,  400, (5,3,4,1,1), 2, 150,    0),
 'Battlecruiser': (190, 70,  20, (250, 80, 40),6000,  2800,  380,  260,  600, (7,3,4,1,1), 3, 200,    0),
 'Battleship':    (160, 50,  15, (350,110, 60),10000, 4500,  520,  320,  800, (9,4,6,1,1), 3, 260,    0),
 'Capital':       (110, 25,   7, (800,250,140),40000,15000, 1500,  800, 3000, (10,5,8,1,1),4, 400, 2000),
 'Supercapital':  (90,  15,   5, (1500,450,250),90000,35000,3000, 1400, 6000, (14,6,10,1,1),4, 400, 3000),
 'JumpFreighter': (100, 20,   6, (600,180,120),25000, 8000,  900,  600,60000, (2,3,6,1,1), 4, 400, 4000),
 'CapitalIndustrial':(90,18,  6, (700,260,150),30000,12000, 1100,  700,40000, (4,5,6,1,1), 4, 400, 4000),
}
FACTIONS = ['halvar_combine', 'tessaly_freehold', 'meridian_compact', 'orrin_union']

def hull(id, name, cls, role, supply, tier, price, faction='', quirk='', **mods):
    sp, ac, yaw, size, hp, sh, pw, st, cargo, slots, maxs, fuel, jfuel = CLS[cls]
    h = dict(id=id, name=name, cls=cls, role=role, supply=supply, tier=tier, faction=faction, price=price, quirk=quirk,
             speed=sp, accel=ac, turn=yaw, size=list(size), hp=hp, shield=sh, power=pw, stability=st, cargo=cargo,
             slots=dict(weapon=slots[0], utility=slots[1], support=slots[2], engine=slots[3], hold=slots[4]), maxModuleSize=maxs,
             shipFuel=fuel, jumpFuel=jfuel, buildable=supply != 'Baseline' or True, color=[0.55, 0.58, 0.62])
    for k, v in mods.items():
        if k == 'slots': h['slots'].update(v)
        else: h[k] = v
    return h

H = []
# ---- Baseline: NPC-sold bare hulls (every corvette + frigate, basic cruiser line, one line battlecruiser per faction) ----
H += [
 hull('wasp', 'Wasp', 'Fighter', 'Combat', 'Baseline', 1, 9000),
 hull('skiff', 'Skiff', 'Corvette', 'Combat', 'Baseline', 1, 18000),
 hull('pick', 'Pick', 'Corvette', 'Miner', 'Baseline', 1, 16000, mining=1.1),
 hull('mule', 'Mule', 'Corvette', 'Hauler', 'Baseline', 1, 15000, cargo=120),
 hull('lance', 'Lance', 'Frigate', 'Combat', 'Baseline', 1, 40000),
 hull('burrow', 'Burrow', 'Frigate', 'Miner', 'Baseline', 1, 38000, mining=1.2, cargo=150),
 hull('satchel', 'Satchel', 'Frigate', 'Hauler', 'Baseline', 1, 36000, cargo=500, slots=dict(weapon=1)),
 hull('gleaner', 'Gleaner', 'Frigate', 'Salvager', 'Baseline', 1, 42000, salvage=1.25),
 hull('lookout', 'Lookout', 'Frigate', 'Surveyor', 'Baseline', 1, 45000, scan=1.5),
 hull('quarry', 'Quarry', 'Cruiser', 'Miner', 'Baseline', 1, 220000, mining=1.3, cargo=1200),
 hull('drayman', 'Drayman', 'Cruiser', 'Hauler', 'Baseline', 1, 200000, cargo=6000, slots=dict(weapon=2)),
 hull('siphon', 'Siphon', 'Cruiser', 'Extractor', 'Baseline', 1, 210000, gas=1.5, cargo=1500),
 hull('cartographer', 'Cartographer', 'Cruiser', 'Surveyor', 'Baseline', 1, 240000, scan=2.0),
 hull('bulwark', 'Bulwark', 'Battlecruiser', 'Combat', 'Baseline', 2, 900000, faction='halvar_combine'),
 hull('outrider', 'Outrider', 'Battlecruiser', 'Combat', 'Baseline', 2, 880000, faction='tessaly_freehold'),
 hull('concord', 'Concord', 'Battlecruiser', 'Combat', 'Baseline', 2, 920000, faction='meridian_compact'),
 hull('hammerline', 'Hammerline', 'Battlecruiser', 'Combat', 'Baseline', 2, 870000, faction='orrin_union'),
]
# ---- Faction-specific: standing-gated, never on the open market; one strong quirk, never best-in-class ----
H += [
 hull('ironvale', 'Ironvale', 'Destroyer', 'Combat', 'Faction', 2, 260000, faction='halvar_combine', quirk='+40% hull, -15% speed', hp=2520, speed=220),
 hull('wildcard', 'Wildcard', 'Cruiser', 'Combat', 'Faction', 2, 520000, faction='tessaly_freehold', quirk='+30% speed and turn, -25% shields', speed=286, turn=33, shield=1200),
 hull('verdict', 'Verdict', 'Cruiser', 'Combat', 'Faction', 2, 560000, faction='meridian_compact', quirk='+35% shields, one fewer weapon', shield=2160, slots=dict(weapon=4)),
 hull('tunneler', 'Tunneler', 'Cruiser', 'Miner', 'Faction', 2, 480000, faction='orrin_union', quirk='+50% mining yield, no hold upgrades', mining=1.95, cargo=1600),
]
# ---- Advanced & specialist: player-built only ----
H += [
 hull('harrier', 'Harrier', 'Destroyer', 'Combat', 'Advanced', 2, 300000),
 hull('snare', 'Snare', 'Destroyer', 'Interdictor', 'Advanced', 2, 340000),
 hull('whisper', 'Whisper', 'Frigate', 'CovertOps', 'Advanced', 2, 180000, scan=1.4),
 hull('sentry', 'Sentry', 'Cruiser', 'Combat', 'Advanced', 2, 600000),
 hull('reclaimer', 'Reclaimer', 'Cruiser', 'Salvager', 'Advanced', 2, 450000, salvage=1.6, cargo=1500),
 hull('pathfinder', 'Pathfinder', 'Cruiser', 'Explorer', 'Advanced', 2, 500000, scan=2.5, shipFuel=300),
 hull('foreman', 'Foreman', 'Battlecruiser', 'IndustrialCommand', 'Advanced', 2, 1400000, cargo=2500),
 hull('citadel', 'Citadel', 'Battleship', 'Combat', 'Advanced', 3, 2500000),
 hull('bastion', 'Bastion', 'Capital', 'Carrier', 'Advanced', 3, 18000000),
 hull('monolith', 'Monolith', 'Capital', 'Dreadnought', 'Advanced', 3, 20000000),
 hull('sovereign', 'Sovereign', 'Supercapital', 'Carrier', 'Advanced', 3, 90000000),
 hull('longhaul', 'Longhaul', 'JumpFreighter', 'Freighter', 'Advanced', 3, 14000000),
 hull('hearthforge', 'Hearthforge', 'CapitalIndustrial', 'IndustrialCommand', 'Advanced', 3, 16000000, mining=1.5),
]

def mod(id, name, slot, size, tier, power, stability, price, shape, scale, color, **fx):
    return dict(id=id, name=name, slot=slot, size=size, tier=tier, power=power, stability=stability, price=price,
                visual=dict(shape=shape, scale=scale, color=color), effects=fx)

M = []
SZ = {1: 'S', 2: 'M', 3: 'L', 4: 'Capital'}
for s in (1, 2, 3, 4):
    k = [1, 2.4, 5, 14][s - 1]
    t = 1 if s <= 2 else (2 if s == 3 else 3)
    M.append(mod(f'pulse_laser_{SZ[s].lower()}', f'Pulse Laser {SZ[s]}', 'Weapon', s, t, round(8 * k), round(6 * k), round(8000 * k * k), 'Cylinder', [0.4 * k, 0.4 * k, 2.0 * k], [1, 0.4, 0.3], damage=round(40 * k), range=5000 * (1 + 0.25 * (s - 1)), interval=0.15))
    M.append(mod(f'autocannon_{SZ[s].lower()}', f'Autocannon {SZ[s]}', 'Weapon', s, t, round(5 * k), round(8 * k), round(7000 * k * k), 'Cylinder', [0.5 * k, 0.5 * k, 1.6 * k], [0.7, 0.7, 0.6], damage=round(22 * k), range=3500 * (1 + 0.25 * (s - 1)), interval=0.08))
    M.append(mod(f'railgun_{SZ[s].lower()}', f'Railgun {SZ[s]}', 'Weapon', s, t, round(14 * k), round(10 * k), round(12000 * k * k), 'Cylinder', [0.35 * k, 0.35 * k, 3.2 * k], [0.4, 0.6, 1], damage=round(160 * k), range=12000 * (1 + 0.25 * (s - 1)), interval=0.9))
    M.append(mod(f'mining_laser_{SZ[s].lower()}', f'Mining Laser {SZ[s]}', 'Weapon', s, t, round(6 * k), round(5 * k), round(6000 * k * k), 'Cone', [0.5 * k, 0.5 * k, 1.4 * k], [0.3, 1, 0.5], mining=round(2 * k, 1), range=3000, interval=0.25))
    M.append(mod(f'salvager_{SZ[s].lower()}', f'Salvager {SZ[s]}', 'Weapon', s, t, round(5 * k), round(4 * k), round(6500 * k * k), 'Cone', [0.5 * k, 0.5 * k, 1.2 * k], [1, 0.8, 0.3], salvage=round(1 * k, 1), range=2500, interval=0.5))
    M.append(mod(f'gas_siphon_{SZ[s].lower()}', f'Gas Siphon {SZ[s]}', 'Utility', s, t, round(5 * k), round(5 * k), round(6000 * k * k), 'Cylinder', [0.6 * k, 0.6 * k, 2 * k], [0.6, 0.9, 1], gas=round(1.5 * k, 1)))
    M.append(mod(f'shield_extender_{SZ[s].lower()}', f'Shield Extender {SZ[s]}', 'Support', s, t, round(10 * k), round(8 * k), round(9000 * k * k), 'Sphere', [1.2 * k, 1.2 * k, 1.2 * k], [0.3, 0.6, 1], shield=round(250 * k)))
    M.append(mod(f'armor_plate_{SZ[s].lower()}', f'Armor Plate {SZ[s]}', 'Support', s, t, 0, round(14 * k), round(7000 * k * k), 'Cube', [2 * k, 0.4 * k, 1 * k], [0.45, 0.45, 0.45], hull=round(400 * k), speed=-0.04))
    M.append(mod(f'cargo_expander_{SZ[s].lower()}', f'Cargo Expander {SZ[s]}', 'Support', s, t, 2, round(6 * k), round(5000 * k * k), 'Cube', [1.8 * k, 1 * k, 1 * k], [0.7, 0.6, 0.4], cargo=round(60 * k * k), speed=-0.03))
    M.append(mod(f'reactor_{SZ[s].lower()}', f'Auxiliary Reactor {SZ[s]}', 'Support', s, t, 0, round(10 * k), round(11000 * k * k), 'Cylinder', [1.2 * k, 1.2 * k, 1.2 * k], [1, 0.9, 0.4], powerOut=round(30 * k)))
    M.append(mod(f'fuel_tank_{SZ[s].lower()}', f'Fuel Tank {SZ[s]}', 'Support', s, t, 1, round(4 * k), round(4000 * k * k), 'Cylinder', [1 * k, 1 * k, 2 * k], [0.9, 0.5, 0.2], shipFuel=round(30 * k)))
    M.append(mod(f'drive_{SZ[s].lower()}', f'Drive {SZ[s]}', 'Engine', s, t, round(12 * k), round(4 * k), round(9000 * k * k), 'Cone', [1.2 * k, 1.2 * k, 1.6 * k], [0.5, 0.8, 1], speed=0.0))
    M.append(mod(f'sprint_drive_{SZ[s].lower()}', f'Sprint Drive {SZ[s]}', 'Engine', s, t, round(20 * k), round(10 * k), round(16000 * k * k), 'Cone', [1.4 * k, 1.4 * k, 2.2 * k], [1, 0.5, 0.9], speed=0.25, turn=0.1))
    M.append(mod(f'ore_hold_{SZ[s].lower()}', f'Ore Hold {SZ[s]}', 'Hold', s, t, 2, round(12 * k), round(8000 * k * k), 'Cube', [1, 1, 1], [0.6, 0.5, 0.35], cargo=round(150 * k * k), oreOnly=True))
    M.append(mod(f'freight_hold_{SZ[s].lower()}', f'Freight Hold {SZ[s]}', 'Hold', s, t, 2, round(14 * k), round(9000 * k * k), 'Cube', [1, 1, 1], [0.5, 0.55, 0.6], cargo=round(120 * k * k), speed=-0.05))
    M.append(mod(f'salvage_bay_{SZ[s].lower()}', f'Salvage Bay {SZ[s]}', 'Hold', s, t, 3, round(12 * k), round(9000 * k * k), 'Cube', [1, 1, 1], [0.8, 0.6, 0.2], cargo=round(80 * k * k), salvage=0.3))
for s in (1, 2, 3):
    k = [1, 2.4, 5][s - 1]
    M.append(mod(f'scanner_array_{SZ[s].lower()}', f'Scanner Array {SZ[s]}', 'Utility', s, s, round(6 * k), round(4 * k), round(10000 * k * k), 'Cylinder', [0.3 * k, 0.3 * k, 3 * k], [0.8, 0.8, 1], scan=0.5 * s))
    M.append(mod(f'tractor_beam_{SZ[s].lower()}', f'Tractor Beam {SZ[s]}', 'Utility', s, s, round(8 * k), round(4 * k), round(9000 * k * k), 'Sphere', [0.8 * k, 0.8 * k, 0.8 * k], [0.4, 1, 0.9], tow=s))
M += [
 mod('interdiction_field', 'Interdiction Field', 'Utility', 2, 2, 40, 30, 180000, 'Sphere', [3, 3, 3], [1, 0.2, 0.2], interdict=1),
 mod('cloak_s', 'Cloaking Device', 'Utility', 1, 2, 25, 10, 220000, 'Sphere', [0.8, 0.8, 0.8], [0.3, 0.3, 0.4], cloak=1),
 mod('command_burst_m', 'Mining Command Burst', 'Utility', 2, 2, 30, 20, 260000, 'Cylinder', [0.6, 0.6, 5], [1, 1, 0.3], command=0.25),
 mod('command_burst_l', 'Mining Command Burst L', 'Utility', 3, 3, 60, 40, 900000, 'Cylinder', [1, 1, 9], [1, 1, 0.3], command=0.4),
 mod('jump_drive', 'Jump Drive', 'Support', 4, 3, 300, 120, 4000000, 'Cylinder', [12, 12, 20], [0.7, 0.4, 1], jumpDrive=True),
 mod('jump_fuel_bay', 'Jump Fuel Bay', 'Support', 4, 3, 10, 60, 600000, 'Cylinder', [10, 10, 16], [0.9, 0.5, 0.2], jumpFuel=1500),
 mod('fighter_hangar', 'Fighter Hangar', 'Support', 4, 3, 120, 150, 3000000, 'Cube', [40, 18, 10], [0.5, 0.5, 0.55], fighters=6),
 mod('siege_cannon', 'Siege Cannon', 'Weapon', 4, 3, 400, 200, 6000000, 'Cylinder', [10, 10, 70], [1, 0.3, 0.2], damage=6000, range=40000, interval=3.0),
]

ITEMS = []
def item(id, name, cat, vol, price, legal=True, **kw):
    d = dict(id=id, name=name, category=cat, volume=vol, price=price, legal=legal); d.update(kw); ITEMS.append(d)
for i in [('ore_ferrite', 'Ferrite Ore', 0.1, 12), ('ore_silicate', 'Silicate Ore', 0.1, 10), ('ore_crystal', 'Crystal Ore', 0.15, 45), ('ore_exotic', 'Exotic Ore', 0.2, 320)]:
    item(i[0], i[1], 'Ore', i[2], i[3])
for i in [('ice_water', 'Water Ice', 0.2, 18), ('ice_volatile', 'Volatile Ice', 0.2, 60)]: item(i[0], i[1], 'Ice', i[2], i[3])
for i in [('gas_nebular', 'Nebular Gas', 0.25, 30), ('gas_volatile', 'Volatile Gas', 0.25, 90)]: item(i[0], i[1], 'Gas', i[2], i[3])
for i in [('ferrite_ingot', 'Ferrite Ingot', 0.05, 60), ('silicon_wafer', 'Silicon Wafer', 0.05, 55), ('crystal_lattice', 'Crystal Lattice', 0.05, 240),
          ('exotic_matter', 'Exotic Matter', 0.05, 1800), ('water', 'Water', 0.1, 40), ('volatiles', 'Refined Volatiles', 0.1, 160), ('jump_fuel', 'Jump Fuel', 0.1, 120)]:
    item(i[0], i[1], 'Refined', i[2], i[3])
for i in [('structural_frame', 'Structural Frame', 1, 900), ('power_cell', 'Power Cell', 0.5, 1400), ('circuit_board', 'Circuit Board', 0.2, 1200),
          ('focusing_lens', 'Focusing Lens', 0.2, 1600), ('drive_coil', 'Drive Coil', 1, 2200), ('shield_emitter', 'Shield Emitter', 0.5, 2000),
          ('sensor_array', 'Sensor Array', 0.5, 1800), ('jump_core', 'Jump Core', 20, 250000), ('habitat_block', 'Habitat Block', 50, 30000), ('dock_section', 'Dock Section', 80, 45000)]:
    item(i[0], i[1], 'Component', i[2], i[3])
for i in [('salvage_scrap', 'Salvaged Scrap', 0.5, 40), ('salvage_alloy', 'Salvaged Alloy', 0.5, 220), ('salvage_electronics', 'Salvaged Electronics', 0.2, 600),
          ('ancient_relic', 'Ancient Relic', 1, 15000)]:
    item(i[0], i[1], 'Salvage', i[2], i[3])
for i in [('food_rations', 'Food Rations', 0.5, 50), ('medical_supplies', 'Medical Supplies', 0.3, 300), ('consumer_goods', 'Consumer Goods', 0.5, 180),
          ('machinery', 'Machinery', 2, 900), ('luxury_goods', 'Luxury Goods', 0.3, 1500), ('colonists', 'Colonist Pods', 2, 500)]:
    item(i[0], i[1], 'Trade', i[2], i[3])
item('contraband_narcotics', 'Glimmer Dust', 'Contraband', 0.2, 2500, legal=False)
item('contraband_weapons', 'Unregistered Arms', 'Contraband', 1, 4000, legal=False)
item('pirate_tags', 'Pirate Ident Tags', 'Bounty', 0.01, 1500)
item('sentinel_core', 'Sentinel Core', 'Bounty', 0.5, 12000)
item('star_chart', 'Star Chart Fragment', 'Component', 0.01, 4000)
item('signal_data', 'Signal Data Cache', 'Component', 0.05, 900)
item('munitions', 'Munitions', 'Component', 0.1, 90)
# Structure kits (doc 19): anchored from the hold. Everything traces back to mining.
KITS = [
    ('kit_beacon', 'Claim Beacon Kit', 40, 400000, {'structural_frame': 40, 'power_cell': 20, 'sensor_array': 10, 'circuit_board': 20}, 600),
    ('kit_gun', 'Gun Placement Kit', 8, 60000, {'structural_frame': 8, 'focusing_lens': 6, 'power_cell': 4}, 120),
    ('kit_platform', 'Defense Platform Kit', 25, 220000, {'structural_frame': 25, 'shield_emitter': 10, 'focusing_lens': 12, 'power_cell': 12}, 300),
    ('kit_habitat', 'Habitat Ring Kit', 60, 180000, {'habitat_block': 3, 'structural_frame': 20, 'power_cell': 10}, 300),
    ('kit_refinery', 'Orbital Refinery Kit', 50, 260000, {'structural_frame': 40, 'power_cell': 20, 'drive_coil': 10}, 360),
    ('kit_factory', 'Orbital Factory Kit', 50, 300000, {'structural_frame': 40, 'circuit_board': 30, 'power_cell': 20}, 360),
    ('kit_fuel_platform', 'Gas Giant Fuel Platform Kit', 60, 350000, {'structural_frame': 50, 'drive_coil': 20, 'power_cell': 20, 'sensor_array': 6}, 420),
    ('kit_station', 'Station Core Kit', 120, 900000, {'dock_section': 4, 'habitat_block': 4, 'structural_frame': 80, 'power_cell': 40}, 900),
    ('kit_section', 'Station Section Kit', 30, 120000, {'structural_frame': 25, 'power_cell': 10, 'circuit_board': 10}, 180),
    ('kit_interdictor', 'Lane Interdictor Kit', 20, 160000, {'structural_frame': 15, 'drive_coil': 15, 'sensor_array': 8}, 240),
    ('kit_lane', 'Trade Lane Ring Pair Kit', 100, 1200000, {'structural_frame': 120, 'drive_coil': 60, 'power_cell': 60, 'sensor_array': 20}, 900),
    ('kit_colony', 'Colony Seed Kit', 80, 500000, {'habitat_block': 6, 'structural_frame': 40, 'food_rations': 200, 'medical_supplies': 50}, 600),
]
for k in KITS: item(k[0], k[1], 'Component', k[2], k[3])
for m in M:
    item(m['id'], m['name'], 'Module', 2.0 * m['size'] ** 2, m['price'], tier=m['tier'])
for h in H:
    item('hull_' + h['id'], h['name'] + ' (packaged hull)', 'Hull', h['size'][0] * 2.0, h['price'])

# Recipes: refine (refinery), fabricate (any station with a factory = shipyard or refinery), build hulls (shipyard)
R = []
def rec(id, out, qty, inputs, secs, facility): R.append(dict(id=id, output=out, qty=qty, inputs=inputs, seconds=secs, facility=facility))
rec('refine_ferrite', 'ferrite_ingot', 10, {'ore_ferrite': 30}, 10, 'refinery')
rec('refine_silicate', 'silicon_wafer', 10, {'ore_silicate': 30}, 10, 'refinery')
rec('refine_crystal', 'crystal_lattice', 10, {'ore_crystal': 30}, 15, 'refinery')
rec('refine_exotic', 'exotic_matter', 5, {'ore_exotic': 30}, 30, 'refinery')
rec('melt_ice', 'water', 10, {'ice_water': 20}, 8, 'refinery')
rec('crack_volatile_ice', 'volatiles', 10, {'ice_volatile': 20}, 12, 'refinery')
rec('crack_gas', 'volatiles', 10, {'gas_volatile': 25}, 12, 'refinery')
rec('jump_fuel', 'jump_fuel', 20, {'gas_nebular': 25}, 10, 'refinery')
rec('munitions', 'munitions', 50, {'ferrite_ingot': 10, 'volatiles': 5}, 10, 'factory')
for k in KITS: rec(k[0], k[0], 1, k[4], k[5], 'factory')
rec('recycle_scrap', 'ferrite_ingot', 6, {'salvage_scrap': 10}, 6, 'refinery')
rec('recycle_alloy', 'ferrite_ingot', 10, {'salvage_alloy': 5}, 6, 'refinery')
rec('structural_frame', 'structural_frame', 1, {'ferrite_ingot': 12}, 10, 'factory')
rec('power_cell', 'power_cell', 1, {'crystal_lattice': 4, 'volatiles': 2}, 10, 'factory')
rec('circuit_board', 'circuit_board', 1, {'silicon_wafer': 8, 'ferrite_ingot': 2}, 10, 'factory')
rec('circuit_from_salvage', 'circuit_board', 1, {'salvage_electronics': 2}, 6, 'factory')
rec('focusing_lens', 'focusing_lens', 1, {'crystal_lattice': 6}, 10, 'factory')
rec('drive_coil', 'drive_coil', 1, {'ferrite_ingot': 10, 'crystal_lattice': 2, 'volatiles': 2}, 12, 'factory')
rec('shield_emitter', 'shield_emitter', 1, {'crystal_lattice': 4, 'circuit_board': 1}, 12, 'factory')
rec('sensor_array', 'sensor_array', 1, {'silicon_wafer': 6, 'crystal_lattice': 2}, 12, 'factory')
rec('jump_core', 'jump_core', 1, {'exotic_matter': 40, 'drive_coil': 20, 'power_cell': 20}, 120, 'shipyard')
rec('habitat_block', 'habitat_block', 1, {'structural_frame': 20, 'water': 50, 'circuit_board': 4}, 60, 'shipyard')
rec('dock_section', 'dock_section', 1, {'structural_frame': 40, 'power_cell': 6}, 60, 'shipyard')
KEY = {'Weapon': 'focusing_lens', 'Utility': 'sensor_array', 'Support': 'shield_emitter', 'Engine': 'drive_coil', 'Hold': 'structural_frame'}
for m in M:
    n = [1, 3, 8, 30][m['size'] - 1]
    ins = {'structural_frame': n, 'circuit_board': max(1, n // 2), KEY[m['slot']]: n}
    if m['size'] == 4: ins['exotic_matter'] = 10
    if m['effects'].get('jumpDrive'): ins['jump_core'] = 1
    rec('build_' + m['id'], m['id'], 1, ins, 10 * m['size'], 'shipyard' if m['size'] >= 3 else 'factory')
for h in H:
    if h['supply'] == 'Faction': continue  # faction hulls: bought with standing or salvaged, never fabricated
    frames = max(4, h['size'][0] // 3)
    ins = {'structural_frame': frames, 'circuit_board': frames // 3 + 1, 'power_cell': frames // 4 + 1, 'drive_coil': frames // 6 + 1}
    if h['tier'] == 3: ins.update({'exotic_matter': frames // 4, 'shield_emitter': frames // 5})
    if h['cls'] in ('Capital', 'Supercapital', 'JumpFreighter', 'CapitalIndustrial'): ins['jump_core'] = 1 if h['cls'] != 'Supercapital' else 2
    rec('build_hull_' + h['id'], 'hull_' + h['id'], 1, ins, 30 + frames, 'shipyard')

STARTER = dict(hull='lance', modules=['pulse_laser_s', 'mining_laser_s', '', 'scanner_array_s', '', 'shield_extender_s', '', 'drive_s', 'ore_hold_s'],
               storage={'pulse_laser_s': 1, 'autocannon_s': 1, 'cargo_expander_s': 1, 'salvager_s': 1, 'armor_plate_s': 1})
os.makedirs(OUT, exist_ok=True)
for name, data in [('hulls', dict(hulls=H, starter=STARTER)), ('modules', dict(modules=M)), ('items', dict(items=ITEMS)), ('recipes', dict(recipes=R))]:
    with open(os.path.join(OUT, name + '.json'), 'w') as f:
        json.dump(data, f, indent=1)
print(len(H), 'hulls', len(M), 'modules', len(ITEMS), 'items', len(R), 'recipes')
