#include "AcAegisGeometry.h"

#include <array>
#include <cmath>
#include <limits>

#include "Optional.h"
#include "GameObject.h"
#include "Map.h"
#include "PathGenerator.h"
#include "Player.h"

#include "AcAegisConfig.h"

namespace
{
    float Dist3D(float ax, float ay, float az, float bx, float by, float bz)
    {
        float dx = ax - bx;
        float dy = ay - by;
        float dz = az - bz;
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    }
}

bool AcAegisGeometry::GetGroundHeight(Player* player, float x, float y, float z, float& groundZ) const
{
    Map* map = player ? player->GetMap() : nullptr;
    if (!player || !player->IsInWorld() || !map)
    {
        groundZ = z;
        return false;
    }

    PositionFullTerrainStatus terrainStatus;
    map->GetFullTerrainStatusForPosition(player->GetPhaseMask(),
        x, y, z, player->GetCollisionHeight(), terrainStatus);

    if (terrainStatus.floorZ > INVALID_HEIGHT)
    {
        groundZ = terrainStatus.floorZ;
        SanitizeGroundHeight(player, x, y, z, groundZ);
        return true;
    }

    groundZ = map->GetHeight(player->GetPhaseMask(), x, y, z);
    if (groundZ > INVALID_HEIGHT)
    {
        SanitizeGroundHeight(player, x, y, z, groundZ);
        return true;
    }

    return false;
}

void AcAegisGeometry::SanitizeGroundHeight(Player* player, float x, float y, float z, float& groundZ) const
{
    float heightAboveGround = z - groundZ;

    // If the reported ground is implausibly far below the player
    // (common in multi-story WMO dungeons where vmap hits a lower
    // floor or terrain base instead of the player's actual surface),
    // try a short-range raycast to find the nearest surface.
    constexpr float kImplausibleHeight = 20.0f;
    constexpr float kShortRaycastDist = 8.0f;

    if (heightAboveGround <= kImplausibleHeight)
        return;

    float hitX = 0.0f, hitY = 0.0f, hitZ = 0.0f;
    if (RaycastStaticAndDynamic(player,
            x, y, z + 0.5f,
            x, y, z - kShortRaycastDist,
            hitX, hitY, hitZ))
    {
        // A surface exists close below the player — the original
        // groundZ belongs to a different floor or terrain base.
        // Use the detected surface as the real ground reference.
        groundZ = hitZ;
        return;
    }

    // No surface found within short range — the player may genuinely
    // be high above any floor.  Keep the original groundZ so that
    // actual fly hacking can still be detected.
}

bool AcAegisGeometry::RaycastStaticAndDynamic(Player* player,
    float startX, float startY, float startZ,
    float endX, float endY, float endZ,
    float& hitX, float& hitY, float& hitZ,
    bool* hitPointValid,
    bool* staticBlocker) const
{
    Map* map = player ? player->GetMap() : nullptr;
    if (!player || !player->IsInWorld() || !map)
    {
        if (hitPointValid)
            *hitPointValid = false;
        if (staticBlocker)
            *staticBlocker = false;

        return false;
    }

    AegisConfig const& cfg = sAcAegisConfig->Get();

    LineOfSightChecks checks = LINEOFSIGHT_CHECK_GOBJECT_ALL;
    if (cfg.useVmaps)
        checks = LineOfSightChecks(checks | LINEOFSIGHT_CHECK_VMAP);

    bool clear = map->isInLineOfSight(
        startX, startY, startZ,
        endX, endY, endZ,
        player->GetPhaseMask(),
        checks,
        VMAP::ModelIgnoreFlags::Nothing);

    if (clear)
    {
        if (hitPointValid)
            *hitPointValid = false;
        if (staticBlocker)
            *staticBlocker = false;

        return false;
    }

    // The line of sight test above only answers yes/no. Resolve the real hit
    // position from the same collision trees so callers no longer have to treat a
    // synthetic midpoint as if it were a hit point.
    bool found = false;
    bool bestDynamic = false;
    float bestX = 0.0f;
    float bestY = 0.0f;
    float bestZ = 0.0f;
    float bestDistSq = std::numeric_limits<float>::max();

    auto considerHit = [&](float candidateX, float candidateY, float candidateZ, bool valid, bool dynamic)
    {
        if (!valid)
            return;

        float dx = candidateX - startX;
        float dy = candidateY - startY;
        float dz = candidateZ - startZ;
        float distSq = dx * dx + dy * dy + dz * dz;
        if (distSq < bestDistSq)
        {
            bestDistSq = distSq;
            bestX = candidateX;
            bestY = candidateY;
            bestZ = candidateZ;
            bestDynamic = dynamic;
            found = true;
        }
    };

    if (cfg.useVmaps)
    {
        float staticX = 0.0f;
        float staticY = 0.0f;
        float staticZ = 0.0f;
        bool staticHit = map->GetMapCollisionData().GetStaticTree().GetObjectHitPos(
            startX, startY, startZ, endX, endY, endZ,
            staticX, staticY, staticZ, 0.0f);
        considerHit(staticX, staticY, staticZ, staticHit, false);
    }

    {
        float dynamicX = 0.0f;
        float dynamicY = 0.0f;
        float dynamicZ = 0.0f;
        bool dynamicHit = map->GetMapCollisionData().GetDynamicTree().GetObjectHitPos(
            player->GetPhaseMask(),
            startX, startY, startZ, endX, endY, endZ,
            dynamicX, dynamicY, dynamicZ, 0.0f);
        considerHit(dynamicX, dynamicY, dynamicZ, dynamicHit, true);
    }

    if (found)
    {
        hitX = bestX;
        hitY = bestY;
        hitZ = bestZ;
    }
    else
    {
        // No tree reported a position even though the line was blocked. Fall back to
        // the segment midpoint and tell the caller the value is an approximation.
        hitX = (startX + endX) * 0.5f;
        hitY = (startY + endY) * 0.5f;
        hitZ = (startZ + endZ) * 0.5f;
    }

    if (hitPointValid)
        *hitPointValid = found;

    // Only a hit that we could positively attribute to the static tree counts as static;
    // an unattributable block must not be excused by the (static-only) navmesh.
    if (staticBlocker)
        *staticBlocker = found && !bestDynamic;

    return true;
}

AegisGeometryResult AcAegisGeometry::CheckShortSegment(Player* player,
    AegisMoveSample const& from,
    AegisMoveSample const& to,
    bool allowReachability) const
{
    AegisConfig const& cfg = sAcAegisConfig->Get();

    AegisGeometryResult result;
    result.checked = true;
    result.directDistance = Dist3D(from.x, from.y, from.z, to.x, to.y, to.z);

    float zOffset = std::max(0.0f, cfg.noClipRayZOffset);
    float hitX = 0.0f;
    float hitY = 0.0f;
    float hitZ = 0.0f;
    bool blocked = RaycastStaticAndDynamic(player,
        from.x, from.y, from.z + zOffset,
        to.x, to.y, to.z + zOffset,
        hitX, hitY, hitZ,
        &result.hitValid,
        &result.staticBlocker);

    float remainingDistance = 0.0f;
    if (blocked)
        remainingDistance = Dist3D(hitX, hitY, hitZ, to.x, to.y, to.z + zOffset);

    bool rayBlocked = blocked && remainingDistance >= cfg.noClipMinRemainingDistance;

    bool reachable = true;
    Map* map = (player && player->IsInWorld()) ? player->GetMap() : nullptr;
    if (allowReachability && player && map)
    {
        float rx = to.x;
        float ry = to.y;
        float rz = to.z;
        reachable = map->CanReachPositionAndGetValidCoords(
            player,
            from.x, from.y, from.z,
            rx, ry, rz,
            true,
            true);
    }

    result.blocked = rayBlocked || !reachable;
    result.reachable = reachable;
    result.pathExists = reachable;
    result.hitX = hitX;
    result.hitY = hitY;
    result.hitZ = hitZ;
    if (rayBlocked)
        result.reason = result.hitValid ? "segment-blocked" : "segment-blocked-approx";
    else if (!reachable)
        result.reason = "segment-unreachable";
    else if (blocked)
        result.reason = result.hitValid ? "segment-hit-too-close" : "segment-hit-too-close-approx";
    else
        result.reason = "segment-clear";

    return result;
}

AegisGeometryResult AcAegisGeometry::CheckLongPath(Player* player,
    AegisMoveSample const& from,
    AegisMoveSample const& to) const
{
    AegisConfig const& cfg = sAcAegisConfig->Get();

    AegisGeometryResult result;
    result.checked = true;
    result.directDistance = Dist3D(from.x, from.y, from.z, to.x, to.y, to.z);

    if (!player)
    {
        result.pathLength = result.directDistance;
        result.reason = "path-no-player";
        return result;
    }

    if (!cfg.useMmaps)
    {
        result.checked = false;
        result.pathLength = result.directDistance;
        result.reason = "mmap-disabled";
        return result;
    }

    PathGenerator path(player);
    bool ok = path.CalculatePath(from.x, from.y, from.z, to.x, to.y, to.z, false);
    result.pathExists = ok;
    result.reachable = ok;
    result.pathLength = ok ? path.getPathLength() : 0.0f;

    if (!ok)
    {
        result.blocked = true;
        result.reason = "path-not-found";
        return result;
    }

    // A blocked straight segment is only evidence when the walkable detour is out of reach
    // as well. The previous test used a fixed ratio to the straight distance
    // (pathLength > directDistance * 2.8), which scales with the segment instead of with the
    // time the unit had: on a 4 yard step it tolerated an 11 yard detour - roughly three
    // times what the movement speed can cover in that window - while still flagging the
    // curved route a player runs around a tree, a crystal or a corner. Compare against the
    // distance the unit can actually cover between the two samples instead: the same speed
    // tolerance the NoClip detector already grants (MaxSpeedMultiplier), plus a small
    // allowance for navmesh approximation. Crossing a wall needs a detour far beyond that.
    float const dtSeconds = to.serverMs > from.serverMs ?
        static_cast<float>(to.serverMs - from.serverMs) / 1000.0f : 0.0f;
    float const allowedSpeed = std::max(from.allowedSpeed, to.allowedSpeed);
    float const budget = std::max(result.directDistance + cfg.pathBudgetSlackYards,
        allowedSpeed * dtSeconds * cfg.pathBudgetSpeedFactor);

    result.blocked = result.pathLength > budget;
    result.reason = result.blocked ? "path-too-long" : "path-ok";
    return result;
}

AegisDoorCrossResult AcAegisGeometry::CheckClosedDoorCross(Player* player,
    AegisMoveSample const& from,
    AegisMoveSample const& to,
    float hitX, float hitY, float hitZ, bool hitValid) const
{
    AegisDoorCrossResult result;

    AegisConfig const& cfg = sAcAegisConfig->Get();
    if (!cfg.noClipDoorCrossEnabled || !player || !player->IsInWorld())
        return result;

    // Doors are modelled from their base upward; the crossing has to happen inside that
    // vertical band. Doors are 3-4 yards tall, so a band starting slightly below the base
    // is enough and nobody walks through a door at a height where the model has no door.
    constexpr float kDoorBaseTolerance = 1.0f;
    constexpr float kDoorTopYards = 4.5f;
    constexpr float kHitToleranceYards = 0.75f;

    float const direct = Dist3D(from.x, from.y, from.z, to.x, to.y, to.z);
    float const searchRange = direct + cfg.noClipDoorCrossHalfWidthYards;

    // Only the nearest door is inspected: doors are sparse and the inspected segment is a
    // few yards long, so the door being crossed is normally the nearest one. Missing one
    // only costs detection - this rule never invents a crossing.
    GameObject* door = player->FindNearestGameObjectOfType(GAMEOBJECT_TYPE_DOOR, searchRange);
    if (!door || !door->IsInWorld() || door->GetMap() != player->GetMap())
        return result;

    // An open door (in either active state) is passable by design.
    if (door->GetGoState() != GO_STATE_READY)
        return result;

    if (!(door->GetPhaseMask() & player->GetPhaseMask()))
        return result;

    // WoW positions use x = x0 + d * cos(o), y = y0 + d * sin(o), so (cos o, sin o) is the
    // door's facing normal and (-sin o, cos o) its width axis.
    float const orientation = door->GetOrientation();
    float const normalX = std::cos(orientation);
    float const normalY = std::sin(orientation);
    float const widthX = -normalY;
    float const widthY = normalX;

    float const doorX = door->GetPositionX();
    float const doorY = door->GetPositionY();
    float const doorZ = door->GetPositionZ();

    float const fromSide = (from.x - doorX) * normalX + (from.y - doorY) * normalY;
    float const toSide = (to.x - doorX) * normalX + (to.y - doorY) * normalY;
    if ((fromSide > 0.0f) == (toSide > 0.0f))
        return result;                                  // both endpoints on the same side

    float const denominator = fromSide - toSide;
    if (std::fabs(denominator) < 0.0001f)
        return result;

    float const t = fromSide / denominator;
    if (t <= 0.0f || t >= 1.0f)
        return result;

    float const crossZ = from.z + (to.z - from.z) * t;
    if (crossZ < doorZ - kDoorBaseTolerance || crossZ > doorZ + kDoorTopYards)
        return result;

    float const crossX = from.x + (to.x - from.x) * t;
    float const crossY = from.y + (to.y - from.y) * t;
    float const lateral = std::fabs((crossX - doorX) * widthX + (crossY - doorY) * widthY);
    if (lateral > cfg.noClipDoorCrossHalfWidthYards)
        return result;                                  // walked past beside the door

    result.door = door;
    result.crossed = true;
    result.lateralOffset = lateral;

    // Corroboration from the collision layer: its own hit point sits on this door's slab.
    if (hitValid)
    {
        float const hitOffsetX = hitX - doorX;
        float const hitOffsetY = hitY - doorY;
        float const hitLateral = std::fabs(hitOffsetX * widthX + hitOffsetY * widthY);
        float const hitPlane = std::fabs(hitOffsetX * normalX + hitOffsetY * normalY);
        if (hitLateral <= cfg.noClipDoorCrossHalfWidthYards + kHitToleranceYards &&
            hitPlane <= kHitToleranceYards &&
            hitZ > doorZ - kDoorBaseTolerance - kHitToleranceYards &&
            hitZ < doorZ + kDoorTopYards + kHitToleranceYards)
        {
            result.collisionConfirmed = true;
            result.hitPlaneDistance = hitPlane;
        }
    }

    return result;
}