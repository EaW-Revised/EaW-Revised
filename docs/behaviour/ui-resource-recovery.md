# Dialog resource separator recovery

The dialog resources are mod inputs. Recovery preserves the loaded dialog and
the positions of its remaining controls.

| Rule | Behaviour | Evidence | Applied |
|---|---|---|---|
| UI-RC1 | Repeated commas between unquoted fields separate tokens without adding an empty numeric field. Thus `215,,22` supplies width 215 and height 22. Quoted commas remain caption text. | Debug build: the resource tokenizer classifies comma as whitespace and skips whitespace before reading the next token. | `src/data/ui/dialog_script_parser.cpp`; repeated-separator and quoted-caption contracts in `tests/ui/dialog_script_tests.cpp`. |

This recovery applies to repeated separators. Missing values at a statement
boundary, missing required fields and incomplete style expressions retain their
existing diagnostics.
