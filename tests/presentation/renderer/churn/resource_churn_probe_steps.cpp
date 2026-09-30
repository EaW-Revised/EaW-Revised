#include "resource_churn_probe.hpp"

namespace {

// A one-bone palette that moves the plate; read back as posed.
std::vector<presentation::animation::BonePose> moved_palette() {
    presentation::animation::BonePose posed;
    posed.skin_asset = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 25, 0, 0, 1};
    return std::vector<presentation::animation::BonePose>{posed};
}

} // namespace

void EawrResourceChurnProbe::build_steps() {
    const ProbeAssets probe_assets{solid(255, 0, 0), solid(0, 255, 0), solid(0, 0, 255, 32),
        unshaded("1.0, 0.0, 0.0"), unshaded("0.0, 1.0, 0.0"), unshaded("0.0, 0.0, 1.0")};
    build_baseline_steps(probe_assets);
    build_upload_steps(probe_assets);
    build_churn_steps();
    build_churn_summary_step();
    build_unavailable_steps(probe_assets);
    build_skin_pose_steps();
    build_pose_retire_steps();
    build_skin_pose_release_step(probe_assets);
    build_shutdown_steps(probe_assets);
}

void EawrResourceChurnProbe::build_baseline_steps(const ProbeAssets& probe_assets) {
    const assets::Texture& red_texture = probe_assets.red_texture;
    const assets::Texture& blue_texture = probe_assets.blue_texture;
    const presentation::MaterialDescription& red_material = probe_assets.red_material;
    const presentation::MaterialDescription& blue_material = probe_assets.blue_material;
    // A throwaway renderer draws a plain and a skinned instance once, so
    // engine resources created lazily on the first 3D draw are in the
    // baseline; the baseline is then taken with no renderer alive.
    steps_.push_back({"warm-up", [=, this] {
        renderer_ = std::make_unique<GodotRenderer>(*host_);
        upload(red, plate(60.0F), red_texture, red_material);
        upload(skinned, plate(60.0F, 1, true), blue_texture, blue_material);
        submit(scene(1, {{1, red, -200}, {2, skinned, 200}}), 2);
    }, [this] { expect_drawn("warm-up"); }});
    steps_.push_back({"warm-up-destroyed", [this] {
        renderer_.reset();
        expected_drawn_ = 0;
    }, [this] {
        baseline_ = sample();
        record("baseline", baseline_);
        check(baseline_.objects == 0, "objects drawn with no renderer alive");
    }});
}

void EawrResourceChurnProbe::build_upload_steps(const ProbeAssets& probe_assets) {
    const assets::Texture& red_texture = probe_assets.red_texture;
    const assets::Texture& green_texture = probe_assets.green_texture;
    const assets::Texture& blue_texture = probe_assets.blue_texture;
    const presentation::MaterialDescription& red_material = probe_assets.red_material;
    const presentation::MaterialDescription& green_material = probe_assets.green_material;
    const presentation::MaterialDescription& blue_material = probe_assets.blue_material;
    // Shared references and replacement discipline.
    steps_.push_back({"upload-shared", [=, this] {
        renderer_ = std::make_unique<GodotRenderer>(*host_);
        upload(red, plate(60.0F), red_texture, red_material);
        upload(green, plate(60.0F), green_texture, green_material);
        upload(skinned, plate(60.0F, 1, true), blue_texture, blue_material);
        // The same content again is a second lease on the same RID bundle.
        upload(red, plate(60.0F), red_texture, red_material);
        check(references(red) == 2, "identical re-upload must share the live bundle");
        // Different content under the live ID is refused, not silently kept.
        const auto conflicting = renderer_->upload(red, plate(60.0F), green_texture, red_material);
        check(!conflicting && conflicting.error().code == presentation::diagnostic_codes::duplicate_asset,
            "a different texture under a live asset ID must fail with EAWR-RENDER-0002");
        check(references(red) == 2, "a refused conflicting upload must not change the lease count");
        expected_drawn_ = 0;
    }, [this] {
        with_assets_ = sample();
        record("with-assets", with_assets_);
        check(with_assets_.texture > baseline_.texture && with_assets_.buffer > baseline_.buffer,
            "uploaded textures and meshes must be visible in RenderingServer memory");
        evidence_.push_back("\"shared_references\":" + std::to_string(references(red)));
        evidence_.push_back(std::string("\"conflicting_upload_refused\":")
            + (count_code(presentation::diagnostic_codes::duplicate_asset) == 1 ? "true" : "false"));
    }});

    // A failed upload after GPU allocation leaves no RID behind.
    steps_.push_back({"failed-upload", [this] {
        const std::size_t before = renderer_->resources().size();
        const auto failed = renderer_->upload(partial, partially_invalid_plate(), solid(9, 9, 9, 64), unshaded("0.5, 0.5, 0.5"));
        check(!failed && failed.error().code == presentation::diagnostic_codes::upload_failed,
            "an out-of-range palette bone must fail the upload with EAWR-RENDER-0005");
        check(renderer_->resources().size() == before && references(partial) == 0,
            "a failed upload must not enter the registry");
    }, [this] {
        const Sample now = sample();
        record("failed-upload", now);
        check(now.texture == with_assets_.texture && now.buffer == with_assets_.buffer,
            "a failed upload must free its texture and partial mesh (RenderingServer memory "
            + std::to_string(now.texture) + "/" + std::to_string(now.buffer) + " vs "
            + std::to_string(with_assets_.texture) + "/" + std::to_string(with_assets_.buffer) + ")");
        evidence_.push_back(std::string("\"failed_upload_memory_restored\":")
            + (now.texture == with_assets_.texture && now.buffer == with_assets_.buffer ? "true" : "false"));
    }});
}

void EawrResourceChurnProbe::build_churn_steps() {
    // Repeated scene switching: shared assets, asset swaps, skinned
    // instances, unavailable assets, empty scenes and fresh entity IDs. The
    // fresh skinned entities are posed like a live sim poses spawned units,
    // and retired with clear_skin_pose once the next cycle starts, so the
    // pose store stays bounded while every cycle poses two new IDs.
    for (int cycle = 0; cycle < churn_cycles; ++cycle) {
        const sim::EntityId fresh = 1000 + static_cast<sim::EntityId>(cycle) * 4;
        const sim::EntityId retired = fresh - 4;
        const std::uint64_t tick = 100 + static_cast<std::uint64_t>(cycle) * 8;
        const std::vector<std::pair<std::shared_ptr<const sim::RenderSnapshot>, std::size_t>> scenes{
            {scene(tick, {{1, red, -480}, {2, red, -360}, {3, red, -240}, {4, red, -120}, {5, red, 0},
                {6, red, 120}, {7, green, 240}, {8, skinned, 360}, {9, skinned, 480}}), 9},
            {scene(tick + 1, {{1, red, -480}, {2, red, -360}, {3, red, -240}, {8, skinned, 360}}), 4},
            {scene(tick + 2, {}), 0},
            {scene(tick + 3, {{1, green, -480}, {2, green, -360}, {3, green, -240}, {4, green, -120},
                {5, green, 0}, {6, green, 120}, {7, red, 240}, {8, skinned, 360}, {9, skinned, 480}}), 9},
            {scene(tick + 4, {{1, red, -480}, {2, red, -360}, {3, red, -240}, {4, unavailable, -120},
                {5, unavailable, 0}, {6, unavailable, 120}, {7, unavailable_other, 240}, {8, skinned, 360}}), 4},
            {scene(tick + 5, {{fresh, red, -300}, {fresh + 1, green, -100}, {fresh + 2, skinned, 100},
                {fresh + 3, skinned, 300}}), 4},
        };
        for (std::size_t item = 0; item < scenes.size(); ++item) {
            const auto snapshot = scenes[item].first;
            const std::size_t expected = scenes[item].second;
            steps_.push_back({"churn-" + std::to_string(cycle) + "-" + std::to_string(item), [=, this] {
                if (item == 0 && cycle > 0) {
                    renderer_->clear_skin_pose(retired + 2);
                    renderer_->clear_skin_pose(retired + 3);
                }
                if (item == 5) {
                    const auto palette = moved_palette();
                    check(renderer_->set_skin_pose(fresh + 2, skinned, palette).has_value()
                        && renderer_->set_skin_pose(fresh + 3, skinned, palette).has_value(),
                        "fresh skinned entities must accept a pose");
                    churn_posed_ += 2;
                }
                submit(snapshot, expected);
            }, [=, this] { verify_churn_switch(cycle, item, expected, fresh); }, 2});
        }
    }
}

void EawrResourceChurnProbe::verify_churn_switch(
    const int cycle, const std::size_t item, const std::size_t expected, const sim::EntityId fresh) {
    const Sample now = sample();
    check(now.objects == static_cast<std::int64_t>(expected), "churn drew "
        + std::to_string(now.objects) + " objects, expected " + std::to_string(expected));
    ++churn_steps_;
    churn_frames_ += 2;
    churn_max_texture_ = std::max(churn_max_texture_, now.texture);
    const auto counts = renderer_->lifecycle_counts();
    check(counts.instances == expected, "churn left stale or missing registry instances");
    churn_max_poses_ = std::max(churn_max_poses_, counts.skin_poses);
    // Only the current cycle's two fresh skinned entities hold a
    // pose; a retired ID that kept one would grow this per cycle.
    check(counts.skin_poses == (item == 5 ? 2U : 0U),
        "churn holds " + std::to_string(counts.skin_poses) + " stored poses");
    if (item == 5) {
        // evidence() re-reads, so each answer is taken at once.
        for (const sim::EntityId entity : {fresh + 2, fresh + 3}) {
            const auto* read_back = evidence(entity);
            if (read_back && read_back->skeleton_posed) ++churn_posed_readback_;
        }
    }
    if (item == 0) {
        // Every full scene holds the same two skeletons; their
        // transform textures must not accumulate across switches.
        if (cycle == 0) {
            churn_full_ = now;
            record("churn-full-first", now);
        } else if (now.texture != churn_full_.texture || now.buffer != churn_full_.buffer) {
            ++churn_texture_drift_;
        }
        check(counts.skeletons == 2, "a full scene must hold exactly two live skeletons");
    }
    if (item == 2) {
        check(now.texture == with_assets_.texture && now.buffer == with_assets_.buffer,
            "an empty scene must hold exactly the uploaded assets' memory");
        check(counts.skeletons == 0, "an empty scene must hold no skeleton");
    }
}

void EawrResourceChurnProbe::build_churn_summary_step() {
    steps_.push_back({"churn-summary", [this] {
        const sim::EntityId last = 1000 + static_cast<sim::EntityId>(churn_cycles - 1) * 4;
        renderer_->clear_skin_pose(last + 2);
        renderer_->clear_skin_pose(last + 3);
        submit(scene(900, {}), 0);
    }, [this] {
        const Sample now = sample();
        record("after-churn", now);
        check(churn_texture_drift_ == 0, "RenderingServer memory drifted across full-scene switches");
        check(now.texture == with_assets_.texture && now.buffer == with_assets_.buffer,
            "churn must return RenderingServer memory to the uploaded assets");
        check(renderer_->resources().size() == 3 && references(red) == 2,
            "churn must not change the resource registry");
        const std::size_t poses_after = renderer_->lifecycle_counts().skin_poses;
        check(poses_after == 0, "retired churn entities must leave no stored pose");
        check(churn_posed_readback_ == churn_posed_, "every posed fresh entity must read back as posed");
        evidence_.push_back("\"churn\":{\"cycles\":" + std::to_string(churn_cycles) + ",\"switches\":"
            + std::to_string(churn_steps_) + ",\"frames\":" + std::to_string(churn_frames_)
            + ",\"full_scene_memory_drift\":" + std::to_string(churn_texture_drift_)
            + ",\"max_texture_memory\":" + std::to_string(churn_max_texture_)
            + ",\"posed_fresh_entities\":" + std::to_string(churn_posed_)
            + ",\"posed_readback\":" + std::to_string(churn_posed_readback_)
            + ",\"max_skin_poses\":" + std::to_string(churn_max_poses_)
            + ",\"skin_poses_after\":" + std::to_string(poses_after) + "}");
    }});
}

void EawrResourceChurnProbe::build_unavailable_steps(const ProbeAssets& probe_assets) {
    const assets::Texture& red_texture = probe_assets.red_texture;
    const presentation::MaterialDescription& green_material = probe_assets.green_material;
    // Unavailable asset: the stale instance goes at once; a steady wait is
    // reported once; the entity recovers when the asset arrives.
    const auto waiting = scene(1000, {{1, red, -240}, {2, late, 0}, {3, red, 240}, {4, unavailable, 120}});
    const auto recovered_scene = scene(1001, {{1, red, -240}, {2, late, 0}, {3, red, 240}});
    steps_.push_back({"unavailable-before", [this] {
        submit(scene(999, {{1, red, -240}, {2, red, 0}, {3, red, 240}}), 3);
    }, [this] { expect_drawn("unavailable-before"); }});
    // A sentinel diagnostic just before the wait: a steady wait may add its
    // two reports once, but must not rotate the sentinel out of the bounded
    // history or change its tail while the scene is unchanged.
    constexpr std::string_view sentinel = "renderer asset 2 is already uploaded with different";
    steps_.push_back({"unavailable-replacement", [=, this] {
        const auto conflicting = renderer_->upload(green, plate(60.0F), red_texture, green_material);
        check(!conflicting && conflicting.error().code == presentation::diagnostic_codes::duplicate_asset,
            "sentinel conflicting upload must be refused");
        submit(waiting, 2);
        check(renderer_->lifecycle_counts().missing_asset_waits == 2, "two entities must wait on unloaded assets");
    }, [=, this] {
        expect_drawn("unavailable-replacement");
        steady_tail_ = tail(3);
        check(steady_tail_.size() == 3 && steady_tail_[0].find(sentinel) != std::string::npos
                && steady_tail_[1].find("unloaded renderer asset 77") != std::string::npos
                && steady_tail_[2].find("unloaded renderer asset 404") != std::string::npos,
            "the wait must be reported once per entity right after the sentinel");
    }});
    for (int frame = 0; frame < steady_missing_frames; ++frame) {
        steps_.push_back({"unavailable-steady-" + std::to_string(frame), [=, this] { submit(waiting, 2); },
            [] {}, 1});
    }
    steps_.push_back({"unavailable-steady-summary", [=, this] { submit(waiting, 2); }, [=, this] {
        expect_drawn("unavailable-steady");
        const std::vector<std::string> now = tail(3);
        const bool unchanged = now == steady_tail_;
        check(unchanged, "a steady wait must not add diagnostics on every submit");
        check(has_message(sentinel), "the sentinel must survive the steady wait in the bounded history");
        evidence_.push_back("\"steady_missing\":{\"frames\":" + std::to_string(steady_missing_frames + 2)
            + ",\"history_tail_unchanged\":" + (unchanged ? "true" : "false")
            + ",\"sentinel_retained\":" + (has_message(sentinel) ? "true" : "false") + "}");
    }});
    steps_.push_back({"unavailable-recovery", [=, this] {
        upload(late, plate(60.0F, 2), solid(255, 255, 0), unshaded("1.0, 1.0, 0.0"));
        submit(recovered_scene, 3, 4);
        check(renderer_->lifecycle_counts().missing_asset_waits == 0, "recovered entities must stop waiting");
    }, [this] {
        expect_drawn("unavailable-recovery");
        const auto* item = evidence(2);
        check(item && item->asset_id == late && item->mesh_surfaces == 2,
            "the recovered entity must draw the arrived two-surface asset");
    }});
    // Explicit replacement: new content under the same ID needs release first.
    steps_.push_back({"replacement", [=, this] {
        const auto refused = renderer_->upload(late, plate(60.0F, 3), solid(255, 255, 0), unshaded("1.0, 1.0, 0.0"));
        check(!refused && refused.error().code == presentation::diagnostic_codes::duplicate_asset,
            "new content under a live ID must be refused");
        check(renderer_->release(late).has_value(), "release of the late asset");
        check(renderer_->instance_count() == 2, "releasing a live asset must remove its instance immediately");
        upload(late, plate(60.0F, 3), solid(255, 255, 0), unshaded("1.0, 1.0, 0.0"));
        submit(recovered_scene, 3, 5);
    }, [this] {
        expect_drawn("replacement");
        const auto* item = evidence(2);
        check(item && item->mesh_surfaces == 3, "the replaced asset's new three-surface mesh must be drawn");
        evidence_.push_back(std::string("\"replacement_reads_back_new_mesh\":")
            + (item && item->mesh_surfaces == 3 ? "true" : "false"));
    }});
    // Live release while referenced, then recovery by re-upload.
    steps_.push_back({"live-release", [=, this] {
        check(renderer_->release(late).has_value(), "live release");
        check(renderer_->instance_count() == 2, "live release must remove the instance before the next submit");
        submit(recovered_scene, 2);
    }, [this] { expect_drawn("live-release"); }});
    steps_.push_back({"live-release-recovery", [=, this] {
        upload(late, plate(60.0F), solid(255, 255, 0), unshaded("1.0, 1.0, 0.0"));
        submit(recovered_scene, 3);
    }, [this] { expect_drawn("live-release-recovery"); }});
}

void EawrResourceChurnProbe::build_skin_pose_steps() {
    // Skin poses end with their asset: a pose left pending on an entity must
    // not bind to a later re-upload of the same asset ID.
    steps_.push_back({"skin-pose-pending", [this] {
        presentation::animation::BonePose posed;
        posed.skin_asset = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 25, 0, 0, 1};
        const std::vector<presentation::animation::BonePose> palette{posed};
        check(renderer_->set_skin_pose(20, skinned, palette).has_value(), "pending pose for entity 20");
        check(renderer_->set_skin_pose(21, skinned, palette).has_value(), "pending pose for entity 21");
        submit(scene(1100, {{21, skinned, 0}}), 1);
    }, [this] {
        expect_drawn("skin-pose-control");
        const auto* item = evidence(21);
        // Positive control: the read-back sees a bound pose.
        check(item && item->skeleton_bones == 1 && item->skeleton_posed, "a bound pose must read back as posed");
    }});
    steps_.push_back({"skin-pose-absence", [this] {
        submit(scene(1101, {}), 0);
        submit(scene(1102, {{21, skinned, 0}}), 1);
    }, [this] {
        expect_drawn("skin-pose-absence");
        const auto* item = evidence(21);
        check(item && item->skeleton_posed, "a posed entity returning after an absence keeps its pose");
    }});
}

void EawrResourceChurnProbe::build_pose_retire_steps() {
    // Retiring posed entities: absence alone keeps every pose (the store
    // cannot tell a retired ID from a briefly absent one), so this is the
    // growth a live sim would see; clear_skin_pose ends it, and clearing a
    // live instance returns its skeleton to rest.
    constexpr std::size_t retiring = 40;
    constexpr sim::EntityId first_retiring = 3000;
    steps_.push_back({"skin-pose-retire-spawn", [=, this] {
        pose_base_ = renderer_->lifecycle_counts().skin_poses;
        std::vector<Member> members{{21, skinned, -480}, {22, skinned, 480}};
        const auto palette = moved_palette();
        check(renderer_->set_skin_pose(22, skinned, palette).has_value(), "pose for live entity 22");
        for (std::size_t index = 0; index < retiring; ++index) {
            const sim::EntityId entity = first_retiring + index;
            check(renderer_->set_skin_pose(entity, skinned, palette).has_value(), "pose for retiring entity");
            members.push_back({entity, skinned, -468 + static_cast<std::int64_t>(index) * 24});
        }
        submit(scene(1110, members), retiring + 2);
    }, [=, this] {
        expect_drawn("skin-pose-retire-spawn");
        std::size_t posed = 0;
        for (std::size_t index = 0; index < retiring; ++index) {
            const auto* item = evidence(first_retiring + index);
            if (item && item->skeleton_posed) ++posed;
        }
        check(posed == retiring, "every retiring entity must read back as posed");
        check(renderer_->lifecycle_counts().skin_poses == pose_base_ + retiring + 1, "spawned poses are stored");
    }});
    steps_.push_back({"skin-pose-retire-absent", [this] {
        submit(scene(1111, {{21, skinned, -200}, {22, skinned, 200}}), 2);
    }, [=, this] {
        expect_drawn("skin-pose-retire-absent");
        const std::size_t kept = renderer_->lifecycle_counts().skin_poses - pose_base_ - 1;
        check(kept == retiring, "absent entities keep their poses until cleared");
        evidence_.push_back("\"pose_retire\":{\"posed\":" + std::to_string(retiring)
            + ",\"kept_while_absent\":" + std::to_string(kept));
    }});
    steps_.push_back({"skin-pose-retire-clear", [=, this] {
        for (std::size_t index = 0; index < retiring; ++index) renderer_->clear_skin_pose(first_retiring + index);
        renderer_->clear_skin_pose(22);
        renderer_->clear_skin_pose(9999); // no pose: nothing to end
        submit(scene(1112, {{21, skinned, -200}, {22, skinned, 200}}), 2);
    }, [this] {
        expect_drawn("skin-pose-retire-clear");
        const std::size_t after = renderer_->lifecycle_counts().skin_poses;
        check(after == pose_base_, "clear_skin_pose must end every retired pose");
        const auto* live = evidence(22);
        const bool rest = live && live->skeleton_bones == 1 && !live->skeleton_posed;
        check(rest, "clearing a live entity's pose must return its skeleton to rest");
        const auto* kept = evidence(21);
        const bool untouched = kept && kept->skeleton_posed;
        check(untouched, "clearing other entities must not touch entity 21's pose");
        evidence_.back() += ",\"after_clear\":" + std::to_string(after - pose_base_)
            + ",\"cleared_live_reads_rest\":" + (rest ? "true" : "false")
            + ",\"other_pose_kept\":" + (untouched ? "true" : "false") + "}";
    }});
}

void EawrResourceChurnProbe::build_skin_pose_release_step(const ProbeAssets& probe_assets) {
    const assets::Texture& blue_texture = probe_assets.blue_texture;
    const presentation::MaterialDescription& blue_material = probe_assets.blue_material;
    steps_.push_back({"skin-pose-release", [=, this] {
        submit(scene(1103, {}), 0);
        const std::size_t poses = renderer_->lifecycle_counts().skin_poses;
        check(poses >= 2, "both poses must be stored before the release");
        check(renderer_->release(skinned).has_value(), "release of the skinned asset");
        check(renderer_->lifecycle_counts().skin_poses == poses - 2, "poses must end with their asset");
        upload(skinned, plate(60.0F, 1, true), blue_texture, blue_material);
        submit(scene(1104, {{20, skinned, -200}, {21, skinned, 200}}), 2);
    }, [this] {
        expect_drawn("skin-pose-release");
        const auto* first = evidence(20);
        const bool first_rest = first && first->skeleton_bones == 1 && !first->skeleton_posed;
        const auto* second = evidence(21);
        const bool second_rest = second && second->skeleton_bones == 1 && !second->skeleton_posed;
        check(first_rest && second_rest, "a re-uploaded asset must not inherit poses from its released predecessor");
        evidence_.push_back(std::string("\"stale_pose_after_reupload\":") + (first_rest && second_rest ? "false" : "true"));
    }});
}

void EawrResourceChurnProbe::build_shutdown_steps(const ProbeAssets& probe_assets) {
    const assets::Texture& red_texture = probe_assets.red_texture;
    const presentation::MaterialDescription& red_material = probe_assets.red_material;
    // Shutdown with live shared, skinned and recovered instances.
    steps_.push_back({"shutdown", [this] {
        submit(scene(1200, {{1, red, -360}, {2, red, -120}, {3, green, 120}, {4, skinned, 360}, {5, late, 0}}), 5);
        const auto counts = renderer_->lifecycle_counts();
        evidence_.push_back("\"shutdown_live\":{\"assets\":" + std::to_string(counts.assets) + ",\"instances\":"
            + std::to_string(counts.instances) + ",\"skeletons\":" + std::to_string(counts.skeletons)
            + ",\"skin_poses\":" + std::to_string(counts.skin_poses) + ",\"red_references\":"
            + std::to_string(references(red)) + "}");
        renderer_.reset();
        expected_drawn_ = 0;
    }, [this] {
        after_shutdown_ = sample();
        record("after-shutdown", after_shutdown_);
        check(after_shutdown_.objects == 0, "no object may be drawn after the renderer is destroyed");
        check(after_shutdown_.texture == baseline_.texture && after_shutdown_.buffer == baseline_.buffer,
            "shutdown must return RenderingServer memory to the baseline");
    }});
    // A fresh renderer on the same host after shutdown draws and releases.
    steps_.push_back({"restart", [=, this] {
        renderer_ = std::make_unique<GodotRenderer>(*host_);
        upload(red, plate(60.0F), red_texture, red_material);
        submit(scene(1300, {{1, red, -200}, {2, red, 0}, {3, red, 200}}), 3);
    }, [this] { expect_drawn("restart"); }});
    steps_.push_back({"restart-release", [this] {
        check(renderer_->release(red).has_value(), "restart release");
        check(renderer_->resources().empty() && renderer_->instance_count() == 0,
            "the final release must empty the registry and instances");
        renderer_.reset();
        expected_drawn_ = 0;
    }, [this] {
        after_restart_ = sample();
        record("after-restart", after_restart_);
        check(after_restart_.objects == 0 && after_restart_.texture == baseline_.texture
                && after_restart_.buffer == baseline_.buffer,
            "the restarted renderer must also return RenderingServer to the baseline");
    }});
}
