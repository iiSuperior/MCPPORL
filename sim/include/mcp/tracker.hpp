// How each client sees the other player, ported from vanilla 26.3:
//
//   server: ServerEntity.sendChanges (the entity tracker) decides every tick
//           whether to send a move packet for a player to the clients that
//           track it, and encodes positions with VecDeltaCodec (1/4096 steps)
//           as a stepped PositionPath (SteppedInterpolationTracker);
//   client: ClientPacketListener.handleMoveEntity / handleEntityPositionSync
//           feed the remote player's SteppedInterpolationHandler, which
//           Entity.commonTick advances once per client tick. RemotePlayer is
//           noPhysics, so that interpolation is the only thing that moves it.
//
// The remote player's box is what a client's crosshair pick tests, so this is
// what "where the opponent is" means for aiming. Scope: players on foot, never
// riding, never teleported by the server; paths hold at most one step.
#pragma once

#include <cstdint>
#include <cstring>

#include "mcp/jmath.hpp"
#include "mcp/vec.hpp"

namespace mcp {

namespace track {

// Mth.packDegrees / unpackDegrees
MCP_HD inline int8_t packDegrees(float angle) {
    float f = angle * 256.0F / 360.0F;
    int32_t i = static_cast<int32_t>(f);  // Mth.floor(float)
    if (f < static_cast<float>(i)) i--;
    return static_cast<int8_t>(static_cast<uint8_t>(static_cast<uint32_t>(i) & 0xFFu));
}
MCP_HD inline float unpackDegrees(int8_t rot) { return static_cast<float>(static_cast<int32_t>(rot) * 360) / 256.0F; }

// Mth.wrapDegrees(float)
MCP_HD inline float wrapDegrees(float v) {
    float f = ::fmodf(v, 360.0F);
    if (f >= 180.0F) f -= 360.0F;
    if (f < -180.0F) f += 360.0F;
    return f;
}
// Mth.rotLerp(float, float, float) / Mth.lerp(float, float, float) / Mth.lerp(double, double, double)
MCP_HD inline float rotLerp(float a, float from, float to) { return from + a * wrapDegrees(to - from); }
MCP_HD inline float lerpF(float a, float from, float to) { return from + a * (to - from); }
MCP_HD inline double lerpD(double a, double from, double to) { return from + a * (to - from); }
MCP_HD inline Vec3 lerp(const Vec3& from, const Vec3& to, double a) {
    return Vec3{lerpD(a, from.x, to.x), lerpD(a, from.y, to.y), lerpD(a, from.z, to.z)};
}

// Objects.equals on two Vec3 records: Double.compare per component, so 0.0 and
// -0.0 differ (NaN is out of scope).
MCP_HD inline bool sameBits(double a, double b) {
    uint64_t x, y;
    std::memcpy(&x, &a, 8);
    std::memcpy(&y, &b, 8);
    return x == y;
}
MCP_HD inline bool sameVec(const Vec3& a, const Vec3& b) { return sameBits(a.x, b.x) && sameBits(a.y, b.y) && sameBits(a.z, b.z); }

// VecDeltaCodec.encode / decode (Math.round: floor(x + 0.5) without the
// double rounding of the naive sum).
MCP_HD inline int64_t encode(double input) {
    double v = input * 4096.0;
    double f = ::floor(v);
    return static_cast<int64_t>(v - f >= 0.5 ? f + 1.0 : f);
}
MCP_HD inline double decode(int64_t v) { return static_cast<double>(v) / 4096.0; }

MCP_HD inline bool deltaTooBig(int64_t xa, int64_t ya, int64_t za) {
    return xa < -32768 || xa > 32767 || ya < -32768 || ya > 32767 || za < -32768 || za > 32767;
}

// VecDeltaCodec.decode(xa, ya, za) against `base`.
MCP_HD inline Vec3 decodeDelta(const Vec3& base, int64_t xa, int64_t ya, int64_t za) {
    if (xa == 0 && ya == 0 && za == 0) return base;
    double x = xa == 0 ? base.x : decode(encode(base.x) + xa);
    double y = ya == 0 ? base.y : decode(encode(base.y) + ya);
    double z = za == 0 ? base.z : decode(encode(base.z) + za);
    return Vec3{x, y, z};
}

}  // namespace track

// PositionPath for a player: Linear(end), or Stepped with one PositionStep
// (end, tickOffset). Longer stepped paths only come from syncPosition, which
// players on foot never set.
struct PositionPath {
    Vec3 end{};
    bool stepped = false;
    int32_t tickOffset = 0;
};

// One tracker packet for a player, as the receiving client decodes it.
enum class MoveKind : uint8_t { None = 0, Pos = 1, PosRot = 2, Rot = 4, Sync = 8 };

struct EntityMove {
    MoveKind kind = MoveKind::None;
    // Pos / PosRot: the VecDelta (one step when stepped)
    int16_t xa = 0, ya = 0, za = 0;
    bool stepped = false;
    int32_t tickOffset = 0;
    int8_t yRotn = 0, xRotn = 0;  // PosRot / Rot
    // Sync: the full path and the exact rotation
    PositionPath path{};
    float yRot = 0.0F, xRot = 0.0F;
    bool onGround = false;
};

// ServerEntity for one player plus its SteppedInterpolationTracker. The player
// entity type has updateInterval 2.
struct EntityTracker {
    static constexpr int32_t kUpdateInterval = 2;
    Vec3 base{};  // positionCodec
    int8_t lastSentYRot = 0, lastSentXRot = 0;
    int32_t tickCount = 0, teleportDelay = 0;
    bool wasOnGround = false;
    int32_t ticksSinceLastStep = -1;  // SteppedInterpolationTracker

    // new ServerEntity(...) when the player joins the level.
    MCP_HD void start(const Vec3& pos, float yRot, float xRot, bool onGround) {
        *this = EntityTracker{};
        base = pos;
        lastSentYRot = track::packDegrees(yRot);
        lastSentXRot = track::packDegrees(xRot);
        wasOnGround = onGround;
    }

    // ServerEntity.sendChanges for a player that is not riding. `needsSync` and
    // `dataDirty` are the entity's flags at this point of the server tick
    // (knockback or pushing; changed synched data such as health or the sprint
    // flag). Returns the move packet sent to the other client, if any.
    MCP_HD EntityMove sendChanges(const Vec3& pos, float yRot, float xRot, bool onGround, bool needsSync, bool dataDirty) {
        EntityMove out{};
        ticksSinceLastStep++;  // updateTracking (never a syncPosition step for players)
        if (needsSync || tickCount % kUpdateInterval == 0 || dataDirty) {
            int8_t yRotn = track::packDegrees(yRot), xRotn = track::packDegrees(xRot);
            // Math.abs(yRotn - lastSentYRot) >= 1 on the widened bytes: any difference.
            bool shouldSendRotation = yRotn != lastSentYRot || xRotn != lastSentXRot;
            teleportDelay++;
            // getPositionPath (addStep, then the path), then clear
            PositionPath path{pos, false, 0};
            if (ticksSinceLastStep > 0) {
                path.stepped = true;
                path.tickOffset = ticksSinceLastStep;
            }
            ticksSinceLastStep = 0;
            Vec3 d{pos.x - base.x, pos.y - base.y, pos.z - base.z};
            bool positionChanged = d.lengthSqr() >= static_cast<double>(7.6293945E-6F);
            bool shouldSendPosition = positionChanged || tickCount % 60 == 0;
            // createMovePacket
            bool sync = false;
            if (teleportDelay > 400 || wasOnGround != onGround) {
                wasOnGround = onGround;
                teleportDelay = 0;
                sync = true;
            } else if (shouldSendPosition) {
                int64_t xa = track::encode(pos.x) - track::encode(base.x);
                int64_t ya = track::encode(pos.y) - track::encode(base.y);
                int64_t za = track::encode(pos.z) - track::encode(base.z);
                if (track::deltaTooBig(xa, ya, za)) {
                    sync = true;
                } else {
                    out.kind = shouldSendRotation ? MoveKind::PosRot : MoveKind::Pos;
                    out.xa = static_cast<int16_t>(xa);
                    out.ya = static_cast<int16_t>(ya);
                    out.za = static_cast<int16_t>(za);
                    out.stepped = path.stepped;
                    out.tickOffset = path.tickOffset;
                    out.yRotn = yRotn;
                    out.xRotn = xRotn;
                }
            } else if (shouldSendRotation) {
                out.kind = MoveKind::Rot;
                out.yRotn = yRotn;
                out.xRotn = xRotn;
            }
            if (sync) {
                out.kind = MoveKind::Sync;
                out.path = path;
                out.yRot = yRot;
                out.xRot = xRot;
            }
            out.onGround = onGround;
            if (out.kind != MoveKind::None) {
                if (out.kind != MoveKind::Rot) base = pos;
                if (out.kind != MoveKind::Pos) {
                    lastSentYRot = yRotn;
                    lastSentXRot = xRotn;
                }
            }
        }
        tickCount++;
        return out;
    }
};

// The client's RemotePlayer for the other player: its position, rotation and
// SteppedInterpolationHandler (AbstractInterpolationHandler).
struct RemoteView {
    static constexpr int32_t kInterpolationSteps = EntityTracker::kUpdateInterval;
    static constexpr int32_t kMaxSteps = 8;

    struct PosRot {
        Vec3 pos{};
        float yRot = 0.0F, xRot = 0.0F;
        MCP_HD void addRotation(float y, float x) {
            yRot += y;
            xRot += x;
        }
    };
    struct Step {
        PosRot pr{};
        int32_t tickOffset = 0;
    };

    bool exists = false;
    bool unsupported = false;
    Vec3 pos{}, old{};
    float yRot = 0.0F, xRot = 0.0F;
    Vec3 codecBase{};
    // SteppedInterpolationHandler.InterpolationData
    PosRot data{};          // the interpolation target (persists across resets)
    PosRot lastStep{};      // lastStepPosRot
    Step steps[kMaxSteps];  // remainingSteps
    int32_t stepCount = 0;
    float currentStepTicks = 0.0F, remainingTicks = 0.0F, interpolationSpeed = 1.0F;
    PosRot lastPositionAndRotation{};  // AbstractInterpolationHandler

    MCP_HD bool active() const { return stepCount > 0; }

    // Entity.setRot
    MCP_HD void setRot(float y, float x) {
        yRot = ::fmodf(y, 360.0F);
        xRot = ::fmodf(x, 360.0F);
    }

    // ClientPacketListener.handleAddEntity -> LivingEntity.recreateFromPacket
    // (absSnapTo) -> RemotePlayer.recreateFromPacket (setOldPosAndRot).
    MCP_HD void add(const Vec3& p, int8_t yRotn, int8_t xRotn) {
        *this = RemoteView{};
        exists = true;
        codecBase = p;
        pos = old = p;
        yRot = ::fmodf(track::unpackDegrees(yRotn), 360.0F);
        float x = track::unpackDegrees(xRotn);
        x = x < -90.0F ? -90.0F : (x > 90.0F ? 90.0F : x);
        xRot = ::fmodf(x, 360.0F);
    }

    MCP_HD void addStep(const Vec3& p, float y, float x, int32_t ticks) {
        if (stepCount == kMaxSteps) {
            unsupported = true;
            return;
        }
        steps[stepCount++] = Step{PosRot{p, y, x}, ticks};
        remainingTicks += static_cast<float>(ticks);
    }

    MCP_HD void setLast() { lastPositionAndRotation = PosRot{pos, yRot, xRot}; }

    // SteppedInterpolationHandler.startInterpolating
    MCP_HD void startInterpolating(const PositionPath& path, float y, float x) {
        if (!active()) {  // setStartingPoint
            lastStep = PosRot{pos, yRot, xRot};
            currentStepTicks = 1.0F;
        }
        if (track::sameVec(path.end, data.pos)) {
            addStep(path.end, y, x, kInterpolationSteps);
        } else if (!path.stepped) {
            addStep(path.end, y, x, kInterpolationSteps);
        } else if (y == data.yRot && x == data.xRot) {
            addStep(path.end, y, x, path.tickOffset);
        } else {
            // One step: offset / total is exactly 1.
            float a = static_cast<float>(path.tickOffset) / static_cast<float>(path.tickOffset);
            addStep(path.end, track::rotLerp(a, data.yRot, y), track::lerpF(a, data.xRot, x), path.tickOffset);
        }
        data = PosRot{path.end, y, x};
    }

    // AbstractInterpolationHandler.interpolateTo(path, yRot, xRot, hasRotation);
    // `path` null means "keep the current position".
    MCP_HD void interpolateTo(const PositionPath* path, float y, float x, bool hasRotation) {
        PosRot current = active() ? data : PosRot{pos, yRot, xRot};
        PositionPath p = path != nullptr ? *path : PositionPath{current.pos, false, 0};
        float ty = hasRotation ? y : current.yRot, tx = hasRotation ? x : current.xRot;
        if (!active() || !(data.yRot == ty && data.xRot == tx && track::sameVec(data.pos, p.end))) {
            startInterpolating(p, ty, tx);
            setLast();
        }
    }

    // ClientPacketListener.handleMoveEntity / handleEntityPositionSync
    MCP_HD void receive(const EntityMove& m) {
        switch (m.kind) {
            case MoveKind::None:
                return;
            case MoveKind::Pos:
            case MoveKind::PosRot: {
                PositionPath p{track::decodeDelta(codecBase, m.xa, m.ya, m.za), m.stepped, m.tickOffset};
                codecBase = p.end;
                interpolateTo(&p, track::unpackDegrees(m.yRotn), track::unpackDegrees(m.xRotn), m.kind == MoveKind::PosRot);
                break;
            }
            case MoveKind::Rot:
                interpolateTo(nullptr, track::unpackDegrees(m.yRotn), track::unpackDegrees(m.xRotn), true);
                break;
            case MoveKind::Sync: {
                codecBase = m.path.end;
                Vec3 d{pos.x - m.path.end.x, pos.y - m.path.end.y, pos.z - m.path.end.z};
                if (d.lengthSqr() > 4096.0) unsupported = true;  // snapTo: never in an arena
                interpolateTo(&m.path, m.yRot, m.xRot, true);
                break;
            }
        }
    }

    // Entity.commonTick on the client: setOldPosAndRot, then interpolate().
    MCP_HD void clientTick() {
        old = pos;
        if (!active()) {  // cancel
            stepCount = 0;
            remainingTicks = 0.0F;
            interpolationSpeed = 1.0F;
            return;
        }
        // adjustInterpolationTargetFromDeltas: nothing moves a RemotePlayer
        // between interpolations, so the position delta is zero; the rotation
        // delta is still added (it can turn -0.0 into 0.0).
        Vec3 d{pos.x - lastPositionAndRotation.pos.x, pos.y - lastPositionAndRotation.pos.y, pos.z - lastPositionAndRotation.pos.z};
        if (d.lengthSqr() > static_cast<double>(1.0E-5F)) unsupported = true;
        float dy = yRot - lastPositionAndRotation.yRot, dx = xRot - lastPositionAndRotation.xRot;
        data.addRotation(dy, dx);
        for (int32_t i = 0; i < stepCount; ++i) steps[i].pr.addRotation(dy, dx);
        lastStep.addRotation(dy, dx);
        // doInterpolate
        PosRot target = next();
        pos = target.pos;
        setRot(target.yRot, target.xRot);
        advance(1.0F);  // Level.getRelativeTickSpeed
        setLast();
    }

    // InterpolationData.getNewPositionAndRotation
    MCP_HD PosRot next() {
        while (stepCount > 0) {
            const Step& s = steps[0];
            if (currentStepTicks < static_cast<float>(s.tickOffset)) {
                float a = currentStepTicks / static_cast<float>(s.tickOffset);
                return PosRot{track::lerp(lastStep.pos, s.pr.pos, static_cast<double>(a)), track::rotLerp(a, lastStep.yRot, s.pr.yRot),
                              track::lerpF(a, lastStep.xRot, s.pr.xRot)};
            }
            currentStepTicks -= static_cast<float>(s.tickOffset);
            lastStep = s.pr;
            for (int32_t i = 1; i < stepCount; ++i) steps[i - 1] = steps[i];
            stepCount--;
        }
        return data;
    }

    // InterpolationData.advance
    MCP_HD void advance(float ticks) {
        float t = remainingTicks / static_cast<float>(kInterpolationSteps);
        float targetSpeed = t > 1.0F ? t : 1.0F;  // Math.max (never NaN)
        interpolationSpeed = track::lerpF(1.0F / static_cast<float>(kInterpolationSteps), interpolationSpeed, targetSpeed);
        if (ticks * interpolationSpeed < remainingTicks) {
            ticks *= interpolationSpeed;
        } else {
            ticks = remainingTicks;
            interpolationSpeed = 1.0F;
        }
        currentStepTicks += ticks;
        remainingTicks -= ticks;
    }
};

}  // namespace mcp
