#include "corpus_associations_test_support.hpp"

using namespace association_contracts;

int main() {
    test_shorter_prefix_passes_but_is_not_promoted();
    test_both_pass_is_ambiguous();
    test_neither_passes();
    test_unsupported_and_missing_candidates();
    test_zero_candidates_and_retained_stages();
    test_twenty_bone_candidate_rejects_index_twenty();
    test_selected_drift_fails_closed();
    test_frozen_input_gate();
    test_static_clip_disposition();
    test_shuffled_enumeration_is_deterministic();
    test_receipt_escaping();
    test_frozen_identity_unchanged_is_accepted();
    test_changed_selected_model_same_error_is_drift();
    test_selected_provenance_drift();
    test_alternative_identity_drift_and_unrecorded();
    test_candidate_enumeration_drift();
    test_failure_set_drift();
    test_frozen_metadata_parsing();
#if defined(EAWR_ASSOCIATION_FROZEN_MANIFEST)
    test_tracked_manifest_parses();
#else
    std::cout << "note: tracked manifest path not configured\n";
#endif
    test_output_path_aliasing();
    if (failures != 0) return EXIT_FAILURE;
    std::cout << "animation corpus association contracts passed\n";
    return EXIT_SUCCESS;
}
