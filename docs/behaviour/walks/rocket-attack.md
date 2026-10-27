# Walk: rocket attack mode

Scope: `ROCKET_ATTACK` on live space bodies, including standalone heroes. Evidence
comes from targeted read-only debug-build lookups and the effective stock XML.
Opaque evidence IDs below refer to private research records.

| Rule | Behaviour | Evidence |
|---|---|---|
| WRA-01 | `ROCKET_ATTACK` is a modal ordinary ability. Activation requires the ordinary enabled/ready gate; switching it changes its active flag and updates the shared expiration timer. Positive `Expiration_Seconds` runs at 30 logical frames per second. Expiry switches back to normal mode and begins `Recharge_Seconds`; early cancellation uses the shared proportional recharge. With no expiration it remains active until switched off and does not recharge when cancelled. | debug build ERA-01; [WAB-02 to WAB-13](abilities.md) |
| WRA-02 | Each hardpoint independently reads `Fire_When_In_Rocket_Attack_Mode` while its parent has the ability active, and `Fire_When_In_Normal_Attack_Mode` otherwise. Both omitted flags default to Yes. A mode does not select a projectile category: a hardpoint retains its own projectile, range, pulses and recharge. The gate also applies to a pulse from a volley already started. | debug build ERA-02, ERA-03 |
| WRA-03 | The first authored active ordinary ability with a positive `Targeting_Max_Attack_Distance` supplies the unit's targeting range instead of its ordinary value. Missing, zero and negative ability ranges leave the ordinary range. Acquisition, target suitability, turret targeting and approach use the current range. Hardpoint `Fire_Range_Distance` remains independent; the object's own weapon uses the current targeting range. Existing targeting modifiers and target-extent adjustments retain their ordinary consumers. | debug build ERA-04, ERA-05; [space targeting](../space-targeting.md) |
| WRA-04 | `Supports_Autofire` permits storing the usual per-ability preference. It does not make this modal ability activate when an enemy appears: it has no targeted nested user-input handler and no stock rocket-specific object script. Explicit commands and scripts still switch it. | debug build ERA-01; [WAB-30 and WAB-36](abilities.md); stock XML/Lua inventory |
| WRA-05 | Stock authoring includes the T4B land vehicle, its captured variant and its company entry, and destroyer-droid land data. No stock space unit authors this ability. Binding the kind therefore exposes a previously unsupported stock land ability as well as modded space abilities. | effective stock XML inventory |

WRA-02 also applies to manual assignment admission and its frozen public predicate.
The frozen view carries the current mode, and gun availability combines that mode
with hardpoint health. A mode change that makes an existing manual request
ineligible follows the existing manual eligibility timeout instead of keeping the
gun busy indefinitely.

The implementation adds no timer service or frame-order rule: it uses the existing
ability countdown and copied inputs in the partitioned combat phases.

Nearby interfaces: the object-weapon flags `Fire_Weapon_When_In_Rocket_Attack_Mode`
and `Fire_Weapon_When_In_Normal_Attack_Mode` use the same modal selection in the
debug build (ERA-06), but their land object-weapon loader/firing path is outside
this space-hardpoint change. Deployment and other named mode flags remain
separate gates. Stock land firing integration belongs to the land combat work.
Projectile rocket flight remains governed by the independent
[rocket-flight rules](../rocket-flight.md).
