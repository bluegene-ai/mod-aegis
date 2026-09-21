#include "AcAegisGeometry.h"

#include <array>
#include <cmath>
#include <limits>

#include "Optional.h"
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
    bool* hitPointValid) const
{
    Map* map = player ? player->GetMap() : nullptr;
    if (!player || !player->IsInWorld() || !map)
    {
        if (hitPointValid)
            *hitPointValid = false;

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

        return false;
    }

    // The line of sight test above only answers yes/no. Resolve the real hit
    // position from the same collision trees so callers no longer have to treat a
    // synthetic midpoint as if it were a hit point.
    bool found = false;
    float bestX = 0.0f;
    float bestY = 0.0f;
    float bestZ = 0.0f;
    float bestDistSq = std::numeric_limits<float>::max();

    auto considerHit = [&](float candidateX, float candidateY, float candidateZ, bool valid)
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
        considerHit(staticX, staticY, staticZ, staticHit);
    }

    {
        float dynamicX = 0.0f;
        float dynamicY = 0.0f;
        float dynamicZ = 0.0f;
        bool dynamicHit = map->GetMapCollisionData().GetDynamicTree().GetObjectHitPos(
            player->GetPhaseMask(),
            startX, startY, startZ, endX, endY, endZ,
            dynamicX, dynamicY, dynamicZ, 0.0f);
        considerHit(dynamicX, dynamicY, dynamicZ, dynamicHit);
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
        &result.hitValid);

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

    result.blocked = result.directDistance > 0.1f && result.pathLength > (result.directDistance * 2.8f);
    result.reason = result.blocked ? "path-too-long" : "path-ok";
    return result;
}