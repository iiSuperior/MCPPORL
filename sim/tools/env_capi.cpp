// C interface to BatchEnv (env.hpp) for the Python trainer (ctypes, see
// trainer/mcporl/env.py). All buffers are caller-owned float32/uint8 arrays.
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "mcp/env.hpp"

using namespace mcp;

extern "C" {

int mcp_env_obs_size() { return kObsSize; }
int mcp_env_action_size() { return kActionSize; }
int mcp_env_stats_size() { return kEpisodeStatsSize; }

// reward: damage_dealt, damage_taken, win, loss, reach_shaping, gamma, aim_shaping.
void* mcp_env_create(int n, unsigned long long seed, const char* sinTablePath, int maxTicks, unsigned weaponMask,
                     int sameWeapon, const float* reward, float maxTurnDegPerTick, double minStartDist,
                     double maxStartDist) {
    std::vector<float> table(mth::kSinTableSize);
    std::ifstream in(sinTablePath, std::ios::binary);
    for (auto& v : table) {
        unsigned char b[4];
        if (!in.read(reinterpret_cast<char*>(b), 4)) return nullptr;
        uint32_t u = (uint32_t(b[0]) << 24) | (uint32_t(b[1]) << 16) | (uint32_t(b[2]) << 8) | b[3];
        std::memcpy(&v, &u, 4);
    }
    EnvConfig c;
    c.maxTicks = maxTicks;
    c.weaponMask = weaponMask;
    c.sameWeapon = sameWeapon != 0;
    c.reward = RewardWeights{reward[0], reward[1], reward[2], reward[3], reward[4], reward[5], reward[6], 0.0F, 0.0F};
    c.caps.maxTurnDegPerTick = maxTurnDegPerTick;
    c.minStartDist = minStartDist;
    c.maxStartDist = maxStartDist;
    return new BatchEnv(n, seed, std::move(table), c);
}

// Change the reward weights (same order as create, then aim_dense, reach_dense).
void mcp_env_set_reward(void* h, const float* r) {
    static_cast<BatchEnv*>(h)->cfg.reward = RewardWeights{r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7], r[8]};
}

void mcp_env_destroy(void* h) { delete static_cast<BatchEnv*>(h); }

void mcp_env_observe(void* h, float* obs) { static_cast<BatchEnv*>(h)->observeAll(obs); }

void mcp_env_step(void* h, const float* actions, float* obs, float* reward, unsigned char* done, float* stats) {
    static_cast<BatchEnv*>(h)->step(actions, obs, reward, done, stats);
}

// Scripted actions for every slot whose mask entry is set (see BatchEnv::scripted).
void mcp_env_scripted(void* h, const unsigned char* mask, int kind, float noiseDeg, float turnDeg, float* actions) {
    BatchEnv* e = static_cast<BatchEnv*>(h);
    for (int i = 0; i < e->size(); ++i)
        for (int k = 0; k < 2; ++k)
            if (mask[2 * i + k]) e->scripted(i, k, kind, noiseDeg, turnDeg, actions + (2 * i + k) * kActionSize);
}

// The same, with each slot's own kind, noise and turn rate ([2n] arrays).
void mcp_env_scripted_each(void* h, const unsigned char* mask, const int* kind, const float* noiseDeg, const float* turnDeg,
                           float* actions) {
    BatchEnv* e = static_cast<BatchEnv*>(h);
    for (int s = 0; s < 2 * e->size(); ++s)
        if (mask[s]) e->scripted(s / 2, s % 2, kind[s], noiseDeg[s], turnDeg[s], actions + s * kActionSize);
}

void mcp_env_record(void* h, int i) { static_cast<BatchEnv*>(h)->record(i); }

// 0 not recording, 1 in progress, 2 finished (by a death), 3 truncated.
int mcp_env_record_state(void* h, int i) {
    BatchEnv* e = static_cast<BatchEnv*>(h);
    if (e->recording[i] != 2) return e->recording[i];
    return e->recordedDone[i] ? 2 : 3;
}

// Random draws in the recorded episode that vanilla makes from state we cannot
// reproduce (plus 1000 if it left the supported domain): 0 means replayable bit for bit.
int mcp_env_record_nonparity(void* h, int i) { return static_cast<BatchEnv*>(h)->recordedNonParity[i]; }

// Writes the recorded episode of duel i as an oracle scenario; returns 0 on success.
int mcp_env_export(void* h, int i, const char* path, const char* title) {
    BatchEnv* e = static_cast<BatchEnv*>(h);
    std::ofstream out(path);
    if (!out) return 1;
    out << e->scenario(i, title);
    e->recording[i] = 0;
    return out ? 0 : 1;
}

long long mcp_env_turns_clamped(void* h) { return static_cast<BatchEnv*>(h)->fairness.turnsClamped; }

}  // extern "C"
