# eaw-layerz fixture

`layermanager.lua` and `configuration.lua` are copies of the MIT-licensed library
https://github.com/gruenwaldlk/eaw-layerz (scripts/library/eaw-layerz). Each file keeps the upstream licence notice
as a header. The only change to the library text is the Corvette range in `configuration.lua`, which is set to -40..40
instead of the upstream 0..0 placeholder so a test can see the heights vary.

They are test inputs for `foc_ai_contracts` (`layering_object_bindings`) and are not part of the shipped game.
