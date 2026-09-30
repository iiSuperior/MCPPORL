// Replays a two-player combat scenario (oracle/combat/*.txt) through the
// simulator's duel model and writes a trace with the combat oracle's fields,
// for tools/tracediff.py.
//
// Usage: duel_replay <sin_table.bin> <scenario.txt> <out.jsonl> [surfaceY]
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "mcp/duel.hpp"

using namespace mcp;

namespace {

struct Start {
    double x = 0.5, z = 0.5;
    float yaw = 0.0F;
    float health = 20.0F;
    Weapon weapon = Weapon::Hand;
};

struct Scenario {
    std::string name;
    Start a{0.5, 0.5, 0.0F, 20.0F}, b{0.5, 3.0, 180.0F, 20.0F};
    std::vector<DuelInput> ta, tb;
};

[[noreturn]] void fail(const std::string& where, const std::string& why) {
    std::fprintf(stderr, "%s: %s\n", where.c_str(), why.c_str());
    std::exit(2);
}

std::vector<std::string> words(const std::string& s) {
    std::istringstream ss(s);
    std::vector<std::string> out;
    for (std::string t; ss >> t;) out.push_back(t);
    return out;
}

// CombatScenario.input: rotation carries over between segments.
DuelInput parseInput(const std::string& spec, float& yaw, float& pitch, const std::string& where) {
    DuelInput in;
    for (const std::string& t : words(spec)) {
        if (t.rfind("yaw=", 0) == 0) yaw = std::strtof(t.c_str() + 4, nullptr);
        else if (t.rfind("pitch=", 0) == 0) pitch = std::strtof(t.c_str() + 6, nullptr);
        else if (t == "w") in.keys.forward = true;
        else if (t == "s") in.keys.backward = true;
        else if (t == "a") in.keys.left = true;
        else if (t == "d") in.keys.right = true;
        else if (t == "jump") in.keys.jump = true;
        else if (t == "sneak") in.keys.shift = true;
        else if (t == "sprint") in.keys.sprint = true;
        else if (t == "attack") in.attack = in.attackHeld = true;
        else if (t == "tap") in.attack = true;
        else if (t == "hold") in.attackHeld = true;
        else if (t != "idle") fail(where, "unknown token " + t);
    }
    in.yaw = yaw;
    in.pitch = pitch;
    return in;
}

// Mirrors oracle/harness CombatScenario.parse.
Scenario parseScenario(const std::string& path) {
    std::ifstream f(path);
    if (!f) fail(path, "cannot open");
    Scenario s;
    std::string base = path.substr(path.find_last_of("/\\") + 1);
    s.name = base.substr(0, base.size() - 4);
    struct Segment {
        int count;
        std::string a, b, where;
    };
    std::vector<Segment> segments;
    std::string line;
    int lineNo = 0;
    while (std::getline(f, line)) {
        lineNo++;
        std::string where = path + ":" + std::to_string(lineNo);
        auto hash = line.find('#');
        if (hash != std::string::npos) line.resize(hash);
        for (auto& ch : line) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        std::vector<std::string> tok = words(line);
        if (tok.empty()) continue;
        if (tok[0] == "start") {
            if (tok.size() < 2) fail(where, "start needs a player");
            Start st;
            for (size_t i = 2; i < tok.size(); ++i) {
                auto eq = tok[i].find('=');
                std::string k = tok[i].substr(0, eq), v = eq == std::string::npos ? "" : tok[i].substr(eq + 1);
                if (k == "x") st.x = std::strtod(v.c_str(), nullptr);
                else if (k == "z") st.z = std::strtod(v.c_str(), nullptr);
                else if (k == "yaw") st.yaw = std::strtof(v.c_str(), nullptr);
                else if (k == "health") st.health = std::strtof(v.c_str(), nullptr);
                else if (k == "item") {
                    if (!weaponByName(v.c_str(), st.weapon)) fail(where, "unsupported item " + v);
                }
                else fail(where, "unknown start key " + k);
            }
            if (tok[1] == "a") s.a = st;
            else if (tok[1] == "b") s.b = st;
            else fail(where, "unknown player " + tok[1]);
            continue;
        }
        auto bar = line.find('|');
        if (bar == std::string::npos || line.find('|', bar + 1) != std::string::npos)
            fail(where, "expected '<ticks> <A inputs> | <B inputs>'");
        std::vector<std::string> head = words(line.substr(0, bar));
        Segment seg;
        seg.count = std::atoi(head[0].c_str());
        for (size_t i = 1; i < head.size(); ++i) seg.a += head[i] + " ";
        seg.b = line.substr(bar + 1);
        seg.where = where;
        segments.push_back(seg);
    }
    float ya = s.a.yaw, pa = 0.0F, yb = s.b.yaw, pb = 0.0F;
    for (const Segment& seg : segments) {
        for (int i = 0; i < seg.count; ++i) {
            s.ta.push_back(parseInput(seg.a, ya, pa, seg.where));
            s.tb.push_back(parseInput(seg.b, yb, pb, seg.where));
        }
    }
    return s;
}

std::vector<float> loadSinTable(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::vector<float> t(mth::kSinTableSize);
    for (auto& v : t) {
        unsigned char c[4];
        if (!in.read(reinterpret_cast<char*>(c), 4)) fail(path, "bad sin table");
        uint32_t u = (uint32_t(c[0]) << 24) | (uint32_t(c[1]) << 16) | (uint32_t(c[2]) << 8) | c[3];
        std::memcpy(&v, &u, 4);
    }
    return t;
}

const char* tf(bool v) { return v ? "true" : "false"; }

void writeInput(std::FILE* out, const DuelInput& in) {
    std::fprintf(out,
                 "{\"W\": %s, \"S\": %s, \"A\": %s, \"D\": %s, \"jump\": %s, \"sneak\": %s, \"sprint\": %s, "
                 "\"attack\": %s, \"attackHeld\": %s, \"yaw\": \"%08" PRIx32 "\", \"pitch\": \"%08" PRIx32 "\"}",
                 tf(in.keys.forward), tf(in.keys.backward), tf(in.keys.left), tf(in.keys.right), tf(in.keys.jump),
                 tf(in.keys.shift), tf(in.keys.sprint), tf(in.attack), tf(in.attackHeld), j::fbits(in.yaw),
                 j::fbits(in.pitch));
}

void writeState(std::FILE* out, const DuelPlayer& d) {
    const Player& c = d.client;
    const ServerCopy& s = d.server;
    std::fprintf(out,
                 "{\"pos.x\": \"%016" PRIx64 "\", \"pos.y\": \"%016" PRIx64 "\", \"pos.z\": \"%016" PRIx64
                 "\", \"vel.x\": \"%016" PRIx64 "\", \"vel.y\": \"%016" PRIx64 "\", \"vel.z\": \"%016" PRIx64
                 "\", \"yRot\": \"%08" PRIx32 "\", \"onGround\": %s, \"sprinting\": %s, \"server.sprinting\": %s"
                 ", \"server.health\": \"%08" PRIx32 "\", \"server.hurtTime\": %d, \"server.damageCooldown\": %d"
                 ", \"server.onGround\": %s, \"server.fallDistance\": \"%016" PRIx64
                 "\", \"server.attackStrength\": \"%08" PRIx32 "\", \"server.vel.x\": \"%016" PRIx64
                 "\", \"server.vel.y\": \"%016" PRIx64 "\", \"server.vel.z\": \"%016" PRIx64
                 "\", \"server.yRot\": \"%08" PRIx32 "\", \"speedAttr\": \"%016" PRIx64
                 "\", \"pick\": %d, \"pick.x\": \"%016" PRIx64 "\", \"pick.y\": \"%016" PRIx64 "\", \"pick.z\": \"%016" PRIx64
                 "\", \"missTime\": %d, \"gotVelocity\": %s, \"sentAttack\": %s, \"teleports\": 0"
                 ", \"view.x\": \"%016" PRIx64 "\", \"view.y\": \"%016" PRIx64 "\", \"view.z\": \"%016" PRIx64
                 "\", \"view.yRot\": \"%08" PRIx32 "\", \"view.recv\": %d}",
                 j::dbits(c.x), j::dbits(c.y), j::dbits(c.z), j::dbits(c.vel.x), j::dbits(c.vel.y), j::dbits(c.vel.z),
                 j::fbits(c.yRot), tf(c.onGround), tf(c.sprinting), tf(s.body.sprinting), j::fbits(s.health),
                 s.hurtTime, s.damageCooldownTime, tf(s.body.onGround), j::dbits(s.fallDistance),
                 j::fbits(s.attackStrengthScale(0.5F)), j::dbits(s.body.vel.x), j::dbits(s.body.vel.y),
                 j::dbits(s.body.vel.z), j::fbits(s.body.yRot), j::dbits(c.movementSpeedAttribute()),
                 static_cast<int>(d.pick.type), j::dbits(d.pick.location.x), j::dbits(d.pick.location.y),
                 j::dbits(d.pick.location.z), d.missTime, tf(d.gotVelocity), tf(d.sentAttack), j::dbits(d.view.pos.x),
                 j::dbits(d.view.pos.y), j::dbits(d.view.pos.z), j::fbits(d.view.yRot), d.viewRecv);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: %s <sin_table.bin> <scenario.txt> <out.jsonl> [surfaceY]\n", argv[0]);
        return 2;
    }
    auto sinTab = loadSinTable(argv[1]);
    Scenario s = parseScenario(argv[2]);
    ArenaWorld world;  // the oracle walls its arena in (CombatOracle.forceArena)
    if (argc > 4) world.surfaceY = std::atoi(argv[4]);
    double y = static_cast<double>(world.surfaceY);

    Duel duel;
    duel.spawn(0, s.a.x, y, s.a.z, s.a.yaw, world, s.a.health, s.a.weapon);
    duel.spawn(1, s.b.x, y, s.b.z, s.b.yaw, world, s.b.health, s.b.weapon);
    duel.pairViews();

    std::FILE* out = std::fopen(argv[3], "w");
    if (!out) return 2;
    std::fprintf(out,
                 "{\"format\": \"mcporl-trace\", \"version\": 1, \"mc_version\": \"26.3\", \"source\": \"sim\", "
                 "\"domains\": [\"movement\", \"melee\"], \"fields\": {\"pos.x\": \"f64\", \"pos.y\": \"f64\", "
                 "\"pos.z\": \"f64\", \"vel.x\": \"f64\", \"vel.y\": \"f64\", \"vel.z\": \"f64\", \"yRot\": \"f32\", "
                 "\"onGround\": \"bool\", \"sprinting\": \"bool\", \"server.sprinting\": \"bool\", "
                 "\"server.health\": \"f32\", \"server.hurtTime\": \"i32\", \"server.damageCooldown\": \"i32\", "
                 "\"server.onGround\": \"bool\", \"server.fallDistance\": \"f64\", \"server.attackStrength\": \"f32\", "
                 "\"server.vel.x\": \"f64\", \"server.vel.y\": \"f64\", \"server.vel.z\": \"f64\", "
                 "\"server.yRot\": \"f32\", \"speedAttr\": \"f64\", \"pick\": \"i32\", \"pick.x\": \"f64\", "
                 "\"pick.y\": \"f64\", \"pick.z\": \"f64\", \"missTime\": \"i32\", \"gotVelocity\": \"bool\", "
                 "\"sentAttack\": \"bool\", \"teleports\": \"i32\", \"view.x\": \"f64\", \"view.y\": \"f64\", "
                 "\"view.z\": \"f64\", \"view.yRot\": \"f32\", \"view.recv\": \"i32\"}, "
                 "\"meta\": {\"scenario\": \"%s\", \"players\": [\"A\", \"B\"]}}\n",
                 s.name.c_str());
    for (size_t t = 0; t < s.ta.size(); ++t) {
        duel.step(s.ta[t], s.tb[t], world, sinTab.data());
        if (duel.unsupported()) {
            std::fprintf(stderr, "%s: tick %zu entered a state the port does not support\n", s.name.c_str(), t);
            return 3;
        }
        std::fprintf(out, "{\"t\": %zu, \"inputs\": {\"p0\": ", t);
        writeInput(out, s.ta[t]);
        std::fprintf(out, ", \"p1\": ");
        writeInput(out, s.tb[t]);
        std::fprintf(out, "}, \"entities\": {\"p0\": ");
        writeState(out, duel.p[0]);
        std::fprintf(out, ", \"p1\": ");
        writeState(out, duel.p[1]);
        std::fprintf(out, "}}\n");
        if (duel.done()) break;  // the episode ends on the killing tick, as in the oracle
        duel.deliver();
    }
    std::fclose(out);
    return 0;
}
