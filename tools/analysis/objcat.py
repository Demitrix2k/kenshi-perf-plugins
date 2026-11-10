#!/usr/bin/env python3
"""Group a PerfProbe full object list into categories (name heuristics).
Usage: objcat.py objs.txt   (rows after a "(full list)" line of PerfProbe.log)"""
import re, sys, collections
CAT = [
    ('lights',      r'^\[DeferredLightRenderable|strip_light|Turret Light|unit_sphere|lamp'),
    ('terrain',     r'^\[Terrain|^\[QuadRenderable@TerrainDecals|distant_|ScreeStones|zoneWater|waterPlane|SkyX'),
    ('foliage',     r'^\[SubBatch@BatchedGeometry@Forests|^GrassLDR'),
    ('instanced',   r'^\[InstanceBatch'),
    ('fx/2d',       r'^\[BillboardSet|^\[PointsRenderable|^\[Rectangle2D|^\[ManualObject'),
    ('characters',  r'human_|bone_(male|female)|_morph|quack|gamedata\.base|chareditor|Manskin|Skeleton_|Robot_Variant|goat|'
                    r'[Hh]aircut|beard|Beard|boots|pants|Pants|shirt|Shirt|Dress|dress|coat|Coat|armou?r|Armou?r|Plate|plate_m|'
                    r'cloth\b|_cloth|Jacket|jacket|[Hh]at|helmet|Helmet|mask|Mask|headgear|Glasses|backpack|sandals|bodyLow|BodyTop|'
                    r'Barbarian|Drifter|Assas|Police|hacker|heavy0|ROBOT-ARM|Robotic_|Stump|Tough Leg|Coins on a String|Iron Clad|RagArmoured|Monk|'
                    r'katana|polearm|club0|sabre|nodachi|blade0|Crossbow|jitte|Hoe\.mesh|_arm\.mesh|_leg\.mesh|Bench clothing'),
    ('buildings/props', r'.'),
]
rows = []
for l in open(sys.argv[1]):
    m = re.match(r'(.{48})\s*([\d.]+)%\s+([\d.]+)%\s+(\d+)\s+(\d+)', l.rstrip('\n'))
    if m: rows.append((m.group(1).strip(), float(m.group(2)), float(m.group(3)), int(m.group(4)), int(m.group(5))))
agg = collections.OrderedDict((c, [0, 0.0, 0.0, 0, 0]) for c, _ in CAT)
for name, pm, ps, dm, ds in rows:
    for c, rx in CAT:
        if re.search(rx, name):
            a = agg[c]; a[0] += 1; a[1] += pm; a[2] += ps; a[3] += dm; a[4] += ds; break
td = sum(a[3] + a[4] for a in agg.values())
print(f'{"category":16} {"types":>5} {"main %":>7} {"shadow %":>8} {"draws/s main":>12} {"draws/s shadow":>14} {"share of draws":>14}')
for c, a in agg.items():
    print(f'{c:16} {a[0]:5} {a[1]:7.1f} {a[2]:8.1f} {a[3]:12} {a[4]:14} {100*(a[3]+a[4])/td:13.1f}%')
print(f'total draws/s {td}')
