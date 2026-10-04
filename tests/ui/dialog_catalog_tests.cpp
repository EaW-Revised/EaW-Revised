#include "dialog_catalog_support.hpp"
// Dialog catalogue contracts (#169): the resource-script tokenizer, the
// resource.h symbols, the DIALOGEX parser over every control form the retail
// script uses, GUIDialogs.xml overrides and their resolution. All fixtures are
// written here. With EAWR_EAW_GAME_ROOT set, the FoC dialog sources are also
// read through the FoC VFS and audited; with EAWR_MOD_HUD_ROOTS as well, each
// listed mod's dialog sources are loaded. Nothing from an install is written.

#include "eawr/data/ui/dialog_catalog.hpp"
#include "eawr/data/ui/dialog_script.hpp"
#include "eawr/data/ui/text_database.hpp"

#include "ui_test_support.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dialog_catalog_test_support {

} // namespace

using namespace dialog_catalog_test_support;

void dialog_catalog_contracts() {
    tokenizer_contracts();
    header_contracts();
    script_contracts();
    rc_mod_tolerance_contracts();
    skin_contracts();
    resolution_contracts();
    audit_contracts();
    vfs_contracts();
    corpus_contracts();
    mod_corpus_contracts();
}
