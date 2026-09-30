# Tactical battle end: the win/lose message and the end of the battle

## Applicability

- Product: Star Wars Empire at War: Forces of Corruption, a space skirmish decided by the
  lobby's win condition ([space victory](space-victory.md), VT-01 to VT-12). The claims come from
  the FoC debug build, read on 2026-09-28 under the [clean-room rule](../clean-room.md); the
  private evidence map (IDs BX-01 to BX-08) is under the ignored `out/research/`. EAWR-453, gap 2 of
  the Phase 2 plan, and the victory/defeat part of EAWR-83's acceptance.
- Bounded question: what the local player sees and hears when the battle is decided, what the
  7-second countdown (VT-11) is counted in, and what "the battle ends" means in a skirmish.
- Source tags: **research** (debug build), **data** (a FoC file), **project** (a remake choice)
  and **unverified**.

## Interface

- Input: the outcome ([space victory](space-victory.md): winner, winner's team, deciding frame,
  `end_tick`) and the local player.
- Output: the win or lose message, its music and sound, and at `end_tick` the end of the battle.
- Retained state: none in the simulation beyond the outcome. The display is presentation.

## Rules

| Rule | Behaviour | Source |
|---|---|---|
| BE-01 | In the frame that decides the battle the game starts the tactical win or lose music (the local faction's win music against the loser's faction when the local player won, its lose music against the winner's faction otherwise), plays the local faction's "won/lost space battle" HUD sound event and shows the win or lose message. A condition may suppress the message and its sound (story battles); the skirmish condition does not. | research BX-01, BX-02, BX-03 |
| BE-02 | The message is `TEXT_WIN_TACTICAL` ("WE ARE VICTORIOUS!") when the local player is the winner or the winner's ally, and `TEXT_LOSE_TACTICAL` ("WE HAVE BEEN DEFEATED!") when it is the winner's enemy (VT-10). A player that is neither sees none. | research BX-01, BX-04; data (text) |
| BE-03 | The message is one line of text in `Win_Lose_Message_Font` (EmpireAtWar-Bold) at `Win_Lose_Message_Font_Size` 24, in `Win_Message_Color` (223, 243, 255, 255) or `Lose_Message_Color` (255, 244, 223, 255) (`GameConstants.xml`). It is centred horizontally (its left edge at half the screen width less half its width) and placed at 0.4 of the screen height from the top. A new message replaces the old one; it stays up until the battle ends. | research BX-04; data |
| BE-04 | The countdown is `end_tick` minus the deciding frame, 210 logical frames (VT-11). It counts logical frames, so a pause holds it and the game speed changes how long it lasts on the wall clock ([time controls](tactical-time-controls.md) TM-07). The battle goes on while it runs. | research BX-05 |
| BE-05 | When the countdown reaches zero in a skirmish, the tactical mode pauses and the full-screen battle end dialog (`IDD_BATTLE_END_DIALOG`) opens: the result, the units of each side and the battle's figures, with its own summary-screen win or lose music. In a campaign battle the game returns to the galaxy instead. The rig's retail stills (fog off, Coruscant, EAWR-453 staging probe) show the dialog titled `TEXT_WIN_BATTLE` ("You have won the battle!") or `TEXT_LOST_BATTLE` ("You have lost the battle!") with `TEXT_BATTLE_TIME`, `TEXT_YOUR_LOSSES` and `TEXT_ENEMY_LOSSES` panes listing the lost units, and one Exit button. | research BX-06, BX-07, BX-08; rig capture |

### Project choices

| Rule | Choice |
|---|---|
| BEP-01 | The viewer's live battle shows the BE-02 message from the first presented frame that carries the outcome, drawn by the HUD as BE-03 describes (the font's pixel size follows UI-F1 like the other HUD text). |
| BEP-02 | The live session halts at `end_tick`: the last tick it runs is `end_tick`, the presented tick holds there, and the time buttons and orders stop. Its replay ends at `end_tick`, so a headless run of it matches. |
| BEP-03 | At `end_tick` a minimal end panel replaces the battle end dialog: `TEXT_VICTORY` ("Victory!") or `TEXT_DEFEAT` ("Defeat!") over a `TEXT_BUTTON_QUIT_GAME` ("Quit Game") button that closes the viewer. The message stays behind it. |
| BEP-04 | The live session report lists the display (`live_session.battle_end`: the message key and the tick it was first shown, whether the session halted and the tick the panel opened). |

## Cases

| Case | Input | Expected |
|---|---|---|
| BEC-01 | The local player's side destroys the enemy star base at tick T | "WE ARE VICTORIOUS!" from the frame showing T+1; the session halts at T + 210; the panel reads "Victory!" |
| BEC-02 | The local star base falls at tick T | "WE HAVE BEEN DEFEATED!", then "Defeat!" at T + 210 |
| BEC-03 | Paused for P ticks' worth of wall time during the countdown | The panel still opens at tick T + 210, P later on the wall clock |

## Unknowns

| ID | Unknown | Effect |
|---|---|---|
| BE-U1 | Whether BE-03's 0.4 is the text's top edge or its baseline. | The remake takes the top edge; the line may sit up to one line height off. The owner capture (EAWR-311) settles it. |
| BE-U2 | The battle end dialog's layout and figures (BE-05). | The minimal panel stands in for it (BEP-03). |

## Fidelity list

- The full battle end dialog (BE-05, BE-U2): its own ticket later. Until then the panel's title and button
  (Victory!/Defeat!, Quit Game) differ from the dialog's (You have won/lost the battle!, Exit).
- The win/lose music and sound events (BE-01), with the battle audio (EAWR-443).
