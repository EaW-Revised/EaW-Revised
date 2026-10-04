#pragma once

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

using eawr::test::ui::expect;
namespace ui = eawr::data::ui;





extern const std::string_view header_fixture;
extern const std::string_view script_fixture;
extern const std::string_view skin_fixture;
extern const std::string_view tolerance_script;
extern const std::string_view tolerance_header;
extern const std::string_view tolerance_skin;
eawr::assets::Source source(const std::string& path);
std::span<const std::byte> bytes_of(const std::string_view text);
ui::TextDatabase text_fixture();
const ui::DialogControl* control(const ui::Dialog& dialog, const std::string_view id_name);
bool has_code(const std::vector<eawr::core::Diagnostic>& diagnostics, const std::string_view code,
              const std::string_view fragment);
void tokenizer_contracts();
void header_contracts();
void script_contracts();
void rc_mod_tolerance_contracts();
ui::DialogCatalog catalog_fixture();
void skin_contracts();
void resolution_contracts();
void audit_contracts();
void vfs_contracts();
void corpus_contracts();
void mod_corpus_contracts();


} // namespace dialog_catalog_test_support
