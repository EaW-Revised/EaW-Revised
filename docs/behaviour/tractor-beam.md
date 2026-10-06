# Tactical tractor-beam presentation

This note covers the visible line between a tractor hardpoint and its held
target. It changes presentation only; ability admission and movement remain
under WHE-27 and WHE-57..60.

| Rule | Visible behavior | Evidence |
| --- | --- | --- |
| TBF-01 | `Tractor_Beam_Width` is the full world-space width, with half on either side of the centreline. The line uses `Tractor_Beam_Texture` and `Tractor_Beam_Color`, additive blending, depth testing and no backface culling. Colour reaches the draw as byte RGB, rather than values above one. FoC authors width 10 and RGB 0,75,0. The shared energy line has the same geometry and colour conventions. | debug build: beam creation, simple-line geometry and drawing; effective `gameconstants.xml` |
| TBF-02 | A simple beam maps texture U across its width and V from 1 at the source to 0 at the target. It uses the whole texture. `Tractor_Beam_Frames` sets the animation counter; it is not an atlas-cell count. Simple-line UVs remain fixed while its highlights move. A zero count stops the additional animation service, but endpoint refresh still advances the highlights. | debug build: line creation, animation service and placement; effective texture is 64 by 64 pixels |
| TBF-03 | Four additive highlights use `w_galaxy_dot.tga`, full size 3.5 by 3.5 and 70 percent of the beam colour, quantized to bytes. They start at fractions 0.45, 0.70, 0.50 and 0.89 of the beam, travel toward the source and wrap around a 50-advance cycle. Both endpoint refresh and animation servicing advance their positions. | debug build: tractor line setup, highlight creation and placement |
| TBF-04 | The large green arcs under the selected source ship are its ordinary faction selection ring, independent of the tractor effect. Its side follows WU-02, `Select_Box_Scale` times `Scale_Factor`. | WU-01/02; selected Thrawn debug-build reference with fog off and Environment 1 |

The viewer tracks presentation birth per active source and removes that state
when the beam releases. Its 30 Hz battle clock represents endpoint refresh and,
for a nonzero frame count, animation servicing; pause holds both. The exact
sub-frame phase relative to the original renderer remains unverified.

The debug build also requests a separate authored lightning effect. This note
does not establish its visible contribution. Energy-beam highlight sampling and
per-ability energy appearance overrides remain outside this tractor correction.
