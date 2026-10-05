#!/usr/bin/env python3
"""Refresh the FoC space-skirmish roster and its mechanic evidence (UC-01..06)."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import sys
import xml.etree.ElementTree as ET
from collections import Counter, defaultdict, deque
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
from tools.inventory.corpus import Corpus, CorpusError


def tag_family(tag):
    """Split broad coverage tickets into mechanics a validation matrix can target."""
    patterns = (
        ("ability parameters", r"^Abilities/|^Unit_Abilities_Data/"),
        ("production limits and prerequisites", r"Build_Limit|Prerequisite|Initially_Locked"),
        ("station and mine upgrades", r"Upgrade|Next_Level_Base|Enable_Special_Weapon"),
        ("income streams", r"Income|Interval_In_Secs"),
        ("production and reinforcement", r"Tactical_Build|Production|Population|Reinforcement|Transport"),
        ("carrier spawning and team lifecycle", r"Spawn|Squadron|Team|Garrison"),
        ("weapon damage and armor", r"Damage|Armor|Health|Diminishing_Firepower"),
        ("projectile collision and guidance", r"Projectile|Collision|Shield_Penetration|Detonation"),
        ("weapon firing and hardpoints", r"Fire_|Weapon|Hardpoint|Muzzle"),
        ("target acquisition and orders", r"Target|Chase|Guard|Attack_Move|Formation"),
        ("shield and energy pools", r"Shield|Energy|Stun"),
        ("repairs", r"Repair"),
        ("locomotion and spatial placement", r"Speed|Thrust|Accel|Decel|Braking|Turn|Rate_Of|Bank|Layer|Footprint|Obstacle"),
        ("sensors and concealment", r"Reveal|Fog|Stealth|Cloak|Radar"),
        ("capture and construction", r"Capture|Under_Construction|Sell|Respawn|Child"),
        ("hazard interaction", r"Nebula|Asteroid"),
        ("identity and lifecycle", r"Hero|Victory|Death|Destroy|Behavior|SpaceBehavior"),
    )
    return next((title for title, pattern in patterns if re.search(pattern, tag, re.I)), "other runtime inputs")
