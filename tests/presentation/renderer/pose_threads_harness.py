"""Engine-free stand-ins for executing the production pose/visibility call sites."""

HARNESS = r'''
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>
const auto caller = std::this_thread::get_id();
namespace core {
struct Error { std::string text; };
std::string format_diagnostic(const Error& error) { return error.text; }
template<class T> struct Result {
    std::optional<T> data;
    explicit operator bool() const { return data.has_value(); }
    T& value() { return *data; }
    Error error() const { return {"sample failed"}; }
};
}
namespace sim {
using EntityId = std::uint64_t; using AssetId = std::uint64_t;
struct RenderInstance {
    EntityId entity_id{}; AssetId asset_id{};
    friend bool operator==(const RenderInstance&, const RenderInstance&) = default;
};
namespace math {
struct Fixed { int value{}; static constexpr int scale = 256; int raw() const { return value; } };
}
}
namespace animation {
using Matrix = std::array<float,16>;
struct BonePose { Matrix skin_asset{}, model_asset{}; bool visible = true; };
struct Pose { std::vector<BonePose> bones; };
struct Player {
    mutable std::atomic<unsigned> calls{};
    bool fail_first{};
    static Matrix identity_matrix() { Matrix m{}; m[0]=m[5]=m[10]=m[15]=1; return m; }
    static Matrix multiply(const Matrix& a, const Matrix& b) {
        Matrix result{};
        for (unsigned i=0;i<16;++i) result[i]=a[i]+b[i];
        return result;
    }
    bool apply_bone_transform(Pose& pose, std::size_t bone, const Matrix& matrix) const {
        if (bone >= pose.bones.size()) return false;
        pose.bones[bone].model_asset=multiply(pose.bones[bone].model_asset,matrix);
        pose.bones[bone].skin_asset=pose.bones[bone].model_asset;
        return true;
    }
};
core::Result<Pose> sample_idle(const Player& player, int, unsigned start, std::uint64_t tick, unsigned) {
    if (++player.calls == 1 && player.fail_first) return {};
    Pose pose; pose.bones.resize(3);
    for (unsigned i=0;i<3;++i) {
        auto& b=pose.bones[i]; b.model_asset=b.skin_asset=Player::identity_matrix();
        b.model_asset[12]=b.skin_asset[12]=static_cast<float>(tick+start+i);
        b.model_asset[1]=b.skin_asset[1]=-0.0F; b.visible=i!=2;
    }
    return {std::move(pose)};
}
core::Result<int> sample_idle(const Player& player, int playback, unsigned start, std::uint64_t tick,
    unsigned rate, Pose& output) {
    auto pose=sample_idle(player,playback,start,tick,rate);
    if (!pose) return {};
    output=std::move(pose.value()); return {0};
}
}
namespace particles {
constexpr unsigned map_owner_ticks_per_second=30;
class StepExecutor {
public:
    virtual ~StepExecutor()=default;
    virtual bool run(std::size_t,const std::function<void(std::size_t)>&) const=0;
};
}
struct Executor : particles::StepExecutor {
    unsigned count; bool fail_partial{}; mutable unsigned runs{};
    explicit Executor(unsigned n) : count(n) {}
    bool run(std::size_t size,const std::function<void(std::size_t)>& task) const override {
        ++runs;
        if (fail_partial) { if(size) task(0); return false; }
        std::vector<std::thread> jobs;
        for(unsigned w=0;w<count;++w) jobs.emplace_back([&,w] {
            for(std::size_t i=w;i<size;i+=count) { std::this_thread::yield(); task(i); }
        });
        for(auto& job:jobs) job.join();
        return true;
    }
};
namespace space {
float environment_effect_time(std::uint64_t tick) { return static_cast<float>(tick); }
template<class Visible>
__VISIBILITY__
}
struct SkinPoseWork { unsigned validations{}, builds{}; double validation_ms{},build_ms{}; };
struct FrameTimer { explicit FrameTimer(double*) {} void finish() {} };
struct PerfTrace { static bool fine_particle_timings() { return true; } };
std::vector<std::uint32_t> bytes(std::span<const animation::BonePose> bones) {
    std::vector<std::uint32_t> result;
    for(const auto& bone:bones) {
        for(float value:bone.skin_asset) result.push_back(std::bit_cast<std::uint32_t>(value));
        for(float value:bone.model_asset) result.push_back(std::bit_cast<std::uint32_t>(value));
        result.push_back(bone.visible);
    }
    return result;
}
struct GodotRenderer {
    struct PreparedSkinPose {
        std::shared_ptr<const std::vector<animation::BonePose>> bones;
        explicit operator bool() const { return bool(bones); }
    };
    struct Upload {
        sim::EntityId entity; sim::AssetId asset; std::vector<std::uint32_t> bones;
        friend bool operator==(const Upload&,const Upload&)=default;
    };
    static inline std::atomic<unsigned> builds{};
    std::vector<Upload> uploads;
    static PreparedSkinPose prepare_skin_pose(std::span<const animation::BonePose> bones,SkinPoseWork* work=nullptr) {
        ++builds;
        if(work) { ++work->validations; ++work->builds; }
        return {std::make_shared<const std::vector<animation::BonePose>>(bones.begin(),bones.end())};
    }
    core::Result<int> set_prepared_skin_pose(sim::EntityId entity,sim::AssetId asset,const PreparedSkinPose& pose) {
        assert(std::this_thread::get_id()==caller && pose);
        uploads.push_back({entity,asset,bytes(*pose.bones)}); return {0};
    }
    core::Result<int> set_skin_pose(sim::EntityId entity,sim::AssetId asset,
        std::span<const animation::BonePose> bones) {
        assert(std::this_thread::get_id()==caller);
        uploads.push_back({entity,asset,bytes(bones)}); return {0};
    }
    core::Result<int> set_material_scalar(sim::AssetId,const char*,float) {
        assert(std::this_thread::get_id()==caller); return {0};
    }
    void begin_skin_batch(bool) { assert(std::this_thread::get_id()==caller); }
    SkinPoseWork end_skin_batch() { return {}; }
};
namespace scene {
enum class HardpointState { intact,damaged,destroyed };
struct HardpointArt { bool attached_model,damage_decal; };
HardpointArt hardpoint_art(HardpointState state) { return {state!=HardpointState::destroyed,state==HardpointState::damaged}; }
}
struct SpacePopulation {
    struct LivePose {
        struct TurretPose { sim::math::Fixed yaw{},pitch{}; bool barrel{},xy_only{}; };
        struct Hardpoint { std::uint32_t hardpoint{}; TurretPose angles; };
        std::size_t ship{}; std::optional<TurretPose> turret; std::vector<Hardpoint> hardpoint_turrets;
    };
    struct LiveClipPose { std::size_t ship{}; };
    struct IdlePlacement { std::size_t clip{}; int playback{}; unsigned start_frame{}; };
    struct AnimatedInstance { sim::EntityId entity{}; sim::AssetId asset{}; std::size_t idle{}; };
    struct TurretBinding {
        std::shared_ptr<const animation::Player> player;
        animation::Pose pose; std::vector<animation::BonePose> bind_bones;
        std::size_t turret{}; std::optional<std::size_t> barrel,idle;
        std::optional<LivePose::TurretPose> applied; bool parent_model{};
    };
    struct HardpointGate {
        enum class Art { hull,attached_model,damage_decal };
        std::size_t decision{},hardpoint{}; Art art=Art::hull;
    };
    struct Piece {
        sim::RenderInstance instance;
        std::optional<std::size_t> live;
        std::optional<bool> alternate,blast_charged;
        bool shield{},cloak{},cloak_hidden{};
        std::optional<HardpointGate> gate;
    };
    using GatedInstance = Piece;
    struct Index {
        std::vector<std::vector<std::size_t>> rows;
        std::span<const std::size_t> pieces(std::size_t ship) const {
            // Production queries increment shared counters; they must stay on the caller.
            assert(std::this_thread::get_id()==caller);
            if(ship>=rows.size()) return {};
            return rows[ship];
        }
    } piece_index_;
    struct Marks {
        std::set<std::pair<std::size_t,std::size_t>> values;
        void clear() { values.clear(); }
        void mark(std::size_t a,std::size_t b) { values.emplace(a,b); }
        std::size_t count() const { return values.size(); }
    } attachment_marks_;
    struct Options { bool live_clock=true; unsigned animation_frames=10,idle_offset=3; } options_;
    bool trace_frames_=true;
    double idle_frame_ms_{},idle_sample_ms_{},idle_upload_ms_{},refresh_ms_{};
    SkinPoseWork idle_skin_work_;
    std::optional<unsigned> animation_sample_,effect_sample_,last_unit_sample_;
    std::vector<sim::AssetId> effect_clock_assets_;
    std::vector<IdlePlacement> idle_placements_;
    // Caller-owned scratch; workers write disjoint rows and publication stays ordered.
    struct PreparedIdle {
        std::optional<animation::Pose> pose;
        GodotRenderer::PreparedSkinPose palette;
        SkinPoseWork work;
        bool failed{};
    };
    struct PreparedTurretBinding {
        sim::EntityId entity;
        sim::AssetId asset;
        GodotRenderer::PreparedSkinPose palette;
    };
    std::vector<PreparedIdle> prepared_idle_;
    std::vector<std::uint8_t> idle_consumed_;
    std::vector<std::vector<PreparedTurretBinding>> turret_staged_;
    std::vector<std::span<const std::size_t>> turret_piece_rows_;
    std::vector<std::string> turret_errors_;
    std::vector<std::uint8_t> turret_succeeded_, turret_ships_;
    std::vector<std::size_t> active_clips_;

    std::vector<std::shared_ptr<const animation::Player>> clips_;
    std::map<std::size_t,std::size_t> idle_live_ships_;
    std::vector<bool> live_retired_;
    std::vector<AnimatedInstance> animated_instances_;
    std::vector<std::vector<animation::BonePose>> contact_bones_;
    std::vector<std::string> upload_failures_;
    std::uint64_t idle_sample_failures_{};
    std::vector<std::optional<TurretBinding>> turret_bindings_,parent_hardpoint_turret_bindings_;
    std::vector<std::map<std::uint32_t,TurretBinding>> hardpoint_turret_bindings_;
    std::vector<Piece> pieces_;
    std::string failure_;
    std::vector<sim::RenderInstance> instances_;
    std::vector<std::uint8_t> piece_visibility_, piece_counts_;
    std::vector<bool> live_shown_,live_alternates_,live_defend_,live_stealth_,live_blast_charged_,live_blast_charging_;
    std::vector<std::vector<scene::HardpointState>> hardpoint_states_;
    std::map<std::string,std::uint64_t> blast_meshes_shown_;
    std::uint64_t instance_count_{},attachments_drawn_{},shield_shells_shown_{},stealth_shells_shown_{};
    void pose_units(GodotRenderer&,std::uint32_t,const particles::StepExecutor*);
    bool pose_live_turrets(GodotRenderer&,std::span<const LivePose>,std::span<const LiveClipPose>,const particles::StepExecutor*);
    void refresh_instances(const particles::StepExecutor*);
};
__FUNCTIONS__

SpacePopulation idle_population(bool fail_first=false) {
    SpacePopulation p; auto player=std::make_shared<animation::Player>(); player->fail_first=fail_first;
    p.clips_.push_back(player); p.live_retired_={false,false,true,false}; p.contact_bones_.resize(4);
    for(unsigned i=0;i<4;++i) { p.idle_placements_.push_back({0,0,i}); p.idle_live_ships_[i]=i; }
    p.animated_instances_={{20,40,0},{21,41,0},{22,42,2}}; p.effect_clock_assets_={90};
    return p;
}
SpacePopulation::TurretBinding binding() {
    SpacePopulation::TurretBinding b; b.player=std::make_shared<animation::Player>();
    b.pose.bones.resize(3);
    for(auto& bone:b.pose.bones) bone.skin_asset=bone.model_asset=animation::Player::identity_matrix();
    b.pose.bones[2].visible=false; b.bind_bones=b.pose.bones; b.barrel=1;
    return b;
}
SpacePopulation turret_population() {
    SpacePopulation p; p.turret_bindings_.resize(3); p.parent_hardpoint_turret_bindings_.resize(3);
    p.hardpoint_turret_bindings_.resize(3); p.contact_bones_.resize(3); p.piece_index_.rows.resize(3);
    for(unsigned ship=0;ship<3;++ship) {
        p.turret_bindings_[ship]=binding(); p.contact_bones_[ship]=p.turret_bindings_[ship]->bind_bones;
        SpacePopulation::Piece hull; hull.instance={10+ship*10,100+ship}; hull.live=ship;
        if(ship==2) hull.shield=true; // no ordinary consumer for this ship
        p.piece_index_.rows[ship].push_back(p.pieces_.size()); p.pieces_.push_back(hull);
    }
    SpacePopulation::Piece attachment; attachment.instance={11,201}; attachment.live=0;
    attachment.gate=SpacePopulation::HardpointGate{0,7,SpacePopulation::HardpointGate::Art::attached_model};
    p.piece_index_.rows[0].push_back(p.pieces_.size()); p.pieces_.push_back(attachment);
    p.hardpoint_turret_bindings_[0][7]=binding();
    auto parent=binding(); parent.parent_model=true;
    p.hardpoint_turret_bindings_[0][8]=parent; p.parent_hardpoint_turret_bindings_[0]=binding();
    p.hardpoint_turret_bindings_[1][9]=binding(); // requested, but has no matching piece
    return p;
}
std::vector<SpacePopulation::LivePose> requests() {
    using Angles=SpacePopulation::LivePose::TurretPose;
    const Angles a{{1024},{512},true,false};
    return {{0,a,{{7,a},{8,a}}},{1,a,{{9,a}}},{2,a,{}}};
}
SpacePopulation visibility_population() {
    SpacePopulation p;
    p.live_shown_={true,true,false}; p.live_alternates_={false,true,false};
    p.live_defend_={true,false,false}; p.live_stealth_={false,true,false};
    p.live_blast_charged_={true,false,false}; p.live_blast_charging_={false,true,false};
    p.hardpoint_states_={{scene::HardpointState::intact,scene::HardpointState::destroyed,scene::HardpointState::damaged}};
    for(unsigned i=0;i<513;++i) {
        SpacePopulation::Piece piece; piece.instance={i+1,i+1000}; piece.live=i%3;
        if(i%11==0) piece.shield=true;
        if(i%13==0) piece.cloak=true;
        if(i%7==0) piece.cloak_hidden=true;
        if(i%17==0) piece.blast_charged=i%2==0;
        if(i%19==0) piece.alternate=i%2==0;
        if(i%5==0) piece.gate=SpacePopulation::HardpointGate{0,i%3,SpacePopulation::HardpointGate::Art::attached_model};
        p.pieces_.push_back(piece);
    }
    return p;
}
void same_contacts(const SpacePopulation& a,const SpacePopulation& b) {
    assert(a.contact_bones_.size()==b.contact_bones_.size());
    for(std::size_t i=0;i<a.contact_bones_.size();++i) assert(bytes(a.contact_bones_[i])==bytes(b.contact_bones_[i]));
}
void same_visibility(const SpacePopulation& a,const SpacePopulation& b) {
    assert(a.instances_==b.instances_);
    assert(a.instance_count_==b.instance_count_ && a.attachments_drawn_==b.attachments_drawn_);
    assert(a.shield_shells_shown_==b.shield_shells_shown_ && a.stealth_shells_shown_==b.stealth_shells_shown_);
    assert(a.blast_meshes_shown_==b.blast_meshes_shown_ && a.attachment_marks_.values==b.attachment_marks_.values);
}
int main() {
    auto idle=idle_population(); GodotRenderer idle_renderer; idle.pose_units(idle_renderer,12,nullptr);
    auto turret=turret_population(); GodotRenderer turret_renderer; const auto poses=requests();
    assert(turret.pose_live_turrets(turret_renderer,poses,{},nullptr));
    assert(turret_renderer.uploads.size()==4);
    assert(turret_renderer.uploads[0].entity==10 && turret_renderer.uploads[1].entity==20
        && turret_renderer.uploads[2].entity==11 && turret_renderer.uploads[3].entity==10);
    auto visibility=visibility_population(); visibility.refresh_instances(nullptr);
    assert(GodotRenderer::builds==0 && visibility.piece_visibility_.empty());
    for(unsigned workers:{1,2,4,8}) {
        Executor executor(workers); auto p=idle_population(); GodotRenderer renderer;
        GodotRenderer::builds=0; p.pose_units(renderer,12,&executor);
        assert(renderer.uploads==idle_renderer.uploads && GodotRenderer::builds==1);
        same_contacts(p,idle); assert(p.idle_sample_failures_==0 && p.upload_failures_.empty());
        assert(p.clips_[0]->calls==3); // unused palette row still samples for contacts
        const auto runs=executor.runs; p.pose_units(renderer,12,&executor); assert(executor.runs==runs);
        Executor failed(workers); failed.fail_partial=true;
        auto retry=idle_population(true); GodotRenderer retry_renderer;
        retry.pose_units(retry_renderer,12,&failed);
        assert(retry.idle_sample_failures_==0 && retry_renderer.uploads==idle_renderer.uploads);
        same_contacts(retry,idle); // resets failed preparation before serial rebuild

        auto t=turret_population(); GodotRenderer r; GodotRenderer::builds=0;
        assert(t.pose_live_turrets(r,poses,{},&executor));
        assert(r.uploads==turret_renderer.uploads && GodotRenderer::builds==4); same_contacts(t,turret);
        assert(t.turret_piece_rows_.empty());
        auto duplicate=poses; duplicate.push_back(poses[0]);
        auto expected=turret_population(),actual=turret_population(); GodotRenderer er,ar;
        assert(expected.pose_live_turrets(er,duplicate,{},nullptr));
        const auto before=executor.runs;
        assert(actual.pose_live_turrets(ar,duplicate,{},&executor) && executor.runs==before);
        assert(ar.uploads==er.uploads); same_contacts(actual,expected);
        auto invalid=turret_population(); invalid.contact_bones_[1].clear(); GodotRenderer ir;
        assert(!invalid.pose_live_turrets(ir,poses,{},&executor));
        assert(invalid.turret_piece_rows_.empty());
        assert(ir.uploads.size()==1 && ir.uploads[0].entity==10 && !invalid.failure_.empty());
        auto interrupted=turret_population(); GodotRenderer stopped;
        assert(!interrupted.pose_live_turrets(stopped,poses,{},&failed) && stopped.uploads.empty());
        assert(interrupted.turret_piece_rows_.empty());

        // Warm buffers survive changed request counts and a failed phase.
        const auto staged_capacity=t.turret_staged_[0].capacity();
        const auto staged_data=t.turret_staged_[0].data();
        for (const auto count : {1U,0U,3U}) {
            auto serial=turret_population(); GodotRenderer expected_rows,actual_rows;
            const auto subset=std::span(poses).first(count);
            assert(serial.pose_live_turrets(expected_rows,subset,{},nullptr));
            assert(t.pose_live_turrets(actual_rows,subset,{},&executor));
            assert(actual_rows.uploads==expected_rows.uploads);
            assert(t.turret_staged_[0].capacity()==staged_capacity);
            assert(t.turret_staged_[0].data()==staged_data);
        }
        GodotRenderer recovered;
        assert(interrupted.pose_live_turrets(recovered,poses,{},&executor));
        assert(recovered.uploads==turret_renderer.uploads);
        const auto idle_data=p.prepared_idle_.data();
        for (unsigned tick : {13U,14U}) {
            auto serial=idle_population(); GodotRenderer expected_rows,actual_rows;
            serial.pose_units(expected_rows,tick,nullptr);
            p.pose_units(actual_rows,tick,&executor);
            assert(actual_rows.uploads==expected_rows.uploads);
            same_contacts(p,serial);
            assert(p.prepared_idle_.data()==idle_data);
        }
        auto v=visibility_population(); v.refresh_instances(&executor); same_visibility(v,visibility);
        const auto counts_data=v.piece_counts_.data();
        v.live_defend_[0]=false; v.live_stealth_[1]=false;
        auto serial_visibility=visibility_population();
        serial_visibility.live_defend_[0]=false; serial_visibility.live_stealth_[1]=false;
        // Reset accumulated diagnostics equally before comparing the next frame.
        v.blast_meshes_shown_.clear(); v.shield_shells_shown_=v.stealth_shells_shown_=0;
        serial_visibility.refresh_instances(nullptr); v.refresh_instances(&executor);
        same_visibility(v,serial_visibility); assert(v.piece_counts_.data()==counts_data);
        auto fallback=visibility_population(); fallback.refresh_instances(&failed); same_visibility(fallback,visibility);
        fallback.pieces_.clear(); fallback.refresh_instances(&executor);
        assert(fallback.instances_.empty() && fallback.piece_visibility_.empty() && fallback.attachments_drawn_==0);
    }
}
'''
