package mcporl.oracle;

import com.mojang.authlib.GameProfile;
import java.lang.reflect.Constructor;
import net.minecraft.world.entity.Entity;
import net.minecraft.world.entity.InterpolationHandler;
import net.minecraft.world.entity.SteppedInterpolationHandler;
import net.minecraft.world.entity.player.Player;
import net.minecraft.world.level.GameType;
import net.minecraft.world.level.Level;

/**
 * How one client sees the other player: vanilla's RemotePlayer, reduced to
 * what decides where its hitbox is.
 *
 * RemotePlayer is noPhysics and never moves itself; its position comes only
 * from the client-side SteppedInterpolationHandler, fed the tracker's move and
 * position-sync packets (ClientPacketListener.handleMoveEntity /
 * handleEntityPositionSync) and advanced once per client tick in
 * Entity.commonTick. RemotePlayer.aiStep's pushEntities has no effect on
 * positions: Entity.push skips noPhysics entities, and on a client only the
 * local player is pushable (EntitySelector.pushableBy).
 *
 * The view lives in the server level (the oracle has no client level), so
 * SteppedInterpolationHandler.create would hand out the server-side tracker;
 * the client-side handler is built directly instead.
 */
public final class RemoteView extends Player {
    private static final Constructor<?> CLIENT_HANDLER;

    static {
        try {
            CLIENT_HANDLER = SteppedInterpolationHandler.class.getDeclaredConstructor(Entity.class);
            CLIENT_HANDLER.setAccessible(true);
        } catch (NoSuchMethodException e) {
            throw new ExceptionInInitializerError(e);
        }
    }

    public RemoteView(Level level, GameProfile profile) {
        super(level, profile);
        this.noPhysics = true;
    }

    @Override
    protected InterpolationHandler createInterpolationHandler() {
        try {
            return (InterpolationHandler) CLIENT_HANDLER.newInstance(this);
        } catch (ReflectiveOperationException e) {
            throw new IllegalStateException(e);
        }
    }

    /** Entity.commonTick on a client: setOldPosAndRot, then interpolate. */
    public void clientCommonTick() {
        setOldPosAndRot();
        getInterpolation().interpolate();
        tickCount++;
    }

    @Override
    public boolean isClientAuthoritative() {
        return false;
    }

    @Override
    public GameType gameMode() {
        return GameType.SURVIVAL;
    }
}
