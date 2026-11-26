# Campaign battle co-op mate colours: the `Coop_Player_Colours` list (PROVISIONAL)

## Applicability

- Product: this project's campaign battle (Galactic Conquest) in a co-op game, several human seats (and allied AI) on one
  side. The default colouring is the owner's design (legacy EAWR-8355); the mod tag is the owner's proposal (legacy EAWR-8359).
- Source tags: **project** (an owner decision). Nothing here is a retail rule: the original game has no co-op campaign.
- **PROVISIONAL.** The tag names and shape may be renamed after the owner has tried them; no back-compat promise. A rename is
  one edit in `src/presentation/ui/minimap.cpp` (`read_coop_colours`), the registry rows and this note.

## Interface

- Input: Factions.xml, on the `Faction` object, next to `Color` and `No_Colorization_Color`:

  ```xml
  <Faction Name="Rebel_Alliance">
      <Coop_Player_Colours>
          <Coop_Player_Colour>0 128 128 255</Coop_Player_Colour>  <!-- 1st mate -->
          <Coop_Player_Colour>255 0 128</Coop_Player_Colour>      <!-- 2nd mate, alpha optional -->
      </Coop_Player_Colours>
  </Faction>
  ```

- Input: the campaign's seat table every peer shares (seat ID, faction, lobby slot, active), with the battle's controller
  policy applied, so a seat that leaves the battle turns inactive on every peer at the same tick.
- Output: the model and minimap colour of the local faction's main player and of each team mate of the local side
  (`gc_participant_colour`, `gc_main_seat`) (legacy EAWR-8355) (legacy EAWR-8359).

## Rules

The 3D participant-coloured look requires at least two active humans in the viewed faction (legacy EAWR-8554).
With fewer, models retain the colour-off look even if allied AI are present. The minimap is unchanged: faction-units
mode uses its normal player-colour fallback, while player-coloured-units mode uses the participant colour lookup.
Spectators use the observed battle player's faction for the 3D look.

| Rule | Behaviour | Source |
|---|---|---|
| GCC-01 | Every enemy keeps its faction's own colour-off look, whatever the list says. The main player of the local faction (GCC-08) takes the faction's COLOURED main team colour, the Factions.xml `Color` (Rebels: red), never a list entry; a faction without `Color` keeps the colour-off look. Only the mates on the local side take a list entry. | project (legacy EAWR-8355), owner correction 11-25 (legacy EAWR-8359) |
| GCC-02 | The list of the local seat's faction applies. Entry n is the n-th mate: human mates in slot order (GCC-08) first, then allied AI seats in the caller's order. | project (legacy EAWR-8359) |
| GCC-08 | The main player is the player at position 1 in the faction's lobby slot: the faction's active human seats ordered by lobby slot (the lobby's seat order, sealed into the roster), then seat ID (a co-op seat admitted mid-campaign joins behind the faction's players). Never the host and never the local seat, so every peer shows the same main player. Positions 2, 3... are the mates of GCC-02. When the first player leaves (its seat turns inactive), the next player becomes the first and takes the main colour; the rest move up one mate rank. A unit with no recorded controller counts as the main player's. | owner decision 11-25 (legacy EAWR-8359) |
| GCC-03 | Absent list, empty list, or a mate past the end of a shorter list: that mate takes the built-in shade family of the faction's colour-off colour (`No_Colorization_Color`) (legacy EAWR-8355), as before this tag existed. | project (legacy EAWR-8359) |
| GCC-07 | A unit follows its controlling seat: a gift re-tints it by GCC-01..03 and GCC-08 to the receiver's colour (main: team colour; mate: its entry or shade; enemy: colour-off look); a change of main player (GCC-08) re-tints the same way. | project (legacy EAWR-8355) (legacy EAWR-8359) |
| GCC-04 | Each entry is 3 or 4 integers 0-255 (R G B [A]); the alpha is ignored. | project (legacy EAWR-8359) |
| GCC-05 | Load check: a list is rejected whole when an entry is not a colour, when there are more than 7 entries, when two entries repeat the same RGB, or when an entry equals the faction's own `Color` or `No_Colorization_Color`. The diagnostic (`EAWR-UI-0331`) names the faction and the file, and that faction then uses GCC-03. Other factions are unaffected. | project (legacy EAWR-8359) |
| GCC-06 | Layers and submods: Factions.xml entries resolve as every faction definition does, a later definition of the faction replaces the whole definition, so the list is replaced as a whole for that faction only. | project (legacy EAWR-8359), MM-07 |

## Cases

- GCC-C1: a faction lists teal and pink; the faction's first player takes the faction's `Color`, the 1st human mate is teal, the 2nd pink,
  the 3rd a shade.
- GCC-C3: a mate gifts a unit to the main player: it turns from the mate's colour to the faction's `Color`.
- GCC-C4: two humans in one faction, the host at lobby position 2: both peers show the guest (position 1) in the faction's `Color` and the
  host in the 1st mate's colour.
- GCC-C5: the first player leaves mid-battle: on every peer the second turns to the faction's `Color`, the third to the 1st mate's colour, and their units with
  them.
- GCC-C2: a list of `5 5 5` and `5 5 5 255` is rejected (GCC-05); the faction's mates take shades.

## Unknowns

- Whether the owner wants the list per faction, per side or per seat colour once the shape is tried (hence PROVISIONAL).
- The developer switch `EAWR_GC_ALLY_OWN_COLOURS=1` (palette colours) (legacy EAWR-8355) loses against a list entry, and still applies to mates past its end.
