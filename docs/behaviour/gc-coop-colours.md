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

- Output: the model and minimap colour of each team mate of the local side (`gc_participant_colour`) (legacy EAWR-8355).

## Rules

| Rule | Behaviour | Source |
|---|---|---|
| GCC-01 | The local seat and every enemy keep their faction's own look, whatever the list says. Only the mates on the local side take an entry. | project (legacy EAWR-8355) |
| GCC-02 | The list of the local seat's faction applies. Entry n is the n-th mate: human mates in seat order first, then allied AI seats in the caller's order. | project (legacy EAWR-8359) |
| GCC-03 | Absent list, empty list, or a mate past the end of a shorter list: that mate takes the built-in shade family of the faction colour (legacy EAWR-8355), as before this tag existed. | project (legacy EAWR-8359) |
| GCC-04 | Each entry is 3 or 4 integers 0-255 (R G B [A]); the alpha is ignored. | project (legacy EAWR-8359) |
| GCC-05 | Load check: a list is rejected whole when an entry is not a colour, when there are more than 7 entries, when two entries repeat the same RGB, or when an entry equals the faction's own `Color` or `No_Colorization_Color`. The diagnostic (`EAWR-UI-0331`) names the faction and the file, and that faction then uses GCC-03. Other factions are unaffected. | project (legacy EAWR-8359) |
| GCC-06 | Layers and submods: Factions.xml entries resolve as every faction definition does, a later definition of the faction replaces the whole definition, so the list is replaced as a whole for that faction only. | project (legacy EAWR-8359), MM-07 |

## Cases

- GCC-C1: a faction lists teal and red; the local seat is untinted, the 1st human mate is teal, the 2nd red, the 3rd a shade.
- GCC-C2: a list of `5 5 5` and `5 5 5 255` is rejected (GCC-05); the faction's mates take shades.

## Unknowns

- Whether the owner wants the list per faction, per side or per seat colour once the shape is tried (hence PROVISIONAL).
- The developer switch `EAWR_GC_ALLY_OWN_COLOURS=1` (palette colours) (legacy EAWR-8355) loses against a list entry, and still applies to mates past its end.
