"""Nonblocking audio I/O and the real force/purchase preload seed closure.

The native fixture uses a reader deliberately blocked behind a promise: caller
request/drain must return while it is blocked, and duplicate requests cannot read
twice. No Godot or installed data is needed. Source checks additionally reject a
return to the old synchronous gameplay miss path.
"""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / 'tools'))
from native_cpp_fixture import compile_fixture, select_toolchain


def preload_body():
    text = (ROOT / 'apps/viewer/src/live_session_view.hpp').read_text(encoding='utf-8')
    begin = text.index('    [[nodiscard]] std::set<sim::tactical::TypeId> audio_preload_types() const {')
    opening = text.index('{', begin)
    depth = 1
    end = opening + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[begin:end]


HARNESS = r'''
#include "__QUEUE__"
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <future>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <thread>
#include <vector>
using namespace std::chrono_literals;
using eawr::presentation::godot_backend::BattleAudioLoadQueue;
namespace sim::tactical { using TypeId = std::uint64_t; }
struct Object { std::uint64_t type_id{}; std::vector<Object> members; };
struct Unit { std::uint64_t type_id{}, purchase_type{}; std::vector<Object> contained; };
struct Reserve { std::uint64_t type{}, purchase_type{}; bool restricted{}; std::vector<Object> contained; int owner{1}; };
struct Campaign { std::vector<Reserve> reserves; };
struct Setup { std::vector<Unit> units; std::optional<Campaign> campaign; };
struct Option { std::uint64_t type{}; };
struct Menu { std::uint64_t station{}, next_level{}; std::vector<Option> options; };
struct Rider { std::uint64_t type{}; };
struct Hero { std::uint64_t purchase{}, deployed{}; std::vector<Rider> riders; };
struct Economy { std::vector<Menu> menus; std::vector<Hero> heroes; };
struct View {
    std::optional<Setup> setup_;
    Economy economy_;
    struct Options { bool audio_preload{true}; } options_;
    int player_{1};
__PRELOAD__
};
int main() {
    View view;
    assert(view.audio_preload_types().empty());
    view.setup_ = Setup{{{1, 2, {{3, {{4, {}}}}}}}, Campaign{{{5, 6, false, {{7, {}}}}, {99, 0, true, {}}}}};
    // Menus are deliberately out of dependency order; reachable station upgrades
    // unlock their own menu, purchases, hero transport and carried hero types.
    view.economy_.menus = {{8, 0, {{9}}}, {1, 8, {{10}}}, {88, 0, {{89}}}};
    view.economy_.heroes = {{10, 11, {{12}}}};
    const auto seeds = view.audio_preload_types();
    assert(seeds == std::set<std::uint64_t>({1,2,3,4,5,6,7,8,9,10,11,12}));
    view.setup_->campaign->reserves.push_back({13, 14, false, {{15, {}}}, 2});
    assert(view.audio_preload_types().contains(15));
    view.options_.audio_preload = false;
    assert(view.audio_preload_types() == std::set<std::uint64_t>({1,5,6}));

    std::promise<void> entered, release;
    auto entered_future = entered.get_future();
    const auto released = release.get_future().share();
    std::atomic<int> reads{0};
    auto owner = std::make_shared<int>(42);
    std::weak_ptr<int> lifetime = owner;
    {
        BattleAudioLoadQueue<int, int> queue([&, keep = owner](const int& key) {
            assert(*keep == 42);
            if (reads.fetch_add(1) == 0) entered.set_value();
            released.wait();
            if (key == 3) throw std::runtime_error("synthetic read failure");
            return key * 10;
        }, 2);
        owner.reset();
        assert(!lifetime.expired()); // reader owns the input while external owner is gone
        const auto request_deadline = std::chrono::steady_clock::now() + 2s;
        bool requested = false;
        while (!(requested = queue.request(1)) && std::chrono::steady_clock::now() < request_deadline)
            std::this_thread::yield();
        assert(requested);
        assert(entered_future.wait_for(2s) == std::future_status::ready);
        // A caller can enqueue/drain repeatedly while the only reader is blocked.
        assert(queue.request(1));
        assert(queue.request(2));
        assert(!queue.request(3)); // capacity includes in-flight and queued keys
        for (int i = 0; i < 20; ++i) assert(queue.take_ready().empty());
        assert(reads.load() == 1);
        release.set_value();
        std::map<int,int> committed;
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (committed.size() != 2 && std::chrono::steady_clock::now() < deadline) {
            for (auto& ready : queue.take_ready()) {
                assert(ready.value && ready.error.empty());
                committed.emplace(ready.key, *ready.value);
            }
            std::this_thread::yield();
        }
        assert((committed == std::map<int,int>{{1,10},{2,20}}));
        assert(reads.load() == 2); // duplicate1 never issued another read
        while (!queue.request(3)) std::this_thread::yield();
        bool failed = false;
        const auto failure_deadline = std::chrono::steady_clock::now() + 2s;
        while (!failed && std::chrono::steady_clock::now() < failure_deadline) {
            for (auto& ready : queue.take_ready()) {
                assert(!ready.value && ready.error == "synthetic read failure");
                failed = true;
            }
            std::this_thread::yield();
        }
        assert(failed);
    }
    assert(lifetime.expired()); // queue released the input only after worker join

    // A per-frame drain limit preserves ready work and its capacity claim.
    std::atomic<int> finished{0};
    BattleAudioLoadQueue<int, int> limited([&](const int& key) {
        ++finished;
        return key;
    }, 2);
    const auto limited_deadline = std::chrono::steady_clock::now() + 2s;
    for (const int key : {1, 2}) {
        bool added = false;
        while (!(added = limited.request(key)) && std::chrono::steady_clock::now() < limited_deadline)
            std::this_thread::yield();
        assert(added);
    }
    while (finished.load() != 2 && std::chrono::steady_clock::now() < limited_deadline)
        std::this_thread::yield();
    assert(finished.load() == 2);
    assert(limited.take_ready(0).empty());
    std::vector<int> published;
    while (published.size() != 2 && std::chrono::steady_clock::now() < limited_deadline) {
        const auto ready = limited.take_ready(1);
        assert(ready.size() <= 1);
        if (!ready.empty()) published.push_back(*ready.front().value);
        std::this_thread::yield();
    }
    assert((published == std::vector<int>{1, 2}));

    // Early scene teardown while I/O is blocked must join before dropping the
    // reader's last shared input owner. The worker may not borrow the view.
    std::promise<void> blocked, unblock, closing, closed;
    auto blocked_future = blocked.get_future();
    auto closing_future = closing.get_future();
    auto closed_future = closed.get_future();
    const auto gate = unblock.get_future().share();
    auto input = std::make_shared<int>(77);
    std::weak_ptr<int> input_lifetime = input;
    auto early = std::make_unique<BattleAudioLoadQueue<int,int>>([&, held = input](const int&) {
        blocked.set_value();
        gate.wait();
        assert(*held == 77);
        return *held;
    });
    input.reset();
    const auto early_deadline = std::chrono::steady_clock::now() + 2s;
    bool early_requested = false;
    while (!(early_requested = early->request(1)) && std::chrono::steady_clock::now() < early_deadline)
        std::this_thread::yield();
    assert(early_requested && blocked_future.wait_for(2s) == std::future_status::ready);
    std::thread cleanup([queue = std::move(early), &closing, &closed]() mutable {
        closing.set_value();
        queue.reset();
        closed.set_value();
    });
    closing_future.wait();
    assert(closed_future.wait_for(20ms) == std::future_status::timeout);
    assert(!input_lifetime.expired());
    unblock.set_value();
    cleanup.join();
    assert(closed_future.wait_for(0ms) == std::future_status::ready && input_lifetime.expired());
}
'''


class AudioPreloadSourceTests(unittest.TestCase):
    def test_hidden_control_defaults_on_and_is_confined_to_loading_diagnostics(self):
        options = (ROOT / 'apps/viewer/src/live_session_options.hpp').read_text(encoding='utf-8')
        config = (ROOT / 'apps/viewer/src/live_session_config.cpp').read_text(encoding='utf-8')
        population = (ROOT / 'apps/viewer/src/map_mode_ready_population.cpp').read_text(encoding='utf-8')
        source = (ROOT / 'apps/viewer/src/battle_audio_prepare.cpp').read_text(encoding='utf-8')
        self.assertIn('bool audio_preload{true};', options)
        self.assertIn('--eawr-audio-preload expects on or off', config)
        self.assertIn('audio_options.preload = state.live_options.audio_preload;', population)
        readiness = source[source.index('bool BattleAudio::audio_ready('):]
        self.assertLess(readiness.index('if (!options_.preload)'), readiness.index('prepare_audio(filesystem_'))
        self.assertIn('commit_audio(key, prepare_audio(filesystem_, key), samples_prepared_)', readiness)

    def test_workers_produce_native_buffers_and_publish_one_per_frame(self):
        source = (ROOT / 'apps/viewer/src/battle_audio_prepare.cpp').read_text(encoding='utf-8')
        worker = source[source.index('BattleAudio::PreparedAudio BattleAudio::prepare_audio('):]
        worker = worker[:worker.index('const BattleAudio::Sample& BattleAudio::sample(')]
        for forbidden in ('godot::', 'wav_stream(', 'mp3_stream(', 'instantiate(', 'get_length('):
            self.assertNotIn(forbidden, worker)
        self.assertIn('audio::parse_wav(data, error)', worker)
        self.assertIn('audio_loader_->take_ready(1)', source)

    def test_unqueued_preload_reads_beside_the_load_and_is_joined_before_begin(self):
        source = (ROOT / 'apps/viewer/src/battle_audio_prepare.cpp').read_text(encoding='utf-8')
        prepare = source[source.index('void BattleAudio::prepare('):source.index('void BattleAudio::finish_prepare()')]
        self.assertIn('start_audio_readers(scheduled);', prepare)
        self.assertNotIn('prepare_audio_chunk', prepare)
        # Same disk order as the stepped path, a bound on buffers waiting for the owner thread, publication in order.
        readers = source[source.index('void BattleAudio::start_audio_readers('):source.index('void BattleAudio::pump_loading()')]
        self.assertIn('order_by_disk_position(*filesystem_, state->ordered);', readers)
        self.assertIn('max_waiting_bytes', source)
        self.assertIn('index = state.published;', readers)
        # Owner-thread publication only: no Godot resource is created on a reader.
        loop = source[source.index('void read_loop('):source.index('void BattleAudio::start_audio_readers(')]
        for forbidden in ('godot::', 'commit_audio(', 'wav_stream(', 'mp3_stream('):
            self.assertNotIn(forbidden, loop)
        hud = (ROOT / 'apps/viewer/src/map_mode_hud.cpp').read_text(encoding='utf-8')
        ready = (ROOT / 'apps/viewer/src/map_mode_ready_population.cpp').read_text(encoding='utf-8')
        release = (ROOT / 'apps/viewer/src/battle_audio.cpp').read_text(encoding='utf-8')
        self.assertIn('battle_audio->pump_loading();', hud)
        finish = ready[ready.index('bool MapMode::State::finish_ready_space('):]
        self.assertLess(finish.index('battle_audio->finish_loading();'), finish.index('loading_complete()'))
        self.assertIn('async_prepare_.reset();', release)

    def test_a_failed_or_refused_load_stops_the_readers_without_publishing(self):
        source = (ROOT / 'apps/viewer/src/battle_audio_prepare.cpp').read_text(encoding='utf-8')
        header = (ROOT / 'apps/viewer/src/battle_audio.hpp').read_text(encoding='utf-8')
        ready = (ROOT / 'apps/viewer/src/map_mode_ready.cpp').read_text(encoding='utf-8')
        population = (ROOT / 'apps/viewer/src/map_mode_ready_population.cpp').read_text(encoding='utf-8')
        cancel = source[source.index('void BattleAudio::cancel_loading() {'):source.index('void BattleAudio::finish_loading() {')]
        self.assertIn('async_prepare_.reset();', cancel)
        self.assertNotIn('publish_audio(', cancel)
        # Every failure path that reaches fail_ready, and the ready path with ready == false, cancel the preload.
        fail = ready[ready.index('bool MapMode::State::fail_ready('):]
        fail = fail[:fail.index('void MapMode::State::parse_ready_arguments(')]
        self.assertIn('battle_audio->cancel_loading();', fail)
        finish = population[population.index('bool MapMode::State::finish_ready_space('):]
        self.assertIn('if (ready) state.battle_audio->finish_loading();', finish)
        self.assertIn('else state.battle_audio->cancel_loading();', finish)
        # Nothing is looked up, read or requested while the preload is still being published.
        self.assertIn('if (async_prepare_) return false;', source)
        self.assertIn('if (async_prepare_) return pending_sample_;', source)
        # The bound on waiting buffers counts the decoded copy; a reader's exception is reported.
        self.assertIn('pcm->converted.size()', source)
        self.assertIn('audio loading thread: ', source)
        self.assertNotIn('prepare_audio_batch', header + source)

    def test_runtime_paths_queue_instead_of_opening_files(self):
        source = (ROOT / 'apps/viewer/src/battle_audio_prepare.cpp').read_text(encoding='utf-8')
        sample = source[source.index('const BattleAudio::Sample& BattleAudio::sample('):]
        sample = sample[:sample.index('\ngodot::Ref<godot::AudioStream> BattleAudio::music_stream')]
        self.assertLess(sample.index('if (samples_prepared_)'), sample.index('prepare_audio(filesystem_'))
        self.assertIn('return pending_sample_', sample)
        self.assertNotIn('++synchronous_runtime_loads_', sample)
        self.assertIn('if (runtime && reader_on_main) ++synchronous_runtime_loads_', source)
        self.assertIn('[filesystem = filesystem_]', source)
        self.assertNotIn('(first_type && cue->is_3d)', source)


class AudioPreloadNativeTests(unittest.TestCase):
    def test_blocked_reader_dedup_backpressure_failure_and_real_seed_closure(self):
        toolchain = select_toolchain()
        if toolchain is None:
            self.skipTest('No native C++20 fixture compiler')
        with tempfile.TemporaryDirectory(prefix='eawr-audio-preload-') as temp:
            directory = Path(temp)
            cpp = directory / 'audio.cpp'
            executable = directory / ('audio.exe' if os.name == 'nt' else 'audio')
            queue = (ROOT / 'apps/viewer/src/battle_audio_load_queue.hpp').as_posix()
            cpp.write_text(HARNESS.replace('__QUEUE__', queue).replace('__PRELOAD__', preload_body()), encoding='utf-8')
            compile_fixture(toolchain, cpp, executable)
            ran = subprocess.run([str(executable)], capture_output=True, text=True, timeout=10)
            self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)


class AbilityLoopCpuTests(unittest.TestCase):
    def test_authored_loop_perturbation_and_firing_gate(self):
        preparation = (ROOT / 'apps/viewer/src/battle_audio_prepare.cpp').read_text(encoding='utf-8')
        self.assertIn('ability_loop_audio::bind(kind', preparation)
        toolchain = select_toolchain()
        if toolchain is None:
            self.skipTest('No native C++20 fixture compiler')
        source = r'''
#include "ability_loop_audio.hpp"
#include <cassert>
#include <string>
#include <map>
#include <optional>
using namespace eawr;
using namespace presentation::godot_backend::ability_loop_audio;
int main() {
    using K = sim::tactical::AbilityKind;
    std::map<std::string, int> events{{"mod_a", 1}, {"mod_b", 2}};
    for (const auto kind : {K::leech_shields, K::tractor_beam, K::energy_weapon, K::super_laser}) {
        std::string authored = "mod_a";
        auto read = [&] { return authored; };
        auto resolve = [&](const std::string& name) { return events.at(name); };
        assert(bind(kind, read, resolve) == 1);
        authored = "mod_b";
        assert(bind(kind, read, resolve) == 2);
        sim::tactical::AbilityStatus status{};
        status.kind = kind; status.active = true; status.target = 7; status.started_tick = 10;
        assert(!playing(status, 9, 4) || kind != K::super_laser);
        assert(playing(status, 14, 4));
        if (kind == K::super_laser) assert(!playing(status, 13, 4));
        status.active = false;
        assert(!playing(status, 20, 4));
    }
    assert(!supported(K::defend));
    struct Cue { double fade; } a{1.25}, b{0.5};
    struct Active { const Cue* event; std::optional<int> handle; };
    std::map<int, Active> active;
    std::map<int, const Cue*> desired{{1, &a}, {2, &a}};
    int starts = 0, stops = 0;
    double faded = 0;
    auto start = [&](int id, const Cue* cue) -> std::optional<Active> {
        ++starts; return Active{cue, id == 2 ? std::nullopt : std::optional<int>{id}};
    };
    auto stop = [&](const Active& old) { if (old.handle) { ++stops; faded += old.event->fade; } };
    reconcile(active, desired, start, stop);
    reconcile(active, desired, start, stop);
    assert(starts == 2 && stops == 0); // refused endpoint also does not retry
    desired[1] = &b; desired.erase(2);
    reconcile(active, desired, start, stop);
    assert(starts == 3 && stops == 1 && faded == 1.25 && active.size() == 1);
    desired.clear(); reconcile(active, desired, start, stop);
    assert(stops == 2 && faded == 1.75 && active.empty());
}
'''
        with tempfile.TemporaryDirectory(prefix='eawr-ability-loop-') as temp:
            directory = Path(temp)
            cpp = directory / 'ability_loop.cpp'
            executable = directory / ('ability_loop.exe' if os.name == 'nt' else 'ability_loop')
            cpp.write_text(source, encoding='utf-8')
            compile_fixture(toolchain, cpp, executable,
                            (ROOT / 'include', ROOT / 'apps/viewer/src'))
            ran = subprocess.run([str(executable)], capture_output=True, text=True, timeout=10)
            self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)


def _braced_source(text, needle):
    start = text.index(needle)
    opening = text.index('{', start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


class AbilityLoopAuthoredReadTests(unittest.TestCase):
    def test_real_authored_read_and_preload_selection(self):
        toolchain = select_toolchain()
        if toolchain is None:
            self.skipTest('No native C++20 fixture compiler')
        prepare = (ROOT / 'apps/viewer/src/battle_audio_prepare.cpp').read_text(encoding='utf-8')
        trim = _braced_source(prepare, '[[nodiscard]] std::string trim(')
        same = _braced_source(prepare, '[[nodiscard]] bool same(')
        child = _braced_source(prepare, '[[nodiscard]] std::string child(')
        binding = _braced_source(prepare, 'if (ability_loop_audio::supported(kind))')
        event = _braced_source(prepare, 'const auto event = [this, &immediate_roots, &binding_unit, &first_type]') + ';'
        source = r'''
#include "ability_loop_audio.hpp"
#include <algorithm>
#include <cassert>
#include <cctype>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>
namespace sim = eawr::sim;
namespace tactical = sim::tactical;
namespace ability_loop_audio = eawr::presentation::godot_backend::ability_loop_audio;
namespace data {
struct XmlNode { std::string name, raw_text; std::vector<XmlNode> children; };
namespace tag_trace { inline void used(const XmlNode&) {} }
}
namespace audio { struct SfxEvent { std::string name; bool is_3d; }; }
__TRIM__
__SAME__
__CHILD__
struct Nested { tactical::SpecialAbilityKind kind; std::string name; unsigned fire_delay_frames; };
struct Type { std::vector<Nested> special_abilities; };
struct Sounds { std::map<tactical::AbilityKind, const audio::SfxEvent*> ability_loops; unsigned super_laser_fire_delay{}; };
struct Fixture {
    struct Options { bool preload{true}; } options_;
    std::map<std::string, audio::SfxEvent> registry{{"mod_a", {"mod_a", true}}, {"mod_b", {"mod_b", true}}};
    const audio::SfxEvent* event(std::string_view name, const std::string&) {
        const auto found = registry.find(trim(name));
        return found == registry.end() ? nullptr : &found->second;
    }
    auto run(const data::XmlNode& ability, tactical::AbilityKind kind, bool first_type) {
        std::set<std::string> immediate_roots;
        bool binding_unit = true;
        Type type{};
        Sounds sounds{};
        const std::string where = "synthetic";
        __EVENT__
        __BINDING__
        return std::pair{sounds, immediate_roots};
    }
};
int main() {
    Fixture fixture;
    using K = tactical::AbilityKind;
    for (auto kind : {K::leech_shields, K::tractor_beam, K::energy_weapon, K::super_laser}) {
        data::XmlNode ability{"Unit_Ability", "", {{"SFXEvent_Special_Ability_Loop", " mod_a ", {}}}};
        auto [first, roots] = fixture.run(ability, kind, true);
        assert(first.ability_loops.at(kind)->name == "mod_a" && roots == std::set<std::string>{"mod_a"});
        ability.children[0].raw_text = "mod_b";
        auto [second, changed_roots] = fixture.run(ability, kind, true);
        assert(second.ability_loops.at(kind)->name == "mod_b" && changed_roots == std::set<std::string>{"mod_b"});
        auto [later, no_roots] = fixture.run(ability, kind, false);
        assert(later.ability_loops.at(kind)->name == "mod_b" && no_roots.empty());
        ability.children.clear();
        auto [missing, empty] = fixture.run(ability, kind, true);
        assert(missing.ability_loops.at(kind) == nullptr && empty.empty());
    }
}
'''
        for key, value in (('__TRIM__', trim), ('__SAME__', same), ('__CHILD__', child),
                           ('__EVENT__', event), ('__BINDING__', binding)):
            source = source.replace(key, value)
        with tempfile.TemporaryDirectory(prefix='eawr-loop-authored-') as temp:
            directory = Path(temp)
            cpp = directory / 'authored.cpp'
            executable = directory / ('authored.exe' if os.name == 'nt' else 'authored')
            cpp.write_text(source, encoding='utf-8')
            compile_fixture(toolchain, cpp, executable,
                            (ROOT / 'include', ROOT / 'apps/viewer/src'))
            ran = subprocess.run([str(executable)], capture_output=True, text=True, timeout=10)
            self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)


if __name__ == '__main__':
    unittest.main()
