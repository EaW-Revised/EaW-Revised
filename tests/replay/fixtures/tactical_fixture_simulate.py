"""Independent tactical fixture simulate helpers; no production simulation imports."""
from __future__ import annotations
from dataclasses import dataclass, field, replace
from decimal import ROUND_HALF_EVEN, Decimal, getcontext
import argparse
import hashlib
import json
from pathlib import Path
import struct
import sys

from tactical_fixture_types import (
    OUT,
    NAME,
    MAGIC,
    STATE_MAGIC,
    SNAPSHOT_MAGIC,
    FORMAT_VERSION,
    HEADER_SIZE,
    RULES_VERSION,
    STATE_ENCODING_VERSION,
    SNAPSHOT_ENCODING_VERSION,
    MATH_VERSION,
    FRACTIONAL_BITS,
    ONE,
    TICK_NUMERATOR,
    TICK_DENOMINATOR,
    FINAL_TICK_COUNT,
    SEED,
    CONTENT_IDENTITY,
    COMMANDABLE,
    OPCODES,
    ORDER_KINDS,
    EVENT_KINDS,
    REASONS,
    HULL_TARGET,
    Player,
    Order,
    Unit,
    Command,
    q24,
    vec,
    rne_shift,
    decimal_pi,
    decimal_sin_cos,
    yaw_quat,
    to_matrix,
    VISIBILITY,
    VISIBILITY_SEED,
    VISIBILITY_CONTENT,
    VISIBILITY_SENSORS,
    FOG_LAYOUT,
    TIE,
    FRIGATE,
    DURABILITY,
    DURABILITY_SEED,
    DURABILITY_CONTENT,
    DURABILITY_TICKS,
    ROLES,
    STATES,
    rne_div,
    HardpointSpec,
    ProfileSpec,
    scaled,
)
from tactical_fixture_encode import (
    player_bytes,
    unit_bytes,
    order_bytes,
    command_body,
    command_bytes,
    header_bytes,
    replay_bytes,
    event_bytes,
    state_bytes,
    visibility_masks,
    snapshot_bytes,
    fog_grid_bytes,
    fog_cells,
    command_offsets,
    mutations,
)




# --- Tactical rules v1 -------------------------------------------------------------------


def execute(units: dict[int, Unit], teams: dict[int, int], command: Command) -> list[dict[str, object]]:
    issuer_team = teams[command.player_id]
    command_reason = "none"
    if command.kind == "attack":
        target = units.get(command.target)
        if target is None:
            command_reason = "target_not_live"
        elif teams[target.owner] == issuer_team:
            command_reason = "target_not_hostile"
    events = []
    for unit_id in command.units:
        reason = command_reason
        unit = units.get(unit_id)
        if reason == "none":
            if unit is None:
                reason = "unit_not_live"
            elif unit.owner != command.player_id:
                reason = "unit_not_owned"
        if reason == "none":
            assert unit is not None
            unit.order = Order(command.kind, command.tick, command.destination, command.target)
        events.append({
            "tick": command.tick,
            "player": command.player_id,
            "sequence": command.sequence,
            "unit": unit_id,
            "event": "order_accepted" if reason == "none" else "order_rejected",
            "order": command.kind,
            "reason": reason,
        })
    return events





def simulate(players, initial, commands) -> dict[str, object]:
    teams = {player.player_id: player.team_id for player in players}
    units = {unit.entity_id: replace(unit, order=Order()) for unit in initial}
    next_id = max(units) + 1 if units else 1
    rng_state = SEED
    # sim_headless binds no sensor table, so each unit is seen by its own team only.
    sensors: dict[int, int] = {}
    snapshots = [{"tick": 0, "sha256": hashlib.sha256(snapshot_bytes(players, units, 0, [], sensors)).hexdigest()}]
    hashes = []
    all_events = []
    ticks = []
    for tick in range(FINAL_TICK_COUNT):
        events = []
        for command in commands:
            if command.tick == tick:
                events.extend(execute(units, teams, command))
        completed = tick + 1
        state = state_bytes(players, units, completed, next_id, rng_state)
        state_hash = hashlib.sha256(state).hexdigest()
        snapshot = snapshot_bytes(players, units, completed, events, sensors)
        snapshot_hash = hashlib.sha256(snapshot).hexdigest()
        hashes.append({"tick": completed, "sha256": state_hash})
        snapshots.append({"tick": completed, "sha256": snapshot_hash})
        all_events.extend(events)
        ticks.append({
            "completed_tick": completed,
            "state_bytes": len(state),
            "state_sha256": state_hash,
            "snapshot_bytes": len(snapshot),
            "snapshot_sha256": snapshot_hash,
            "event_count": len(events),
            "orders": {
                str(unit_id): {
                    "kind": units[unit_id].order.kind,
                    "issued_tick": units[unit_id].order.issued_tick,
                    "destination_raw": list(units[unit_id].order.destination),
                    "target": units[unit_id].order.target,
                }
                for unit_id in sorted(units)
                if units[unit_id].order.kind != "none"
            },
        })
    return {"hashes": hashes, "snapshots": snapshots, "events": all_events, "ticks": ticks}





class Durability:
    """Hull and hardpoint health of the durable units, with rules HD and HS of the note."""

    def __init__(self, profiles: tuple[ProfileSpec, ...], units: dict[int, Unit]) -> None:
        self.constraint = q24("0.2")      # Hull_Vs_Hard_Points_Health_Constraint
        self.engines_off = q24("0.4")     # Engines_Disabled_Speed_Modifier
        self.damaged = q24("0.33")        # Health_Low_Percent_Threshold
        self.profiles = {profile.type_id: profile for profile in profiles}
        self.health: dict[int, list[int]] = {}   # entity -> [hull, hardpoint...]
        for unit in units.values():
            profile = self.profiles.get(unit.type_id)
            if profile is not None:
                self.health[unit.entity_id] = [profile.hull] + [spec.health for spec in profile.hardpoints]

    def profile(self, unit: Unit) -> ProfileSpec | None:
        return self.profiles.get(unit.type_id) if unit.entity_id in self.health else None

    def destroyed(self, unit_id: int, profile: ProfileSpec, index: int) -> bool:
        return profile.hardpoints[index].destroyable and self.health[unit_id][1 + index] == 0

    def role_alive(self, unit_id: int, profile: ProfileSpec, role: str, when_absent: bool) -> bool:
        indices = [i for i, spec in enumerate(profile.hardpoints) if spec.role == role]
        if not indices:
            return when_absent
        return any(not self.destroyed(unit_id, profile, i) for i in indices)

    def damage(self, unit_id: int, profile: ProfileSpec, target: int, amount: int) -> tuple[int | None, bool]:
        """HD-02, HD-03, HD-20, HD-21: returns (destroyed hardpoint, unit destroyed)."""
        health = self.health[unit_id]
        if amount <= 0:
            return None, False
        if target == HULL_TARGET:
            if health[0] <= 0:
                return None, False
            health[0] = max(0, health[0] - amount)
            return None, health[0] == 0
        if health[1 + target] <= 0:
            return None, False
        health[1 + target] = max(0, health[1 + target] - amount)
        if health[1 + target] > 0:
            return None, False
        dead = False
        if profile.dies_with_hardpoints and not any(
                spec.destroyable and health[1 + i] > 0 for i, spec in enumerate(profile.hardpoints)):
            health[0] = 0
            dead = True
        return target, dead

    def service(self, unit_id: int, profile: ProfileSpec) -> tuple[list[int], bool]:
        """HS-01 to HS-04 over exact integers: returns (destroyed hardpoints, unit destroyed)."""
        health = self.health[unit_id]
        maxima = [spec.health for spec in profile.hardpoints]
        t = sum(value for spec, value in zip(profile.hardpoints, maxima) if spec.destroyable)
        s = sum(health[1 + i] for i, spec in enumerate(profile.hardpoints) if spec.destroyable)
        if t == 0 or health[0] <= 0:
            return [], False
        m, c, k = profile.hull, self.constraint, ONE
        if profile.dies_with_hardpoints:
            cap = min(m, rne_div(s * m * k + c * m * t, t * k))
            if health[0] > cap:
                health[0] = cap
                if cap == 0:
                    return [], True
        h = health[0]
        current = s * m * k
        limit = min(t * m * k, t * h * k + t * c * m)
        if current <= limit:
            return [], False
        excess = current - limit
        before = list(health)
        destroyed = []
        for i, spec in enumerate(profile.hardpoints):
            if not spec.destroyable or before[1 + i] <= 0:
                continue
            loss = rne_div(excess * before[1 + i], m * k * t)
            if loss <= 0:
                continue
            health[1 + i] = max(0, before[1 + i] - loss)
            if health[1 + i] == 0:
                destroyed.append(i)
        return destroyed, False

    def state_bytes(self, unit: Unit) -> bytes:
        if unit.entity_id not in self.health:
            return b""
        health = self.health[unit.entity_id]
        return struct.pack("<qII", health[0], len(health) - 1, 0) + b"".join(struct.pack("<q", v) for v in health[1:])

    def instance_bytes(self, unit: Unit) -> bytes:
        profile = self.profile(unit)
        if profile is None:
            return struct.pack("<II", 0, 0)
        health = self.health[unit.entity_id]
        engines = self.role_alive(unit.entity_id, profile, "engine", True)
        shields = self.role_alive(unit.entity_id, profile, "shield_generator", True)
        launch = self.role_alive(unit.entity_id, profile, "fighter_bay", False)
        factor = ONE if engines else self.engines_off
        speed = rne_div(profile.max_speed * factor, ONE) if profile.max_speed is not None else 0
        flags = (1 if engines else 0) | (2 if shields else 0) | (4 if launch else 0) | (8 if profile.max_speed is not None else 0)
        data = struct.pack("<IIqqqqII", 1, len(profile.hardpoints), health[0], profile.hull, factor, speed, flags, 0)
        for i, spec in enumerate(profile.hardpoints):
            value = health[1 + i]
            if not spec.destroyable:
                state = "intact"
            elif value == 0:
                state = "destroyed"
            elif value * ONE < spec.health * self.damaged:
                state = "damaged"
            else:
                state = "intact"
            enabled = not self.destroyed(unit.entity_id, profile, i)
            data += struct.pack("<qBBBBI", value, ROLES[spec.role], STATES[state], 1 if enabled else 0, 0, 0)
        return data





def simulate_durability(players, initial, commands, profiles) -> dict[str, object]:
    teams = {player.player_id: player.team_id for player in players}
    units = {unit.entity_id: replace(unit, order=Order()) for unit in initial}
    durability = Durability(profiles, units)
    next_id = max(units) + 1
    sensors: dict[int, int] = {}
    snapshots = [{"tick": 0, "sha256": hashlib.sha256(
        snapshot_bytes(players, units, 0, [], sensors, durability)).hexdigest()}]
    hashes, all_events, ticks = [], [], []

    def destruction(tick: int, kind: str, unit: Unit, hardpoint: int = 0) -> dict[str, object]:
        return {"tick": tick, "player": unit.owner, "sequence": 0, "unit": unit.entity_id, "event": kind,
                "order": "none", "reason": "none", "hardpoint": hardpoint}

    for tick in range(DURABILITY_TICKS):
        events = []
        for command in commands:
            if command.tick != tick:
                continue
            if command.kind != "damage":
                events.extend(execute(units, teams, command))
                continue
            for unit_id in command.units:
                unit = units.get(unit_id)
                profile = durability.profile(unit) if unit is not None else None
                if unit is None:
                    reason = "unit_not_live"
                elif profile is None:
                    reason = "not_damageable"
                elif command.hardpoint != HULL_TARGET and (
                        command.hardpoint >= len(profile.hardpoints)
                        or not profile.hardpoints[command.hardpoint].destroyable):
                    reason = "hardpoint_invalid"
                else:
                    reason = "none"
                events.append({"tick": tick, "player": command.player_id, "sequence": command.sequence,
                               "unit": unit_id, "event": "order_accepted" if reason == "none" else "order_rejected",
                               "order": "damage", "reason": reason})
                if reason != "none":
                    continue
                destroyed, dead = durability.damage(unit_id, profile, command.hardpoint, command.amount)
                if destroyed is not None:
                    events.append(destruction(tick, "hardpoint_destroyed", unit, destroyed))
                if dead:
                    events.append(destruction(tick, "unit_destroyed", unit))
                    del units[unit_id]
                    del durability.health[unit_id]
        for unit_id in sorted(units):
            profile = durability.profile(units[unit_id])
            if profile is None:
                continue
            destroyed, dead = durability.service(unit_id, profile)
            events.extend(destruction(tick, "hardpoint_destroyed", units[unit_id], i) for i in destroyed)
            if dead:
                events.append(destruction(tick, "unit_destroyed", units[unit_id]))
                del units[unit_id]
                del durability.health[unit_id]
        completed = tick + 1
        state = state_bytes(players, units, completed, next_id, DURABILITY_SEED, DURABILITY_CONTENT, durability)
        snapshot = snapshot_bytes(players, units, completed, events, sensors, durability)
        hashes.append({"tick": completed, "sha256": hashlib.sha256(state).hexdigest()})
        snapshots.append({"tick": completed, "sha256": hashlib.sha256(snapshot).hexdigest()})
        all_events.extend(events)
        ticks.append({
            "completed_tick": completed,
            "state_sha256": hashes[-1]["sha256"],
            "snapshot_sha256": snapshots[-1]["sha256"],
            "health_raw": {str(unit_id): list(values) for unit_id, values in sorted(durability.health.items())},
            "live_units": sorted(units),
        })
    return {"hashes": hashes, "snapshots": snapshots, "events": all_events, "ticks": ticks}
