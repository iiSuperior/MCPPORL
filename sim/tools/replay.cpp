// Replays an oracle scenario (oracle/scenarios/*.txt) through the simulator and
// writes a trace in docs/TRACE_FORMAT.md format, for tools/tracediff.py.
//
// Usage: replay <sin_table.bin> <scenario.txt> <out.jsonl> [surfaceY]
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "mcp/player.hpp"

using namespace mcp;

namespace {

struct TickInput {
    Keys keys;
    float yaw, pitch;
};

struct Scenario {
    std::string name;
    double startX = 0.5, startZ = 0.5;
    std::vector<TickInput> ticks;
};

// Mirrors oracle/harness Scenario.parse, including float accumulation order.
Scenario parseScenario(const std::string& path) {
    std::ifstream in(path);
    if (!in) {
        std::fprintf(stderr, "cannot open %s\n", path.c_str());
        std::exit(2);
    }
    Scenario s;
    std::string base = path.substr(path.find_last_of("/\\") + 1);
    s.name = base.substr(0, base.size() - 4);
    float yaw = 0.0F, pitch = 0.0F;
    std::string line;
    int lineNo = 0;
    while (std::getline(in, line)) {
        lineNo++;
        auto hash = line.find('#');
        if (hash != std::string::npos) line.resize(hash);
        std::istringstream ss(line);
        std::vector<std::string> tok;
        for (std::string t; ss >> t;) {
            for (auto& ch : t) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            tok.push_back(t);
        }
        if (tok.empty()) continue;
        auto fail = [&](const std::string& why) {
            std::fprintf(stderr, "%s:%d: %s\n", path.c_str(), lineNo, why.c_str());
            std::exit(2);
        };
        if (tok[0] == "start") {
            for (size_t i = 1; i < tok.size(); ++i) {
                auto eq = tok[i].find('=');
                std::string k = tok[i].substr(0, eq), v = tok[i].substr(eq + 1);
                if (k == "x") s.startX = std::strtod(v.c_str(), nullptr);
                else if (k == "z") s.startZ = std::strtod(v.c_str(), nullptr);
                else if (k == "yaw") yaw = std::strtof(v.c_str(), nullptr);
                else if (k == "pitch") pitch = std::strtof(v.c_str(), nullptr);
                else fail("unknown start key " + k);
            }
            continue;
        }
        int count = std::atoi(tok[0].c_str());
        Keys k;
        float dyaw = 0.0F, dpitch = 0.0F;
        for (size_t i = 1; i < tok.size(); ++i) {
            const std::string& t = tok[i];
            if (t.rfind("yaw=", 0) == 0) yaw = std::strtof(t.c_str() + 4, nullptr);
            else if (t.rfind("pitch=", 0) == 0) pitch = std::strtof(t.c_str() + 6, nullptr);
            else if (t.rfind("dyaw=", 0) == 0) dyaw = std::strtof(t.c_str() + 5, nullptr);
            else if (t.rfind("dpitch=", 0) == 0) dpitch = std::strtof(t.c_str() + 7, nullptr);
            else if (t == "w") k.forward = true;
            else if (t == "s") k.backward = true;
            else if (t == "a") k.left = true;
            else if (t == "d") k.right = true;
            else if (t == "jump") k.jump = true;
            else if (t == "sneak") k.shift = true;
            else if (t == "sprint") k.sprint = true;
            else if (t == "idle") {}
            else fail("unknown token " + t);
        }
        for (int i = 0; i < count; ++i) {
            yaw += dyaw;
            pitch += dpitch;
            s.ticks.push_back(TickInput{k, yaw, pitch});
        }
    }
    return s;
}

std::vector<float> loadSinTable(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::vector<float> t(mth::kSinTableSize);
    for (auto& v : t) {
        unsigned char b[4];
        if (!in.read(reinterpret_cast<char*>(b), 4)) {
            std::fprintf(stderr, "bad sin table %s\n", path.c_str());
            std::exit(2);
        }
        uint32_t u = (uint32_t(b[0]) << 24) | (uint32_t(b[1]) << 16) | (uint32_t(b[2]) << 8) | b[3];
        std::memcpy(&v, &u, 4);
    }
    return t;
}

const char* b(bool v) { return v ? "true" : "false"; }

}  // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: %s <sin_table.bin> <scenario.txt> <out.jsonl> [surfaceY]\n", argv[0]);
        return 2;
    }
    auto sinTab = loadSinTable(argv[1]);
    Scenario s = parseScenario(argv[2]);
    FlatWorld world;
    if (argc > 4) world.surfaceY = std::atoi(argv[4]);

    Player p;
    p.x = s.startX;
    p.y = static_cast<double>(world.surfaceY);
    p.z = s.startZ;

    std::FILE* out = std::fopen(argv[3], "w");
    if (!out) return 2;
    std::fprintf(out,
                 "{\"format\": \"mcporl-trace\", \"version\": 1, \"mc_version\": \"26.3\", \"source\": \"sim\", "
                 "\"domains\": [\"movement\"], \"fields\": {\"pos.x\": \"f64\", \"pos.y\": \"f64\", \"pos.z\": \"f64\", "
                 "\"vel.x\": \"f64\", \"vel.y\": \"f64\", \"vel.z\": \"f64\", \"yRot\": \"f32\", \"xRot\": \"f32\", "
                 "\"xxa\": \"f32\", \"zza\": \"f32\", \"onGround\": \"bool\", \"horizontalCollision\": \"bool\", "
                 "\"sprinting\": \"bool\", \"crouching\": \"bool\"}, \"meta\": {\"scenario\": \"%s\"}}\n",
                 s.name.c_str());
    int t = 0;
    for (const TickInput& in : s.ticks) {
        tick(p, in.keys, in.yaw, in.pitch, world, sinTab.data());
        if (p.unsupported) {
            std::fprintf(stderr, "%s: tick %d entered a state the port does not support\n", s.name.c_str(), t);
            return 3;
        }
        std::fprintf(out,
                     "{\"t\": %d, \"entities\": {\"p0\": {\"pos.x\": \"%016" PRIx64 "\", \"pos.y\": \"%016" PRIx64
                     "\", \"pos.z\": \"%016" PRIx64 "\", \"vel.x\": \"%016" PRIx64 "\", \"vel.y\": \"%016" PRIx64
                     "\", \"vel.z\": \"%016" PRIx64 "\", \"yRot\": \"%08" PRIx32 "\", \"xRot\": \"%08" PRIx32
                     "\", \"xxa\": \"%08" PRIx32 "\", \"zza\": \"%08" PRIx32
                     "\", \"onGround\": %s, \"horizontalCollision\": %s, \"sprinting\": %s, \"crouching\": %s}}}\n",
                     t++, j::dbits(p.x), j::dbits(p.y), j::dbits(p.z), j::dbits(p.vel.x), j::dbits(p.vel.y),
                     j::dbits(p.vel.z), j::fbits(p.yRot), j::fbits(p.xRot), j::fbits(p.xxa), j::fbits(p.zza),
                     b(p.onGround), b(p.horizontalCollision), b(p.sprinting), b(p.crouching));
    }
    std::fclose(out);
    return 0;
}
