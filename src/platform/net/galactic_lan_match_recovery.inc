// Included inside GalacticLanMatch::State; all methods run on the live I/O owner.
using RecoveryRefusal = net::GalacticRecoveryRefusal;
using RecoveryStage = net::GalacticRecoveryStage;
struct RecoveryBegin {
    net::PeerIndex source{};
    std::promise<RecoveryRefusal> result;
};
class RecoveryPair final : public net::MessageTransport {
public:
    RecoveryPair(net::MessageTransport& raw, net::PeerIndex source, net::PeerIndex target)
        : raw_(raw), source_(source), target_(target) {}
    core::Result<void> send(net::PeerIndex to, std::span<const std::uint8_t> bytes) override {
        return raw_.send(to == 0 ? source_ : target_, bytes);
    }
    std::optional<net::Received> poll() override { return {}; }
    net::LinkState link(net::PeerIndex peer) const override { return raw_.link(peer == 0 ? source_ : target_); }
    net::PeerIndex self() const override { return raw_.self() == source_ ? 0U : 1U; }
    std::uint32_t peer_count() const override { return 2; }
private:
    net::MessageTransport& raw_;
    net::PeerIndex source_{}, target_{};
};
struct RecoveryIncident {
    net::PeerIndex source{}, target{};
    std::uint64_t epoch{}, started{}, progress{}, fence_started{};
    net::GalacticRecoveryRequest request;
    std::optional<std::uint64_t> barrier;
    net::GalacticCheckpointTransfer transfer{true};
    std::optional<net::GalacticRecoveryStream> stream;
    std::shared_ptr<const std::vector<std::uint8_t>> checkpoint;
    std::optional<net::GalacticRecoveryPackage> package;
    std::vector<std::uint8_t> sending;
    std::size_t sent{};
    std::optional<net::GalacticRecoveryWorker::Result> candidate;
    std::optional<net::GalacticRecoveryControl> expected;
    std::vector<net::GalacticRecoveryProof> proofs;
    std::vector<bool> published;
    std::optional<std::promise<RecoveryRefusal>> result;
    std::optional<std::future<RecoveryRefusal>> publication;
    bool initial_ready{}, worker_started{}, final_log{}, voted{}, commit_seen{}, commit_sent{}, request_sent{}, local_published{}, release_seen{}, refusal_sent{};
    std::uint8_t removed{};
};
std::optional<RecoveryBegin> recovery_begin_;
std::unique_ptr<RecoveryIncident> recovery_incident_;
net::GalacticRecoveryWorker recovery_worker_;
net::GalacticRecoveryAnchor recovery_worker_frontier_;
std::vector<std::uint64_t> recovery_high_epochs_, recovery_seen_edges_;
std::uint64_t recovery_request_epoch_{}, recovery_now_{}, recovery_retry_after_{};
    unsigned recovery_auto_attempts_{};

void attach_recovery_receiver() {
    session_->set_galactic_recovery_handler([this](net::Received frame) { receive_live_recovery(std::move(frame)); });
    session_->set_galactic_recovery_gate([this] {
        return !recovery_quarantined_ && (!recovery_incident_ || !recovery_incident_->barrier
            || adapter_->completed_tick() < *recovery_incident_->barrier);
    });
}
void recovery_notice(RecoveryRefusal reason) {
    recovery_refusal_ = std::string(net::galactic_recovery_reason(reason));
    const std::lock_guard lock(mutex_); view_.recovery_refusal = recovery_refusal_;
}
void send_recovery_refusal(net::PeerIndex peer, std::uint64_t epoch, RecoveryRefusal reason, std::optional<net::PeerIndex> target = {}) {
    std::vector<std::uint8_t> bytes{net::gc_recovery_refusal_kind};
    for (unsigned i = 0; i < 8; ++i) bytes.push_back(static_cast<std::uint8_t>(epoch >> (8U * i)));
    bytes.push_back(static_cast<std::uint8_t>(reason));
    const auto seat = target.value_or(recovery_incident_ && recovery_incident_->epoch == epoch ? recovery_incident_->target : peer);
    for (unsigned i = 0; i < 4; ++i) bytes.push_back(static_cast<std::uint8_t>(seat >> (8U * i)));
    const bool committed = recovery_incident_ && (recovery_incident_->commit_seen || recovery_incident_->commit_sent || recovery_incident_->local_published);
    bytes.push_back(static_cast<std::uint8_t>(committed));
    if (peer != transport_.self() && transport_.link(peer) == net::LinkState::open)
        static_cast<void>(transport_.send(peer, bytes));
}
void stop_live_recovery(RecoveryRefusal reason, bool notify = true) {
    recovery_notice(reason);
    // All votes fix the new frontier. Late malformed/duplicate traffic cannot
    // roll an already committed membership back to the previous core.
    if (recovery_incident_ && (recovery_incident_->commit_seen || recovery_incident_->commit_sent || recovery_incident_->local_published)) return;
    // A sent vote may already have completed the source's proof set on another
    // pair stream. Without an all-peer abort proof this owner cannot roll back.
    if (recovery_incident_ && recovery_incident_->voted && transport_.self() != recovery_incident_->source) {
        if (notify && !recovery_incident_->refusal_sent) {
            recovery_incident_->refusal_sent = true;
            for (net::PeerIndex peer = 0; peer < transport_.peer_count(); ++peer)
                if (peer != transport_.self()) send_recovery_refusal(peer, recovery_incident_->epoch, reason);
        }
        // Keep the proved candidate and fence until the source's decision.
        // The existing bounded voted-owner timeout quarantines if none arrives.
        return;
    }
    recovery_worker_.cancel();
    if (!recovery_incident_) return;
    if (notify) for (net::PeerIndex peer = 0; peer < transport_.peer_count(); ++peer)
        if (peer != transport_.self()) send_recovery_refusal(peer, recovery_incident_->epoch, reason);
    auto incident = std::move(recovery_incident_);
    if (incident->result) incident->result->set_value(reason);
}
void quarantine_live_recovery(RecoveryRefusal reason) {
    recovery_notice(reason);
    if (recovery_incident_) for (net::PeerIndex peer = 0; peer < transport_.peer_count(); ++peer)
        if (peer != transport_.self()) send_recovery_refusal(peer, recovery_incident_->epoch, reason);
    auto incident = std::move(recovery_incident_);
    if (incident && incident->result) incident->result->set_value(reason);
    recovery_worker_.cancel(); recovery_listening_ = recovery_quarantined_ = true;
    for (net::PeerIndex peer = 0; peer < transport_.peer_count(); ++peer)
        if (peer != transport_.self() && options_.quarantine_recovery_peer) options_.quarantine_recovery_peer(peer);
    failed_ = make_diagnostic(net::reconnect_errors::unavailable, std::string(net::galactic_recovery_reason(reason)));
    const std::lock_guard lock(mutex_);
    view_.phase = GalacticLanPhase::ended; view_.reason = GalacticLanReason::local_left;
    view_.message = "Recovery could not publish this peer; the surviving campaign continues.";
}
bool recovery_send_control(const net::GalacticRecoveryControl& control, bool all) {
    const auto bytes = net::encode_galactic_recovery_control(control);
    if (!bytes) return false;
    bool success = true;
    for (net::PeerIndex peer = 0; peer < transport_.peer_count(); ++peer) {
        if (peer == transport_.self() || (!all && peer != control.source)
            || (control.stage == RecoveryStage::release && (control.removed & (1U << peer)))) continue;
        // Departed seats outside this transaction are not voters.
        if (peer != control.target && recovery_incident_ && recovery_incident_->package
            && recovery_incident_->package->session_frontier
            && recovery_incident_->package->session_frontier->peers[peer].departed) continue;
        if (transport_.link(peer) != net::LinkState::open || !transport_.send(peer, bytes.value())) {
            if (control.stage != RecoveryStage::published) success = false;
        }
    }
    return success;
}
RecoveryRefusal recovery_snapshot(RecoveryIncident& incident) {
    if (recovery_split_ || !recovery_log_) return recovery_split_ ? RecoveryRefusal::divergent_epoch : RecoveryRefusal::unavailable;
    net::GalacticRecoveryPackage package;
    auto refusal = recovery_log_->package(incident.target, incident.request, package);
    if (refusal != RecoveryRefusal::none) return refusal;
    auto frontier = session_->recovery_frontier();
    if (!frontier) return RecoveryRefusal::busy;
    if (frontier.value().split) return RecoveryRefusal::divergent_epoch;
    package.session_frontier = std::move(frontier).value();
    if (incident.barrier) {
        if (package.frontier.completed_tick != *incident.barrier) return RecoveryRefusal::stale_barrier;
        const auto horizon = std::min(options_.final_tick_count,
            *incident.barrier + options_.input_delay + (session_->recovery_options().keep_admission_tick_open ? 0U : 1U));
        for (std::size_t peer = 0; peer < package.session_frontier->peers.size(); ++peer) {
            const auto& row = package.session_frontier->peers[peer];
            if (peer != incident.target && !row.departed && row.next_seal < horizon) return RecoveryRefusal::busy;
        }
    }
    auto encoded = net::encode_galactic_recovery_log(package);
    if (!encoded || encoded.value().size() > options_.recovery_log_bytes) return RecoveryRefusal::limit;
    incident.package = std::move(package); incident.sending = std::move(encoded).value(); incident.sent = 0;
    return RecoveryRefusal::none;
}
void receive_live_recovery(net::Received frame) {
    if (frame.frame.empty()) return;
    if (frame.from >= options_.seats.size() || frame.from == transport_.self()) { recovery_notice(RecoveryRefusal::seat_binding); return; }
    const auto kind = frame.frame.front();
    if (frame.frame.size() > (kind == net::save_manifest_kind ? net::max_setup_bytes + 256 : net::save_chunk_bytes + 256)) {
        recovery_notice(RecoveryRefusal::limit); return;
    }
    if (kind == net::gc_recovery_request_kind) {
        auto request = net::decode_galactic_recovery_request(frame.frame);
        if (!request) { recovery_notice(RecoveryRefusal::malformed); return; }
        if (request.value().seat != options_.seats[frame.from].seat) {
            send_recovery_refusal(frame.from, request.value().transfer_epoch, RecoveryRefusal::seat_binding); return;
        }
        if (recovery_incident_ && recovery_incident_->target == frame.from
            && recovery_incident_->source == transport_.self() && recovery_incident_->epoch == request.value().transfer_epoch
            && recovery_incident_->request.anchor == request.value().anchor) return;
        if (recovery_high_epochs_.size() != options_.seats.size()) recovery_high_epochs_.resize(options_.seats.size());
        if (request.value().transfer_epoch <= recovery_high_epochs_[frame.from]) {
            send_recovery_refusal(frame.from, request.value().transfer_epoch, RecoveryRefusal::stale_barrier); return;
        }
        if (recovery_incident_ || recovery_worker_.busy()) {
            send_recovery_refusal(frame.from, request.value().transfer_epoch, RecoveryRefusal::busy); return;
        }
        if (!options_.recovery_offer || !options_.recovery_restore || failed_) {
            send_recovery_refusal(frame.from, request.value().transfer_epoch, RecoveryRefusal::unavailable); return;
        }
        if (monitor_->frozen() && options_.seats.size() == 2) {
            send_recovery_refusal(frame.from, request.value().transfer_epoch, RecoveryRefusal::divergent_epoch); return;
        }
        auto incident = std::make_unique<RecoveryIncident>();
        incident->source = transport_.self(); incident->target = frame.from; incident->epoch = request.value().transfer_epoch;
        incident->started = incident->progress = recovery_now_; incident->request = request.value();
        const auto refusal = recovery_snapshot(*incident);
        if (refusal != RecoveryRefusal::none) { send_recovery_refusal(frame.from, incident->epoch, refusal); return; }
        net::SaveManifest manifest; manifest.epoch = incident->epoch; manifest.byte_length = incident->package->bytes->size();
        manifest.peers = {0, 1}; manifest.offer = *options_.recovery_offer;
        net::RecoveryTransferIdentity identity; identity.initial_tick = incident->package->checkpoint.completed_tick;
        identity.source = local_seat(); identity.target = options_.seats[frame.from].seat;
        identity.proof[0] = incident->package->checkpoint_digest; identity.proof[1] = incident->package->checkpoint.state;
        identity.proof[2] = core::sha256(incident->sending);
        const auto offer = net::encode_setup_offer(manifest.offer);
        if (!offer) { send_recovery_refusal(frame.from, incident->epoch, RecoveryRefusal::malformed); return; }
        identity.proof[3] = core::sha256(offer.value()); manifest.recovery = identity;
        RecoveryPair pair(transport_, incident->source, incident->target);
        const auto selected = incident->transfer.select(pair, std::move(manifest), incident->package->bytes,
            [](const auto&, auto) { return core::Result<void>::success(); }, recovery_now_);
        if (!selected) { send_recovery_refusal(frame.from, incident->epoch, RecoveryRefusal::restore); return; }
        recovery_high_epochs_[frame.from] = incident->epoch; recovery_incident_ = std::move(incident);
        return;
    }
    if (kind == net::gc_recovery_proof_kind) {
        auto decoded = net::decode_galactic_recovery_control(frame.frame);
        if (!decoded) { recovery_notice(RecoveryRefusal::malformed); return; }
        const auto& control = decoded.value();
        if (control.source >= options_.seats.size() || control.target >= options_.seats.size()) {
            recovery_notice(RecoveryRefusal::seat_binding); return;
        }
        if (control.stage == RecoveryStage::fence && !recovery_incident_) {
            if (frame.from != control.source || transport_.self() == control.target || !recovery_log_
                || control.frontier.epoch != recovery_log_->frontier().epoch
                || control.frontier.lineage != recovery_log_->frontier().lineage || recovery_split_
                || adapter_->completed_tick() > control.barrier || control.barrier >= options_.final_tick_count) {
                send_recovery_refusal(frame.from, control.transfer_epoch, RecoveryRefusal::stale_barrier, control.target); return;
            }
            if (recovery_high_epochs_.size() != options_.seats.size()) recovery_high_epochs_.resize(options_.seats.size());
            if (control.transfer_epoch <= recovery_high_epochs_[control.target]) {
                send_recovery_refusal(frame.from, control.transfer_epoch, RecoveryRefusal::stale_barrier, control.target); return;
            }
            recovery_high_epochs_[control.target] = control.transfer_epoch;
            auto incident = std::make_unique<RecoveryIncident>();
            incident->source = control.source; incident->target = control.target; incident->epoch = control.transfer_epoch;
            incident->started = incident->progress = recovery_now_;
            incident->request = {control.frontier, options_.seats[control.target].seat, control.transfer_epoch};
            recovery_incident_ = std::move(incident);
        }
        if (!recovery_incident_ || control.transfer_epoch != recovery_incident_->epoch
            || control.source != recovery_incident_->source || control.target != recovery_incident_->target) {
            recovery_notice(RecoveryRefusal::stale_barrier); return;
        }
        auto& incident = *recovery_incident_;
        if (control.stage == RecoveryStage::staged) {
            if (incident.barrier && frame.from == incident.target && transport_.self() == incident.source) return;
            if (frame.from != incident.target || transport_.self() != incident.source || !incident.package
                || control.frontier != incident.package->frontier || control.sealed_log != core::sha256(incident.sending)) {
                stop_live_recovery(RecoveryRefusal::state_hash); return;
            }
            if (incident.barrier) return; // identical staged proof is idempotent
            const auto tick = adapter_->completed_tick();
            const bool target_active = !incident.package->session_frontier->peers[incident.target].departed;
            const auto barrier = monitor_->frozen() || target_active ? tick : tick + 64 + options_.input_delay;
            if (barrier >= options_.final_tick_count) { stop_live_recovery(RecoveryRefusal::stale_barrier); return; }
            incident.barrier = barrier; incident.fence_started = recovery_now_; incident.initial_ready = true;
            net::GalacticRecoveryControl fence{incident.epoch, barrier, incident.source, incident.target,
                RecoveryStage::fence, incident.request.anchor, {}};
            if (!recovery_send_control(fence, true)) stop_live_recovery(RecoveryRefusal::unavailable);
            return;
        }
        if (control.stage == RecoveryStage::fence) {
            if (frame.from != incident.source || adapter_->completed_tick() > control.barrier
                || (incident.barrier && *incident.barrier != control.barrier)) { stop_live_recovery(RecoveryRefusal::stale_barrier); return; }
            if (control.frontier != incident.request.anchor) { stop_live_recovery(RecoveryRefusal::anchor_hash); return; }
            if (incident.barrier) return;
            incident.barrier = control.barrier; incident.fence_started = recovery_now_;
            incident.final_log = false; incident.expected.reset(); incident.stream.emplace(incident.epoch, options_.recovery_log_bytes);
            return;
        }
        if (!incident.barrier || control.barrier != *incident.barrier
            || control.frontier.completed_tick != control.barrier) { stop_live_recovery(RecoveryRefusal::stale_barrier); return; }
        if (control.stage == RecoveryStage::vote) {
            const auto old = std::find_if(incident.proofs.begin(), incident.proofs.end(), [&](const auto& row) { return row.peer == frame.from; });
            if (old != incident.proofs.end()) {
                if (old->frontier != control.frontier || old->sealed_log != control.sealed_log) stop_live_recovery(RecoveryRefusal::state_hash);
                return;
            }
            incident.proofs.push_back({frame.from, control.frontier, control.sealed_log});
            if (frame.from == incident.source) incident.expected = control;
            incident.progress = recovery_now_; return;
        }
        if (control.stage == RecoveryStage::commit && frame.from == incident.source && incident.expected
            && control.frontier == incident.expected->frontier && control.sealed_log == incident.expected->sealed_log) {
            incident.commit_seen = true; return;
        }
        if ((control.stage == RecoveryStage::published || control.stage == RecoveryStage::release) && incident.expected
            && control.frontier == incident.expected->frontier && control.sealed_log == incident.expected->sealed_log) {
            if (control.stage == RecoveryStage::release && frame.from == incident.source && incident.local_published
                && !(control.removed & (1U << incident.source)) && !(control.removed & (1U << transport_.self()))
                && (static_cast<std::uint32_t>(control.removed) >> options_.seats.size()) == 0) {
                incident.removed = control.removed; incident.release_seen = true;
            }
            else if (control.stage == RecoveryStage::published) {
                if (incident.published.size() != options_.seats.size()) incident.published.resize(options_.seats.size());
                incident.published[frame.from] = true;
            } else { stop_live_recovery(RecoveryRefusal::malformed); return; }
            return;
        }
        stop_live_recovery(RecoveryRefusal::malformed); return;
    }
    if (!recovery_incident_) { recovery_notice(RecoveryRefusal::unavailable); return; }
    auto& incident = *recovery_incident_;
    if (kind == net::gc_recovery_refusal_kind) {
        if (frame.frame.size() != 15 || frame.frame[14] > 1) { recovery_notice(RecoveryRefusal::malformed); return; }
        std::uint64_t epoch{};
        for (unsigned i = 0; i < 8; ++i) epoch |= static_cast<std::uint64_t>(frame.frame[1 + i]) << (8U * i);
        if (epoch != incident.epoch) { recovery_notice(RecoveryRefusal::stale_barrier); return; }
        std::uint32_t target{};
        for (unsigned i = 0; i < 4; ++i) target |= static_cast<std::uint32_t>(frame.frame[10 + i]) << (8U * i);
        if (target != incident.target) { recovery_notice(RecoveryRefusal::stale_barrier); return; }
        const auto reason = static_cast<RecoveryRefusal>(frame.frame[9]);
        if (incident.commit_seen || incident.commit_sent || incident.local_published || (frame.frame[14] && frame.from == incident.source && incident.voted)) {
            incident.commit_seen = true;
            if (frame.from != incident.source) incident.removed = static_cast<std::uint8_t>(incident.removed | (1U << frame.from));
            recovery_notice(reason > RecoveryRefusal::cancelled ? RecoveryRefusal::malformed : reason); return;
        }
        stop_live_recovery(reason > RecoveryRefusal::cancelled || reason == RecoveryRefusal::none ? RecoveryRefusal::malformed : reason, false); return;
    }
    if (kind == net::gc_recovery_log_kind) {
        if (transport_.self() != incident.target || frame.from != incident.source || !incident.stream) {
            recovery_notice(RecoveryRefusal::seat_binding); return;
        }
        const auto previous = incident.stream->progress();
        const auto refusal = incident.stream->receive(frame.frame);
        if (refusal != RecoveryRefusal::none) { stop_live_recovery(refusal); return; }
        if (incident.stream->progress() > previous) incident.progress = recovery_now_;
        return;
    }
    if (net::GalacticCheckpointTransfer::handles(frame.frame)) {
        if ((transport_.self() != incident.source && transport_.self() != incident.target)
            || frame.from != (transport_.self() == incident.source ? incident.target : incident.source)) {
            recovery_notice(RecoveryRefusal::seat_binding); return;
        }
        RecoveryPair pair(transport_, incident.source, incident.target);
        frame.from = transport_.self() == incident.source ? 1U : 0U;
        auto received = incident.transfer.receive_recovery(pair, frame, [this](const net::SaveManifest& manifest,
            std::shared_ptr<const std::vector<std::uint8_t>> checkpoint) { return recovery_restore_poll(manifest, std::move(checkpoint)); }, recovery_now_);
        if (!received) { stop_live_recovery(RecoveryRefusal::restore); return; }
        incident.progress = recovery_now_; return;
    }
    recovery_notice(RecoveryRefusal::malformed);
}
core::Result<bool> recovery_restore_poll(const net::SaveManifest& manifest,
    std::shared_ptr<const std::vector<std::uint8_t>> checkpoint) {
    const auto failure = [](std::string reason) { return core::Result<bool>::failure(make_diagnostic(net::reconnect_errors::log, std::move(reason))); };
    if (!recovery_incident_ || !manifest.recovery || !options_.recovery_offer) return failure("checkpoint_unavailable");
    auto& incident = *recovery_incident_;
    const auto offer = net::encode_setup_offer(*options_.recovery_offer);
    if (!offer || manifest.recovery->proof[3] != core::sha256(offer.value())
        || manifest.recovery->source != options_.seats[incident.source].seat || manifest.recovery->target != local_seat()) return failure("seat_binding");
    incident.checkpoint = std::move(checkpoint);
    if (!incident.stream || !incident.stream->complete()) return core::Result<bool>::success(false);
    if (!incident.worker_started) {
        auto package = net::decode_galactic_recovery_log(incident.stream->bytes(), incident.checkpoint);
        if (!package || package.value().checkpoint.completed_tick != manifest.recovery->initial_tick
            || package.value().checkpoint.state != manifest.recovery->proof[1]
            || package.value().checkpoint_digest != manifest.recovery->proof[0]
            || core::sha256(incident.stream->bytes()) != manifest.recovery->proof[2]
            || package.value().checkpoint.epoch != incident.request.anchor.epoch
            || package.value().checkpoint.lineage != incident.request.anchor.lineage) return failure("anchor_hash");
        incident.package = std::move(package).value();
        recovery_worker_frontier_ = incident.package->frontier;
        const auto refusal = recovery_worker_.start(*incident.package, options_.recovery_restore);
        if (refusal != RecoveryRefusal::none) return failure(std::string(net::galactic_recovery_reason(refusal)));
        incident.worker_started = true;
    }
    auto candidate = recovery_worker_.take(recovery_worker_frontier_);
    if (!candidate) return core::Result<bool>::success(false);
    if (candidate->refusal != RecoveryRefusal::none) return failure(std::string(net::galactic_recovery_reason(candidate->refusal)));
    incident.candidate = std::move(candidate); incident.progress = recovery_now_;
    return core::Result<bool>::success(true);
}

void service_live_recovery(platform::LiveGameStepContext& context, std::uint64_t now) {
    recovery_now_ = now;
    if (!recovery_incident_ && recovery_worker_.busy()) static_cast<void>(recovery_worker_.take(recovery_worker_frontier_));
    if (recovery_listening_ && !recovery_quarantined_ && !recovery_incident_ && !recovery_worker_.busy()) {
        for (net::PeerIndex peer = 0; peer < transport_.peer_count(); ++peer) {
            if (peer == transport_.self() || transport_.link(peer) != net::LinkState::open) continue;
            const auto edge = transport_.edge_identity(peer);
            if (recovery_seen_edges_.size() != options_.seats.size()) recovery_seen_edges_.resize(options_.seats.size());
            if (edge && edge != recovery_seen_edges_[peer]) recovery_auto_attempts_ = 0;
            if (edge && recovery_auto_attempts_ < 8 && now >= recovery_retry_after_) {
                recovery_seen_edges_[peer] = edge; ++recovery_auto_attempts_; recovery_retry_after_ = now + 3000;
                static_cast<void>(request_recovery(peer)); break;
            }
        }
    }
    if (options_.recovery_restore && !recovery_incident_ && !recovery_worker_.busy() && now >= recovery_retry_after_
        && recovery_auto_attempts_ < 8 && monitor_) {
        if (const auto source = monitor_->checkpoint_recovery_source()) {
            ++recovery_auto_attempts_; recovery_retry_after_ = now + 3000;
            static_cast<void>(request_recovery(*source));
        }
    }
    std::optional<RecoveryBegin> begin;
    { const std::lock_guard lock(mutex_); begin.swap(recovery_begin_); }
    if (begin) {
        const auto refuse = [&](RecoveryRefusal reason) { recovery_notice(reason); begin->result.set_value(reason); };
        if (recovery_incident_ || recovery_worker_.busy()) refuse(RecoveryRefusal::busy);
        else if (!options_.recovery_offer || !options_.recovery_restore || !recovery_log_ || !recovery_log_->checkpoint_anchor().epoch)
            refuse(RecoveryRefusal::unavailable);
        else if (begin->source >= options_.seats.size() || begin->source == transport_.self()) refuse(RecoveryRefusal::seat_binding);
        else if (recovery_split_) refuse(RecoveryRefusal::divergent_epoch);
        else {
            auto incident = std::make_unique<RecoveryIncident>();
            incident->source = begin->source; incident->target = transport_.self(); incident->epoch = ++recovery_request_epoch_;
            incident->started = incident->progress = now;
            incident->request = {recovery_log_->checkpoint_anchor(), local_seat(), incident->epoch};
            incident->stream.emplace(incident->epoch, options_.recovery_log_bytes);
            incident->result.emplace(std::move(begin->result));
            const auto encoded = net::encode_galactic_recovery_request(incident->request);
            recovery_incident_ = std::move(incident);
            if (!encoded) stop_live_recovery(RecoveryRefusal::malformed);
        }
    }
    if (!recovery_incident_) return;
    auto& incident = *recovery_incident_;
    const bool expired = now < incident.progress || now - incident.started >= 600'000 || now - incident.progress >= 30'000
        || (incident.barrier && now - incident.fence_started >= options_.completeness_timeout_ms);
    if (expired && !incident.local_published && !incident.publication) {
        if (incident.voted && transport_.self() != incident.source) quarantine_live_recovery(RecoveryRefusal::stale_barrier);
        else stop_live_recovery(RecoveryRefusal::stale_barrier);
        return;
    }
    if (recovery_split_) { stop_live_recovery(RecoveryRefusal::divergent_epoch); return; }
    if (transport_.self() == incident.target && !incident.request_sent && transport_.link(incident.source) == net::LinkState::open) {
        const auto encoded = net::encode_galactic_recovery_request(incident.request);
        if (!encoded || !transport_.send(incident.source, encoded.value())) { stop_live_recovery(RecoveryRefusal::unavailable); return; }
        incident.request_sent = true;
    }
    if (incident.publication) {
        if (incident.publication->wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
        const auto refusal = incident.publication->get();
        if (refusal != RecoveryRefusal::none) { quarantine_live_recovery(refusal); return; }
        incident.publication.reset(); incident.local_published = true;
        incident.published.resize(options_.seats.size()); incident.published[transport_.self()] = true;
        auto published = *incident.expected; published.stage = RecoveryStage::published;
        if (!recovery_send_control(published, true)) { stop_live_recovery(RecoveryRefusal::unavailable); return; }
    }
    if (incident.local_published) {
        if (transport_.self() == incident.source) {
            for (std::size_t peer = 0; peer < options_.seats.size(); ++peer) {
                if (peer == incident.source) continue;
                if (transport_.link(static_cast<net::PeerIndex>(peer)) == net::LinkState::closed
                    || (expired && (incident.published.size() <= peer || !incident.published[peer])))
                    incident.removed = static_cast<std::uint8_t>(incident.removed | (1U << peer));
            }
        } else if (expired && !incident.release_seen) { quarantine_live_recovery(RecoveryRefusal::stale_barrier); return; }
        bool all = true;
        for (std::size_t peer = 0; peer < options_.seats.size(); ++peer)
            if (!(incident.removed & (1U << peer)) && (peer == incident.target || !incident.package->session_frontier->peers[peer].departed)
                && (incident.published.size() <= peer || !incident.published[peer])) all = false;
        if (all && transport_.self() == incident.source && !incident.release_seen) {
            auto release = *incident.expected; release.stage = RecoveryStage::release; release.removed = incident.removed;
            if (!recovery_send_control(release, true)) { stop_live_recovery(RecoveryRefusal::unavailable); return; }
            incident.release_seen = true;
        }
        if (incident.release_seen) {
            for (net::PeerIndex peer = 0; peer < transport_.peer_count(); ++peer)
                if ((incident.removed & (1U << peer)) && options_.quarantine_recovery_peer) options_.quarantine_recovery_peer(peer);
            if (incident.result) incident.result->set_value(RecoveryRefusal::none);
            recovery_incident_.reset();
        }
        return;
    }
    if (transport_.self() == incident.source || transport_.self() == incident.target) {
        RecoveryPair pair(transport_, incident.source, incident.target);
        if (!incident.transfer.pump(pair, now)) { stop_live_recovery(RecoveryRefusal::restore); return; }
    }
    if (transport_.self() == incident.target && incident.transfer.view().restored && !incident.initial_ready) {
        if (!incident.package || !incident.candidate) { stop_live_recovery(RecoveryRefusal::restore); return; }
        const auto encoded = net::encode_galactic_recovery_log(*incident.package);
        if (!encoded) { stop_live_recovery(RecoveryRefusal::malformed); return; }
        net::GalacticRecoveryControl staged{incident.epoch, 0, incident.source, incident.target,
            RecoveryStage::staged, incident.package->frontier, core::sha256(encoded.value())};
        if (!recovery_send_control(staged, false)) { stop_live_recovery(RecoveryRefusal::unavailable); return; }
        incident.initial_ready = true;
    }
    if (incident.barrier && transport_.self() != incident.target && !incident.final_log
        && adapter_->completed_tick() == *incident.barrier) {
        const auto refusal = recovery_snapshot(incident);
        if (refusal != RecoveryRefusal::none && refusal != RecoveryRefusal::busy) { stop_live_recovery(refusal); return; }
        if (refusal == RecoveryRefusal::none) {
            incident.final_log = true;
            if (transport_.self() == incident.source) {
                incident.expected = net::GalacticRecoveryControl{incident.epoch, *incident.barrier, incident.source, incident.target,
                    RecoveryStage::vote, incident.package->frontier, core::sha256(incident.sending)};
            }
        }
    }
    if (transport_.self() == incident.source && !incident.sending.empty() && (!incident.barrier || incident.final_log)) {
        // Only four bounded log chunks enter transport queues per owner call.
        for (unsigned count = 0; count < 4 && incident.sent < incident.sending.size(); ++count) {
            auto chunk = net::GalacticRecoveryStream::chunk(incident.epoch, incident.sending, incident.sent);
            if (!chunk || !transport_.send(incident.target, chunk.value())) { stop_live_recovery(RecoveryRefusal::unavailable); return; }
            incident.sent += std::min(net::save_chunk_bytes, incident.sending.size() - incident.sent);
        }
    }
    if (transport_.self() == incident.target && incident.barrier && incident.expected && !incident.final_log
        && incident.stream && incident.stream->complete()) {
        if (!incident.checkpoint || !incident.candidate || !incident.candidate->owner) { stop_live_recovery(RecoveryRefusal::restore); return; }
        auto package = net::decode_galactic_recovery_log(incident.stream->bytes(), incident.checkpoint);
        if (!package || package.value().frontier != incident.expected->frontier
            || core::sha256(incident.stream->bytes()) != incident.expected->sealed_log
            || package.value().checkpoint != incident.package->checkpoint) { stop_live_recovery(RecoveryRefusal::state_hash); return; }
        if (!incident.worker_started) { stop_live_recovery(RecoveryRefusal::restore); return; }
        net::GalacticRecoveryPackage delta = package.value();
        const auto from = incident.candidate->frontier;
        if (from.completed_tick < delta.checkpoint.completed_tick || from.completed_tick > delta.frontier.completed_tick) {
            stop_live_recovery(RecoveryRefusal::stale_barrier); return;
        }
        delta.ticks.erase(delta.ticks.begin(), delta.ticks.begin() + static_cast<std::ptrdiff_t>(from.completed_tick - delta.checkpoint.completed_tick));
        delta.checkpoint = from;
        auto owner = std::make_shared<std::unique_ptr<net::SessionAdapter>>(std::move(incident.candidate->owner));
        recovery_worker_frontier_ = delta.frontier;
        const auto refusal = recovery_worker_.start(std::move(delta), [owner](const auto&) mutable {
            return core::Result<std::unique_ptr<net::SessionAdapter>>::success(std::move(*owner));
        });
        if (refusal != RecoveryRefusal::none) { stop_live_recovery(refusal); return; }
        incident.candidate.reset(); incident.package = std::move(package).value(); incident.final_log = true;
    }
    if (transport_.self() == incident.target && incident.final_log && !incident.candidate) {
        auto candidate = recovery_worker_.take(recovery_worker_frontier_);
        if (candidate) {
            if (candidate->refusal != RecoveryRefusal::none) { stop_live_recovery(candidate->refusal); return; }
            incident.candidate = std::move(candidate); incident.progress = now;
        }
    }
    const bool can_vote = incident.final_log && incident.expected && incident.package
        && (transport_.self() != incident.target || (incident.candidate && incident.candidate->owner));
    if (can_vote && !incident.voted) {
        const auto encoded = net::encode_galactic_recovery_log(*incident.package);
        if (!encoded || core::sha256(encoded.value()) != incident.expected->sealed_log
            || incident.package->frontier != incident.expected->frontier) { stop_live_recovery(RecoveryRefusal::state_hash); return; }
        auto vote = *incident.expected; vote.stage = RecoveryStage::vote;
        incident.proofs.push_back({transport_.self(), vote.frontier, vote.sealed_log});
        // The first successful pair send can authorize a source commit even if
        // a later send fails. Latch uncertainty before attempting any send.
        incident.voted = true;
        if (!recovery_send_control(vote, true)) { stop_live_recovery(RecoveryRefusal::unavailable); return; }
        incident.progress = now;
    }
    if (!can_vote || !incident.voted || !incident.package->session_frontier) return;
    std::vector<bool> proved(options_.seats.size());
    for (const auto& proof : incident.proofs) {
        if (proof.peer >= proved.size() || proved[proof.peer] || proof.frontier != incident.expected->frontier
            || proof.sealed_log != incident.expected->sealed_log) { stop_live_recovery(RecoveryRefusal::state_hash); return; }
        proved[proof.peer] = true;
    }
    for (std::size_t peer = 0; peer < proved.size(); ++peer) {
        const bool active = peer == incident.target || !incident.package->session_frontier->peers[peer].departed;
        if (proved[peer] && !active) { stop_live_recovery(RecoveryRefusal::seat_binding); return; }
        if (active && !proved[peer]) return;
    }
    if (transport_.self() == incident.source && !incident.commit_sent) {
        auto commit = *incident.expected; commit.stage = RecoveryStage::commit;
        incident.commit_sent = incident.commit_seen = true;
        if (!recovery_send_control(commit, true)) recovery_notice(RecoveryRefusal::unavailable);
    }
    if (!incident.commit_seen) return;
    std::optional<net::SeatInput> rejoin;
    const auto& target = incident.package->session_frontier->peers[incident.target];
    if (target.departed) {
        if (target.producer_sequence && *target.producer_sequence == std::numeric_limits<std::uint64_t>::max()) {
            stop_live_recovery(RecoveryRefusal::limit); return;
        }
        sim::SeatCommandKey key{*incident.barrier, options_.seats[incident.target].seat,
            target.producer_sequence ? *target.producer_sequence + 1 : 0};
        auto bytes = net::encode_galactic_input(key, galactic::SeatRejoin{});
        if (!bytes) { stop_live_recovery(RecoveryRefusal::malformed); return; }
        rejoin = net::SeatInput{key, std::move(bytes).value()};
    }
    net::GalacticRecoveryWorker::Result candidate;
    const bool preserve = transport_.self() != incident.target;
    if (preserve) {
        candidate.frontier = incident.package->frontier;
        candidate.owner = std::make_unique<net::GalacticSessionAdapter>(context.game, context.executor,
            net::GalacticAdapterOptions{options_.clock_policy, true, net::GalacticDeparturePolicy::configured_fallback});
        auto* adapter = static_cast<net::GalacticSessionAdapter*>(candidate.owner.get());
        if (!adapter->restore_network_state(adapter_->checkpoint_network_state())) { stop_live_recovery(RecoveryRefusal::restore); return; }
    } else candidate = std::move(*incident.candidate);
    RecoveryPublication publication{*incident.package, std::move(candidate), incident.proofs, std::move(rejoin), {}, preserve};
    auto result = publication.result.get_future();
    bool busy = false;
    {
        const std::lock_guard lock(mutex_);
        if (recovery_publication_) busy = true;
        else recovery_publication_ = std::move(publication);
    }
    if (busy) { stop_live_recovery(RecoveryRefusal::busy); return; }
    incident.publication = std::move(result);
}
