#pragma once

// Private support for the data_tests contract runner: the shared failure
// counter and the contract groups that run_contracts runs in order.

#include "eawr/data/xml.hpp"

#include <string_view>

namespace eawr::tests::data_contracts {
extern int failures;
void expect(bool condition, std::string_view message);

// xml_catalog_tests.cpp
void run_contracts();
void category_contracts(const eawr::data::Catalog& catalog);

// xml_inheritance_tests.cpp
void inheritance_contracts(const eawr::data::Catalog& catalog, const eawr::core::Result<eawr::vfs::Vfs>& mounted,
                           const eawr::data::LoadOptions& options);

// xml_schema_tests.cpp
void schema_contracts(const eawr::data::Catalog& catalog);

// xml_override_tests.cpp
void override_contracts(const eawr::data::Catalog& catalog, const eawr::vfs::Vfs& filesystem);

// tag_trace_tests.cpp
void tag_trace_contracts();
} // namespace eawr::tests::data_contracts
