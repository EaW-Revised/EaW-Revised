#include "production_private.hpp"
#include "galactic_support.hpp"
#include <exception>
#include <stdexcept>

using namespace galactic_production_test;
using namespace galactic_test;

int case_galactic_production_tests(int argc, char** argv) {
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--offer-projection") {
            offer_projection(); return failures == 0 ? 0 : 1;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--ground-sale") {
            ground_structure_sale(); unit_sale(); return failures == 0 ? 0 : 1;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--retained-census") {
            census_campaign_retained_counts(); return failures == 0 ? 0 : 1;
        }
        if (argc == 3 && std::string_view(argv[1]) == "--write-corpus") { write_corpus(argv[2]); return 0; }
        else if (argc != 2) throw std::runtime_error("expected <fixtures directory> or --write-corpus <new directory>");
        fixture_root = argv[1];
        native_action_production_barrier(); ground_structure_sale(); unit_sale();
        admission(); offer_projection(); orbit_completion_shares_slot_card(); technology_price_production(); cancel_identity(); timing_and_cancel(); inactive_work_budget(); refund_recipient(); finish_task_behind_front(); queue_policies(); content_and_placement(); base_and_tech_completion(); live_starbase_blockers(); production_corpus_bytes(); blocker_parity_checkpoint(); parity_checkpoint();
        shared_technology_writer(); story_production_writers(); census_admission(); census_campaign_retained_counts(); company_original_binding(); census_container_and_capacity(); census_rounding_and_data(); census_composite_counts(); census_stock_composite_boundary(); census_corpus_checkpoint();
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    return failures == 0 ? 0 : 1;
}
