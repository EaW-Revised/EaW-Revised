#include "pathfind_search_internal.hpp"

namespace eawr::sim::tactical::pathfind_detail {

[[nodiscard]] SearchScratch& search_scratch() {
    thread_local SearchScratch scratch;
    return scratch;
}

Finder::Finder(const MotionTable& table, const AvoidanceRules& rules, const MotionProfile& limits, const Footprint& footprint,
        const CollisionWorld& world, const EntityId entity, const Fixed now, const Vec3 start, const Fixed start_yaw,
        const Fixed start_speed, const Vec3 end, Work& work, Boxes& boxes, SearchScratch& scratch)
        : table_(table), rules_(rules), limits_(limits), footprint_(footprint), world_(world), entity_(entity),
          now_(now), start_(start), start_yaw_(start_yaw), start_speed_(start_speed), end_(end), work_(work),
          components_(scratch.components), boxes_(boxes), records_(scratch.records), open_(scratch.open) {
        layer_ = dynamic_layer_index(footprint.layer);
    }

Finder::Step Finder::begin(const Config& config, const Fixed weight, std::vector<PathNode>& nodes) {
        config_ = config;
        weight_ = weight;
        ++work_.stats.tries;
        initialize();
        if (!calc_.ok()) return Step::failed;
        const Signature start = signature({start_.x, start_.y}, clamp180(start_yaw_));
        end_signature_ = signature({end_.x, end_.y}, Fixed{});
        if (start.x == end_signature_.x && start.y == end_signature_.y) {
            // Make_Trivial_Path (MV-12).
            auto trivial = motion_detail::trivial_path(calc_, config_.max_speed, forward_speed_, now_, start_,
                start_yaw_, start_speed_, end_);
            if (!trivial) {
                nodes = {{now_, start_, clamp180(start_yaw_), start_speed_},
                    {calc_.add(now_, calc_.div(length_, config_.max_speed)), {end_.x, end_.y, start_.z},
                        calc_.atan2_deg(calc_.sub(end_.y, start_.y), calc_.sub(end_.x, start_.x)), Fixed{}}};
            } else {
                nodes = std::move(*trivial);
            }
            return calc_.ok() ? Step::found : Step::failed;
        }
        components_.clear();
        records_.clear();
        open_.clear();
        Component origin{{start_.x, start_.y}, clamp180(start_yaw_), now_, start_speed_, op_none, -1, {}};
        origin.distance = distance3(origin.position);
        components_.push_back(origin);
        open_.insert({0, Fixed{}, Fixed{}});
        records_.put(start, {false, Fixed{}});
        expansions_ = 0;
        last_op_ = config_.allow_emergency ? op_emergency_right : op_wait;
        return Step::running;
    }

Finder::Step Finder::resume(Fixed& farthest, std::vector<PathNode>& nodes, const std::uint64_t until) {
        while (!open_.empty() && expansions_ < config_.max_expansions && calc_.ok()) {
            if (work_.stats.expansions >= until) return Step::running;
            auto mark = work_.now();
            const Header header = open_.extract();
            const Component parent = components_[static_cast<std::size_t>(header.component)];
            work_.stats.set_ticks += work_.now() - mark;
            const Signature here = signature(parent.position, parent.facing);
            if (reached(here, parent.op)) {
                build_final(header.component, nodes);
                return calc_.ok() ? Step::found : Step::failed;
            }
            mark = work_.now();
            if (Record* const found = records_.find(here); found != nullptr) found->closed = true;
            work_.stats.set_ticks += work_.now() - mark;
            const auto h = heading_of(parent.facing);
            for (int op = op_forward; op <= last_op_ && calc_.ok(); ++op) {
                Component child;
                if (!expand(op, parent, h, child)) continue;
                ++work_.stats.children;
                const Signature cell = signature(child.position, child.facing);
                mark = work_.now();
                Record* const record = records_.find(cell);
                work_.stats.set_ticks += work_.now() - mark;
                if (record != nullptr && record->closed && culls(op)) continue;
                // the remaining-estimate distance (AV-12), which an arc's cost also reads (#520: once).
                child.distance = distance3(child.position);
                LinearQuery query;
                query.start = parent.position;
                query.start_frame = parent.frame;
                query.end = child.position;
                query.end_frame = child.frame;
                query.x_extent = config_.x_extent;
                query.y_extent = config_.y_extent;
                query.ignore = entity_;
                // The query's facing, computed when an exact test first needs it (#520).
                std::optional<Vec2> facing;
                const auto query_facing = [&]() -> const Vec2& {
                    if (!facing) {
                        facing = query.start != query.end ? unit(calc_, minus(calc_, query.end, query.start))
                                                          : Vec2{heading_of(child.facing).x, heading_of(child.facing).y};
                    }
                    return *facing;
                };
                Fixed cost = calc_.sub(child.frame, parent.frame);
                if (!linear_cost(cost, op, query, parent.velocity, child.distance, query_facing)) continue;
                const Fixed path_cost = calc_.add(cost, header.path_cost);
                const bool counts = !(config_.only_finish_on_end && cell.x == end_signature_.x && cell.y == end_signature_.y);
                if (record != nullptr) {
                    if (record->cost <= path_cost && culls(op)) continue;
                    if (counts) record->cost = path_cost;
                }
                // The remaining estimate (AV-12): straight distance over the maximum speed.
                const Fixed estimate = calc_.div(child.distance, config_.max_speed);
                const Fixed reach_distance = calc_.mul(calc_.sub(child.frame, now_), config_.max_speed);
                if (reach_distance > farthest) farthest = reach_distance;
                const Fixed w = config_.distance_cutoff <= reach_distance ? whole(1) : weight_;
                child.op = op;
                child.previous = header.component;
                const Fixed total = calc_.add(path_cost, calc_.mul(calc_.mul(w, config_.estimate_weight), estimate));
                mark = work_.now();
                components_.push_back(child);
                open_.insert({static_cast<int>(components_.size() - 1), path_cost, total});
                if (record == nullptr && counts) records_.put(cell, {false, path_cost});
                work_.stats.set_ticks += work_.now() - mark;
                ++expansions_;
                ++work_.stats.expansions;
            }
            if (last_op_ == op_emergency_right) last_op_ = op_wait; // emergency turns only from the start
        }
        return Step::failed;
    }

void Finder::initialize() {
        const Fixed rotations = calc_.div(rules_.max_rotations, config_.rotation_coefficient);
        const Fixed vmax = config_.max_speed;
        const Fixed rot = config_.rate_of_turn;
        length_ = calc_.length(calc_.sub(end_.x, start_.x), calc_.sub(end_.y, start_.y));
        Fixed step = calc_.mul(table_.rules.expansion_distance, config_.forward_coefficient);
        if (length_ < calc_.mul(whole(2), step)) {
            step = std::max(motion_detail::min_expansion_distance, calc_.div(length_, whole(2)));
        }
        const Fixed frames_forward = std::max(whole(1), calc_.div(step, vmax));
        const Fixed frames_turn = calc_.div(calc_.div(whole(360), rotations), rot);
        theta_ = calc_.mul(frames_turn, rot);
        while (theta_ >= whole(360)) theta_ = calc_.sub(theta_, whole(360));
        forward_speed_ = calc_.mul(frames_forward, vmax);
        turn_speed_ = calc_.mul(frames_turn, vmax);
        signature_scale_ = calc_.div(signature_cells, forward_speed_);
        angle_scale_ = calc_.div(calc_.add(rotations, whole(1)), whole(360));
        turn_radius_ = calc_.mul(calc_.div(whole(360), theta_), calc_.div(turn_speed_, motion_detail::two_pi));
        const Fixed half_theta = calc_.div(theta_, whole(2));
        const Fixed chord = calc_.mul(calc_.mul(turn_radius_, whole(2)), calc_.sin_deg(half_theta));
        x_advance_ = calc_.mul(chord, calc_.cos_deg(half_theta));
        y_advance_ = calc_.abs(calc_.mul(chord, calc_.sin_deg(half_theta)));
        slow_speed_ = calc_.mul(vmax, rules_.wait_speed);
        emergency_left_ = Fixed{};
        emergency_right_ = Fixed{};
        if (!layer_ || !calc_.ok()) return;
        // Rays of the turn radius plus the distance to reach full speed, 10 degrees apart.
        const Fixed ray = calc_.add(turn_radius_, calc_.div(calc_.abs(calc_.sub(calc_.mul(vmax, vmax),
            calc_.mul(start_speed_, start_speed_))), calc_.mul(whole(2), limits_.acceleration)));
        const Fixed ray_frames = calc_.div(ray, vmax);
        const auto probe = [&](const Fixed direction) {
            for (Fixed angle{}; calc_.abs(angle) < probe_limit && calc_.ok();
                angle = direction.raw() > 0 ? calc_.add(angle, probe_step) : calc_.sub(angle, probe_step)) {
                const auto h = heading(calc_, calc_.add(start_yaw_, angle));
                LinearQuery query;
                query.start = {start_.x, start_.y};
                query.end = {calc_.add(start_.x, calc_.mul(h.x, ray)), calc_.add(start_.y, calc_.mul(h.y, ray))};
                query.facing = {h.x, h.y};
                query.start_frame = calc_.add(now_, calc_.div(calc_.abs(angle), rot));
                query.end_frame = calc_.add(query.start_frame, ray_frames);
                query.x_extent = footprint_.x_extent;
                query.y_extent = footprint_.y_extent;
                query.ignore = entity_;
                if ((dual_layer(query, facing_of(query)) & config_.mask) == 0) return angle;
            }
            return Fixed{};
        };
        emergency_left_ = probe(whole(1));
        emergency_right_ = probe(whole(-1));
    }

[[nodiscard]] bool Finder::reached(const Signature& here, const int op) const noexcept {
        if (config_.only_finish_on_end && op != op_end) return false;
        return here.x == end_signature_.x && here.y == end_signature_.y;
    }

void Finder::frame_and_velocity(const Fixed frame, const Fixed v, const Fixed distance, const int op, Component& child) {
        const Fixed target = is_slow(op) ? slow_speed_ : config_.max_speed;
        if (v == target) {
            child.frame = calc_.add(frame, calc_.div(distance, v));
            child.velocity = v;
            return;
        }
        const Fixed rate = target < v ? limits_.deceleration : limits_.acceleration;
        const Fixed change = calc_.div(calc_.abs(calc_.sub(calc_.mul(v, v), calc_.mul(target, target))),
            calc_.mul(whole(2), rate));
        if (distance <= change) {
            const Fixed delta = calc_.sqrt(calc_.mul(calc_.mul(whole(2), rate), distance));
            child.velocity = target <= v ? calc_.sub(v, delta) : calc_.add(v, delta);
            child.frame = calc_.add(frame, calc_.div(calc_.abs(calc_.sub(child.velocity, v)), rate));
        } else {
            child.velocity = target;
            child.frame = calc_.add(calc_.add(frame, calc_.div(calc_.abs(calc_.sub(target, v)), rate)),
                calc_.div(calc_.sub(distance, change), std::max(v, target)));
        }
    }

bool Finder::expand(const int op, const Component& parent, const motion_detail::Heading& h, Component& child) {
        if (is_wait(op) && !config_.allow_wait) return false;
        const Fixed v = parent.velocity;
        if (!is_emergency(op)) {
            const bool at_full = calc_.abs(calc_.sub(v, config_.max_speed)) < limits_.acceleration;
            const bool at_slow = calc_.abs(calc_.sub(v, slow_speed_)) < limits_.acceleration;
            if (!at_full && !is_slow(op) && op != op_full_speed) return false;
            if (!at_slow && is_slow(op) && op != op_slow_speed && !is_unbounded(op)) return false;
            if (parent.op == op_slow_speed && op != op_wait) return false;
            if (parent.op == op_wait && op != op_wait && op != op_full_speed) return false;
            if (parent.op == op_none && !at_full && !at_slow && op != op_full_speed && op != op_slow_speed) return false;
        }
        const Vec2 forward{h.x, h.y};
        const Vec2 left{calc_.neg(h.y), h.x};
        child.facing = parent.facing;
        child.frame = parent.frame;
        child.velocity = v;
        const auto along = [&](const Fixed distance) {
            return Vec2{calc_.add(parent.position.x, calc_.mul(forward.x, distance)),
                calc_.add(parent.position.y, calc_.mul(forward.y, distance))};
        };
        switch (op) {
        case op_forward: {
            const Fixed distance = std::min(parent.distance, forward_speed_);
            // Project (AV-U7): on the target a forward step has no length and no cost, and in
            // Q24 it would repeat until the expansion limit; FoC's binary32 steps do not stay on
            // the target (the S-10 recording finds its detour), so the remake drops it.
            if (distance.raw() == 0) return false;
            child.position = along(distance);
            frame_and_velocity(parent.frame, v, distance, op, child);
            return true;
        }
        case op_match:
            return match(parent, h, child);
        case op_left:
        case op_right: {
            const Fixed side = op == op_left ? y_advance_ : calc_.neg(y_advance_);
            child.position = {calc_.add(calc_.add(parent.position.x, calc_.mul(forward.x, x_advance_)), calc_.mul(left.x, side)),
                calc_.add(calc_.add(parent.position.y, calc_.mul(forward.y, x_advance_)), calc_.mul(left.y, side))};
            child.facing = clamp180(op == op_left ? calc_.add(parent.facing, theta_) : calc_.sub(parent.facing, theta_));
            frame_and_velocity(parent.frame, v, turn_speed_, op, child);
            return true;
        }
        case op_end:
            if (parent.op != op_match) return false;
            child.position = {end_.x, end_.y};
            frame_and_velocity(parent.frame, v, parent.distance, op, child);
            return true;
        case op_full_speed:
        case op_slow_speed: {
            const Fixed target = op == op_slow_speed ? slow_speed_ : config_.max_speed;
            if (v == target) return false;
            const Fixed rate = target <= v ? limits_.deceleration : limits_.acceleration;
            const Fixed distance = calc_.div(calc_.abs(calc_.sub(calc_.mul(v, v), calc_.mul(target, target))),
                calc_.mul(whole(2), rate));
            child.position = along(distance);
            child.frame = calc_.add(parent.frame, calc_.div(calc_.abs(calc_.sub(target, v)), rate));
            child.velocity = target;
            return true;
        }
        case op_wait: {
            const Fixed frames = calc_.div(rules_.wait_frames, config_.max_speed);
            const Fixed distance = calc_.mul(calc_.mul(frames, config_.max_speed), rules_.wait_speed);
            child.position = along(distance);
            frame_and_velocity(parent.frame, v, distance, op, child);
            return true;
        }
        case op_emergency_left:
        case op_emergency_right: {
            const Fixed angle = op == op_emergency_left ? emergency_left_ : emergency_right_;
            if (angle.raw() == 0) return false;
            child.position = parent.position;
            child.facing = clamp180(calc_.add(parent.facing, angle));
            child.frame = calc_.add(parent.frame, calc_.div(calc_.abs(angle), config_.rate_of_turn));
            child.velocity = Fixed{};
            return true;
        }
        default:
            return false;
        }
    }

bool Finder::match(const Component& parent, const motion_detail::Heading& h, Component& child) {
        // A node on the target has no bearing to turn to (FoC's facing of a zero vector).
        if (parent.position.x == end_.x && parent.position.y == end_.y) return false;
        const Fixed bearing = calc_.atan2_deg(calc_.sub(end_.y, parent.position.y), calc_.sub(end_.x, parent.position.x));
        const Fixed difference = calc_.abs(clamp180(calc_.sub(bearing, parent.facing)));
        if (difference > theta_ || difference < motion_detail::aligned_degrees) return false;
        const Vec2 toward = unit(calc_, {calc_.sub(end_.x, parent.position.x), calc_.sub(end_.y, parent.position.y)});
        const bool left_turn = calc_.sub(calc_.mul(h.x, toward.y), calc_.mul(h.y, toward.x)).raw() >= 0;
        const Fixed side = left_turn ? turn_radius_ : calc_.neg(turn_radius_);
        const Vec2 centre{calc_.add(parent.position.x, calc_.mul(calc_.neg(h.y), side)),
            calc_.add(parent.position.y, calc_.mul(h.x, side))};
        const Vec2 back = minus(calc_, centre, {end_.x, end_.y});
        const Fixed distance = calc_.length(back.x, back.y);
        if (!(turn_radius_ < distance)) return false;
        // Rotate the target-to-centre direction by asin(R / d) toward the turn.
        const Fixed tangent = calc_.sqrt(calc_.sub(calc_.mul(distance, distance), calc_.mul(turn_radius_, turn_radius_)));
        const Fixed opening = calc_.sub(whole(90), calc_.atan2_deg(tangent, turn_radius_));
        const Fixed rotate = std::clamp(opening, Fixed{}, whole(90));
        const Fixed base = calc_.atan2_deg(back.y, back.x);
        const Fixed direction = left_turn ? calc_.add(base, rotate) : calc_.sub(base, rotate);
        const auto d = heading_of(direction);
        child.position = {calc_.add(end_.x, calc_.mul(d.x, tangent)), calc_.add(end_.y, calc_.mul(d.y, tangent))};
        child.facing = clamp180(calc_.add(direction, whole(180)));
        const Fixed turn = calc_.abs(clamp180(calc_.sub(child.facing, parent.facing)));
        const Fixed arc = calc_.div(calc_.mul(config_.max_speed, turn), config_.rate_of_turn);
        frame_and_velocity(parent.frame, parent.velocity, arc, op_match, child);
        return true;
    }

void Finder::build_final(const int last, std::vector<PathNode>& nodes) {
        std::vector<int> chain;
        for (int index = last; index >= 0; index = components_[static_cast<std::size_t>(index)].previous) {
            chain.push_back(index);
        }
        std::reverse(chain.begin(), chain.end());
        nodes.clear();
        for (const int index : chain) {
            const auto& component = components_[static_cast<std::size_t>(index)];
            nodes.push_back({component.frame, {component.position.x, component.position.y, start_.z},
                clamp180(component.facing), component.velocity});
            if (is_emergency(component.op) && nodes.size() >= 2) nodes[nodes.size() - 2].speed = Fixed{};
        }
        MotionProfile profile = limits_;
        motion_detail::finish_path(calc_, profile, theta_, nodes);
    }
// The formation centre path's maximum speed reduction for short moves (research
// E71-22): the planning speed drops until the target lies outside both turn circles (or ahead
// and at least a turn diameter away) and the ship can brake plus turn before it.
[[nodiscard]] Fixed reduced_speed(Calc& calc, const MotionProfile& limits, const Vec3 position, const Fixed yaw,
    const Fixed speed, const Vec3 target) {
    const Fixed original = limits.max_speed;
    const Fixed rot_turns = calc.div(limits.rate_of_turn, degrees_per_radian_turn); // turns per frame
    const Fixed rot_radians = calc.mul(rot_turns, motion_detail::two_pi);
    const auto h = heading(calc, yaw);
    const Vec2 target_xy{target.x, target.y};
    const Fixed to_target = calc.length(calc.sub(position.x, target.x), calc.sub(position.y, target.y));
    Fixed v = original;
    for (int round = 0; round < 64 && calc.ok(); ++round) {
        Fixed candidate = v;
        const Fixed radius = calc.div(v, rot_radians);
        Vec2 point{position.x, position.y};
        if (speed < v && limits.acceleration.raw() > 0) {
            const Fixed gain = calc.div(calc.abs(calc.sub(calc.mul(speed, speed), calc.mul(v, v))),
                calc.mul(whole(2), limits.acceleration));
            point = {calc.add(point.x, calc.mul(h.x, gain)), calc.add(point.y, calc.mul(h.y, gain))};
        }
        const Vec2 left{calc.mul(calc.neg(h.y), radius), calc.mul(h.x, radius)};
        const Fixed to_left = calc.length(calc.sub(target_xy.x, calc.add(point.x, left.x)),
            calc.sub(target_xy.y, calc.add(point.y, left.y)));
        const Fixed to_right = calc.length(calc.sub(target_xy.x, calc.sub(point.x, left.x)),
            calc.sub(target_xy.y, calc.sub(point.y, left.y)));
        const Fixed distance = calc.length(calc.sub(target_xy.x, point.x), calc.sub(target_xy.y, point.y));
        const bool behind = calc.dot(calc.sub(target_xy.x, point.x), calc.sub(target_xy.y, point.y), h.x, h.y).raw() < 0;
        const Fixed diameter = calc.mul(whole(2), radius);
        if (!(radius <= to_left && radius <= to_right && (!behind || diameter <= distance))) {
            const Fixed factor = std::max(tenth, calc.div(calc.mul(distance, nine_tenths), diameter));
            candidate = std::min(calc.mul(factor, v), candidate);
            candidate = std::max(candidate, calc.mul(original, tenth));
        }
        if (limits.acceleration.raw() > 0) {
            // FoC reads the acceleration here, not the deceleration.
            Fixed braking_speed = v;
            Fixed needed = calc.add(calc.div(calc.mul(v, v), limits.acceleration), radius);
            while (to_target < needed && calc.ok()) {
                braking_speed = calc.mul(braking_speed, nine_tenths);
                if (!(calc.mul(v, tenth) <= braking_speed)) break;
                needed = calc.add(calc.div(calc.mul(braking_speed, braking_speed), limits.acceleration),
                    calc.div(braking_speed, rot_radians));
            }
            candidate = std::min(braking_speed, candidate);
        }
        if (!(calc.abs(calc.sub(candidate, v)) > speed_settle)) break;
        v = candidate;
    }
    return v;
}


} // namespace eawr::sim::tactical::pathfind_detail
