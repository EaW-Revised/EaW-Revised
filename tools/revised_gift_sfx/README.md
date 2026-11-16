# Gift audio XML recovery

These three XML files are exact copies of the Revised layer's gift configuration.
The selection-advisor installer test compares its output with these files.
To restore the mapping after regenerating the layer, copy these three XML files
into that layer's `Data/XML` folder. Do not copy or replace audio files.

The `GIFT_UNITS` action uses the same advisor mapping as selection commands.
Both giving and receiving players hear their local advisor's configured event
once per committed gift command, including campaign child battles.
Missing gift audio is silent. The installer does not copy gift WAVs.

Each gift event lists four takes (`gift_01` through `gift_04`) with
`Play_Sequentially=Yes`. The ordinary SFX queue cycles them in authored order
(SND-05/08); it uses uniform random selection when that flag is absent or false.
