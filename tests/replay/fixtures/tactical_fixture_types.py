"""Independent tactical fixture types helpers; no production simulation imports."""
from __future__ import annotations
from dataclasses import dataclass, field, replace
from decimal import ROUND_HALF_EVEN, Decimal, getcontext
import argparse
import hashlib
import json
from pathlib import Path
import struct
import sys





OUT = Path(__file__).resolve().parent



NAME = "tactical-v2"



MAGIC = b"EAWRPLY\x00"



STATE_MAGIC = b"EAWRTST\x00"



SNAPSHOT_MAGIC = b"EAWRTSN\x00"



FORMAT_VERSION = 2



HEADER_SIZE = 104



RULES_VERSION = 1



STATE_ENCODING_VERSION = 1



SNAPSHOT_ENCODING_VERSION = 3



MATH_VERSION = 1



FRACTIONAL_BITS = 24



ONE = 1 << FRACTIONAL_BITS



TICK_NUMERATOR = 1



TICK_DENOMINATOR = 30



FINAL_TICK_COUNT = 6



SEED = 0x5EED0000_00000066



CONTENT_IDENTITY = hashlib.sha256(b"EAWR-P2-03-tactical-v2-synthetic-content\n").digest()



COMMANDABLE = 1




OPCODES = {"stop": 1, "move": 2, "attack": 3, "damage": 4}



ORDER_KINDS = {"none": 0, **OPCODES}



EVENT_KINDS = {"order_accepted": 1, "order_rejected": 2, "hardpoint_destroyed": 3, "unit_destroyed": 4}



REASONS = {
    "none": 0,
    "unit_not_live": 1,
    "unit_not_owned": 2,
    "target_not_live": 3,
    "target_not_hostile": 4,
    "not_damageable": 5,
    "hardpoint_invalid": 6,
}



HULL_TARGET = 0xFFFFFFFF





@dataclass(frozen=True)
class Player:
    player_id: int
    team_id: int
    faction_id: int
    flags: int





@dataclass
class Order:
    kind: str = "none"
    issued_tick: int = 0
    destination: tuple[int, int, int] = (0, 0, 0)
    target: int = 0





@dataclass
class Unit:
    entity_id: int
    type_id: int
    owner: int
    position: tuple[int, int, int]
    rotation: tuple[int, int, int, int]
    order: Order = field(default_factory=Order)





@dataclass(frozen=True)
class Command:
    tick: int
    player_id: int
    sequence: int
    kind: str
    units: tuple[int, ...]
    destination: tuple[int, int, int] = (0, 0, 0)
    target: int = 0
    amount: int = 0             # damage: Q24 raw
    hardpoint: int = HULL_TARGET  # damage: HardPoints index, or the hull





# --- Q24 helpers -------------------------------------------------------------------------


def q24(text: str) -> int:
    """Exact decimal text to Q24 raw, nearest-even."""
    return int((Decimal(text) * ONE).quantize(Decimal(1), rounding=ROUND_HALF_EVEN))





def vec(x: str, y: str, z: str) -> tuple[int, int, int]:
    return (q24(x), q24(y), q24(z))





def rne_shift(value: int) -> int:
    """value / 2^24 rounded to nearest, ties to even (symmetric for negatives)."""
    quotient, remainder = divmod(value, ONE)
    if remainder * 2 > ONE or (remainder * 2 == ONE and quotient % 2 == 1):
        quotient += 1
    return quotient





def decimal_pi() -> Decimal:
    return Decimal("3.14159265358979323846264338327950288419716939937510582097494459")





def decimal_sin_cos(angle: Decimal) -> tuple[Decimal, Decimal]:
    sine = Decimal(0)
    cosine = Decimal(0)
    term = Decimal(1)
    for n in range(0, 80):
        if n % 2 == 0:
            cosine += term if n % 4 == 0 else -term
        else:
            sine += term if n % 4 == 1 else -term
        term = term * angle / (n + 1)
    return sine, cosine





def yaw_quat(degrees: int) -> tuple[int, int, int, int]:
    """Right-handed rotation about +Z (turns-free: exact decimal authoring input only)."""
    getcontext().prec = 60
    half = Decimal(degrees) * decimal_pi() / Decimal(360)
    sine, cosine = decimal_sin_cos(half)
    raw = [
        0,
        0,
        int((sine * ONE).quantize(Decimal(1), rounding=ROUND_HALF_EVEN)),
        int((cosine * ONE).quantize(Decimal(1), rounding=ROUND_HALF_EVEN)),
    ]
    norm = sum(component * component for component in raw)
    assert abs(norm - ONE * ONE) <= 8 * ONE, (degrees, norm)
    return (raw[0], raw[1], raw[2], raw[3])





def to_matrix(rotation: tuple[int, int, int, int], position: tuple[int, int, int]) -> list[list[int]]:
    """docs/fixed-point.md: exact widened sums, one nearest-even rounding per component."""
    x, y, z, w = rotation
    norm = x * x + y * y + z * z + w * w
    if abs(norm - ONE * ONE) > 8 * ONE:
        raise ValueError("non-unit quaternion")

    def diagonal(a: int, b: int) -> int:
        return rne_shift(ONE * ONE - 2 * a * a - 2 * b * b)

    def pair(a: int, b: int, c: int, d: int, subtract: bool) -> int:
        second = 2 * c * d
        return rne_shift(2 * a * b + (-second if subtract else second))

    return [
        [diagonal(y, z), pair(x, y, z, w, True), pair(x, z, y, w, False), position[0]],
        [pair(x, y, z, w, False), diagonal(x, z), pair(y, z, x, w, True), position[1]],
        [pair(x, z, y, w, True), pair(y, z, x, w, False), diagonal(x, y), position[2]],
    ]






# --- P2-05 visibility fixture ------------------------------------------------------------
#
# Rules v1 moves nothing, so the approach and retreat of one enemy fighter is a sequence of
# one-tick replays ("frames"), each a setup with the fighter at the next position. Sensor
# ranges are the FoC Space_FOW_Reveal_Range values of the named types; the type IDs are
# synthetic.

VISIBILITY = "tactical-visibility"



VISIBILITY_SEED = 0x5EED0000_00000068



VISIBILITY_CONTENT = hashlib.sha256(b"EAWR-P2-05-tactical-visibility-synthetic-content\n").digest()



VISIBILITY_SENSORS = (
    (102, "Nebulon_B_Frigate", "1200"),
    (104, "X-Wing", "500"),
    (203, "TIE_Fighter", "500"),
    (301, "Orbital_Resource_Container", "100"),
)



FOG_LAYOUT = {
    "origin_x_raw": q24("-4096"), "origin_y_raw": q24("-4096"),
    "cell_x_raw": q24("256"), "cell_y_raw": q24("256"), "width": 32, "height": 32,
}



TIE = 5



FRIGATE = 1





# --- P2-09 hull and hardpoint durability ---------------------------------------------------

DURABILITY = "tactical-durability"



DURABILITY_SEED = 0x5EED0000_00000072



DURABILITY_CONTENT = hashlib.sha256(b"EAWR-P2-09-tactical-durability-foc-shaped-content\n").digest()



DURABILITY_TICKS = 6



ROLES = {"other": 0, "weapon": 1, "engine": 2, "shield_generator": 3, "fighter_bay": 4, "special_ability": 5}



STATES = {"intact": 0, "damaged": 1, "destroyed": 2}





def rne_div(numerator: int, denominator: int) -> int:
    """numerator / denominator (both >= 0) rounded to nearest, ties to even."""
    quotient, remainder = divmod(numerator, denominator)
    if 2 * remainder > denominator or (2 * remainder == denominator and quotient % 2 == 1):
        quotient += 1
    return quotient





@dataclass(frozen=True)
class HardpointSpec:
    role: str
    health: int = 0          # Q24 raw maximum; zero when not destroyable
    repair_amount: int = 0
    repair_cost: int = 0

    @property
    def destroyable(self) -> bool:
        return self.health > 0





@dataclass(frozen=True)
class ProfileSpec:
    type_id: int
    name: str
    hull: int
    max_speed: int | None
    hardpoints: tuple[HardpointSpec, ...]
    dies_with_hardpoints: bool = False





def scaled(value: str) -> int:
    """FoC Health or Tactical_Health x Object_Max_Health_Multiplier_Space 1.5, as Q24 raw."""
    return rne_div(q24(value) * q24("1.5"), ONE)
