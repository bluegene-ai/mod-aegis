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
    float& hitX, float& hitY, float& hitZ) const
{
    Map* map = player ? player->GetMap() : nullptr;
    if (!player || !player->IsInWorld() || !map)
        return false;

    LineOfSightChecks checks = LINEOFSIGHT_CHECK_GOBJECT_ALL;
    if (sAcAegisConfig->Get().useVmaps)
        checks = LineOfSightChecks(checks | LINEOFSIGHT_CHECK_VMAP);

    bool clear = map->isInLineOfSight(
        startX, startY, startZ,
        endX, endY, endZ,
        player->GetPhaseMask(),
        checks,
        VMAP::ModelIgnoreFlags::Nothing);

    if (clear)
        return false;

    hitX = (startX + endX) * 0.5f;
    hitY = (startY + endY) * 0.5f;
    hitZ = (startZ + endZ) * 0.5f;
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
        hitX, hitY, hitZ);

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
        result.reason = "segment-blocked";
    else if (!reachable)
        result.reason = "segment-unreachable";
    else if (blocked)
        result.reason = "segment-hit-too-close";
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