package mcporl.oracle;

import com.mojang.authlib.GameProfile;
import net.minecraft.util.Mth;
import net.minecraft.world.entity.Pose;
import net.minecraft.world.entity.ai.attributes.Attributes;
import net.minecraft.world.entity.player.Input;
import net.minecraft.world.entity.player.Player;
import net.minecraft.world.level.GameType;
import net.minecraft.world.level.Level;
import net.minecraft.world.phys.Vec2;

/**
 * A player driven by scripted key presses, behaving as the client's own
 * LocalPlayer does: it owns its movement and simulates it locally.
 *
 * All physics is vanilla. Only the client-side input handling from
 * LocalPlayer.aiStep/applyInput/modifyInput is ported here, because that code
 * lives in the client and not in the shared Player class. Ported pieces are
 * limited to what Phase 1 exercises; unsupported states throw instead of
 * silently producing a wrong trace.
 */
public final class OraclePlayer extends Player {
    private Input pendingKeys = Input.EMPTY;
    private Input keys = Input.EMPTY;
    private Vec2 moveVector = Vec2.ZERO;
    private boolean crouching;

    public OraclePlayer(Level level, GameProfile profile) {
        super(level, profile);
    }

    /** Keys held during the next tick (the client samples them inside aiStep). */
    public void setKeys(Input next) {
        this.pendingKeys = next;
    }

    public Input keys() {
        return keys;
    }

    public float moveX() {
        return moveVector.x;
    }

    public float moveY() {
        return moveVector.y;
    }

    // The local client is the authoritative side for its own player, so from
    // this player's point of view it is not "client-authoritative remote".
    // That makes isLocalInstanceAuthoritative() true, as for LocalPlayer.
    @Override
    public boolean isClientAuthoritative() {
        return false;
    }

    @Override
    public GameType gameMode() {
        return GameType.SURVIVAL;
    }

    // A client copy lives in the server level next to its own server twin (at the
    // same position), which a real client never sees. Player-to-player pushing is
    // out of scope for the oracle until it models remote-player interpolation.
    @Override
    protected void pushEntities() {
    }

    // LocalPlayer.getViewYRot: the crosshair follows the body yaw, not the
    // head yaw LivingEntity uses (which only catches up in aiStep).
    @Override
    public float getViewYRot(final float a) {
        return this.getYRot(a);
    }

    @Override
    public boolean isShiftKeyDown() {
        return keys.shift();
    }

    @Override
    public boolean isCrouching() {
        return crouching;
    }

    private boolean hasForwardImpulse() {
        return moveVector.y > 1.0E-5F;
    }

    private boolean isMovingSlowly() {
        return isCrouching() || isVisuallyCrawling();
    }

    // Port of LocalPlayer.aiStep, input-related parts only.
    @Override
    public void aiStep() {
        if (getAbilities().flying || getAbilities().mayfly || isPassenger() || isUsingItem() || isFallFlying()) {
            throw new IllegalStateException("oracle: state not supported by the Phase 1 input port");
        }
        boolean hadForwardImpulse = hasForwardImpulse();
        crouching = !isSwimming()
                && canPlayerFitWithinBlocksAndEntitiesWhen(Pose.CROUCHING)
                && (isShiftKeyDown() || !isSleeping() && !canPlayerFitWithinBlocksAndEntitiesWhen(Pose.STANDING));

        // LocalPlayer.moveTowardsClosestSpace is not ported: it only acts while the
        // player's corners are inside suffocating blocks, which Phase 1 arenas avoid.

        // KeyboardInput.tick
        keys = pendingKeys;
        float forward = impulse(keys.forward(), keys.backward());
        float left = impulse(keys.left(), keys.right());
        moveVector = new Vec2(left, forward).normalized();

        // Sprinting (double-tap-forward sprint is not modelled; the sprint key is).
        if (canStartSprinting() && keys.sprint()) {
            setSprinting(true);
        }
        if (isSprinting()) {
            if (isSwimming()) {
                throw new IllegalStateException("oracle: swim sprinting not supported yet");
            } else if (shouldStopRunSprinting()) {
                setSprinting(false);
            }
        }
        super.aiStep();
    }

    // Port of LocalPlayer.isSprintingPossible (private there).
    private boolean isSprintingPossible(boolean allowedInShallowWater) {
        return !isMobilityRestricted()
                && hasEnoughFoodToDoExhaustiveManoeuvres()
                && (allowedInShallowWater || !isInShallowWater());
    }

    private boolean canStartSprinting() {
        return !isSprinting()
                && hasForwardImpulse()
                && isSprintingPossible(getAbilities().flying)
                && (!isFallFlying() || isUnderWater())
                && (!isMovingSlowly() || isUnderWater());
    }

    private boolean shouldStopRunSprinting() {
        return !isSprintingPossible(getAbilities().flying)
                || !hasForwardImpulse()
                || horizontalCollision && !minorHorizontalCollision;
    }

    // Port of LocalPlayer.applyInput / modifyInput.
    @Override
    protected void applyInput() {
        Vec2 modified = modifyInput(moveVector);
        this.xxa = modified.x;
        this.zza = modified.y;
        this.jumping = keys.jump();
    }

    private Vec2 modifyInput(Vec2 input) {
        if (input.lengthSquared() == 0.0F) {
            return input;
        }
        Vec2 v = input.scale(0.98F);
        if (isMovingSlowly()) {
            v = v.scale((float) getAttributeValue(Attributes.SNEAKING_SPEED));
        }
        return modifyInputSpeedForSquareMovement(v);
    }

    private static Vec2 modifyInputSpeedForSquareMovement(Vec2 input) {
        float length = input.length();
        if (length <= 0.0F) {
            return input;
        }
        Vec2 direction = input.scale(1.0F / length);
        float d = distanceToUnitSquare(direction);
        float modifiedLength = Math.min(length * d, 1.0F);
        return direction.scale(modifiedLength);
    }

    private static float distanceToUnitSquare(Vec2 direction) {
        float x = Math.abs(direction.x);
        float y = Math.abs(direction.y);
        float tan = y > x ? x / y : y / x;
        return Mth.sqrt(1.0F + Mth.square(tan));
    }

    private static float impulse(boolean positive, boolean negative) {
        return positive == negative ? 0.0F : (positive ? 1.0F : -1.0F);
    }
}
