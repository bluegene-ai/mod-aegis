# mod-ac-aegis

mod-ac-aegis is a new AzerothCore anti-cheat module designed around three
principles:

- reuse AzerothCore movement semantics instead of inventing a parallel ruleset;
- keep hot-path checks light and reserve geometry or path checks for suspicious
  segments only;
- use progressive punishments with persistent offense history, instead of
  jumping from one noisy hit to an irreversible ban.

The module combines:

- AcSentinel's evidence aggregation, geometry recheck mindset, and staged
  punishment model;
- mod-anticheat's stricter single-event coverage for teleport, fly, climb, and
  server-order related movement anomalies.

Primary coverage in the current implementation:

- speed hack
- teleport / coordinate jump
- wall or door clipping
- illegal fly or air suspension
- wall climb and super jump
- long-duration fixed-position gathering bot patterns

Main references:

- docs/PLAN.md
- docs/DESIGN.md
- docs/BLUEPRINT.md

Module layout:

- conf/AcAegis.conf.dist
- data/sql/db-characters/base/001_ac_aegis_tables.sql
- src/

Usage:

- Copy or merge the module settings from conf/AcAegis.conf.dist into your
  server configuration before enabling automatic actions in production.
- Build and load the module together with the rest of AzerothCore. The module
  does not patch core tables and does not require AzerothCore source edits.
- Apply the characters database SQL manually before first startup. Aegis will
  not create or alter its tables at runtime.

Initial SQL install:

1. Import data/sql/db-characters/base/001_ac_aegis_tables.sql into the
  characters database before starting worldserver on a fresh install.
2. Verify that both ac_aegis_offense and ac_aegis_event exist in the
  characters database.
3. Start worldserver only after the base tables are present.

Upgrade and migration workflow:

1. Never rely on worldserver startup to create or modify Aegis tables. All
  schema changes must ship as SQL files.
2. Keep data/sql/db-characters/base/001_ac_aegis_tables.sql as the full fresh
  install schema for the current module release.
3. For any released schema change after the base file already exists in the
  field, add an incremental SQL migration under AzerothCore's characters
  update flow, normally data/sql/updates/pending_db_characters while the
  change is under review.
4. After the SQL update is merged upstream, follow AzerothCore convention and
  move that migration into data/sql/updates/db_characters for released
  history.
5. Existing servers must apply every new incremental migration in order during
  upgrade; they should not re-import the base file over a live database.
6. Fresh installs may import only the latest base file, plus any newer update
  files that were added after that base snapshot was produced.

Release rule for future Aegis table changes:

1. If you add, remove, or alter any ac_aegis_* column, index, or table, ship a
  matching characters SQL migration in the same release.
2. Update data/sql/db-characters/base/001_ac_aegis_tables.sql to reflect the
  new final schema for fresh installs.
3. Also ship an incremental ALTER or data backfill script for upgrades, so
  existing realms can move forward without dropping historical data.
4. Document the migration in this README and in release notes whenever the
  change affects operators.
5. Do not edit unrelated core SQL files and do not place Aegis schema changes
  outside the AzerothCore characters update directories.

Recommended upgrade procedure:

1. Stop worldserver.
2. Back up the characters database.
3. Apply the new Aegis SQL update files for the target release.
4. Deploy the new module binary and config changes.
5. Start worldserver and confirm the module loads without schema warnings.

Notes:

- The module is designed to work without AzerothCore core edits.
- The default mode is automatic handling enabled, but bans still require strong
  evidence and repeated offense history.
- Runtime schema creation is disabled; apply table changes only through the SQL
  migration files shipped under data/sql.
- AcAegis.Geometry.UseMmaps controls whether long suspicious micro-paths are
  rechecked through MMAP pathfinding before they are escalated as blocked-path
  evidence.
- AcAegis.Offense.CountOnlyOnPunish controls whether offense history grows only
  for punishment-stage decisions, or also for rollback-only observe decisions.
- Characters DB tables are used for event summaries and persistent offense
  state. Actual bans reuse AzerothCore's existing ban system via BanMgr.
- File logs are buffered and written by a background worker thread instead of
  synchronously reopening the file on every append.
- Event rows are queued and written to the characters database in background
  batches with a bounded queue, instead of synchronously inserting one row per
  detection on the hot path.
- Punishment world broadcasts are controlled by a dedicated config switch, remain
  enabled by default, and use the configurable
  `AcAegis.AutoAction.Broadcast.Format` template.

## Detection, punishment and the enabled switch

AcAegis.Enabled only gates **new detection**:

- with the switch off no new evidence is produced, no new offense is recorded and
  no new punishment is started;
- a punishment that is already in its cycle keeps running until it expires,
  including across a relog: an active debuff is re-applied on login, an active
  jail sentence is restored and its escape teleports stay blocked, and an expired
  temporary ban is still lifted by the offline sweep.

To release every currently punished player immediately, use `.aegis purge`
(administrator) or clear the `ac_aegis_offense` table by hand. Flipping
`AcAegis.Enabled` is not an emergency stop for active punishments by design.

## Detection quality notes

- Punishment is decided while the evidence is produced but **executed on the next
  world update**, so debuffs, teleports and kicks never run inside a loot,
  gathering or movement callback. At most one punishment per player is executed
  per tick.
- The rollback target is recorded only from samples that produced no evidence, so
  `Rollback` moves the player back to the last clean position instead of
  re-teleporting them onto the flagged one.
- Graces are scoped: the mount and whitelisted-spell graces only silence the
  aerial/mount detectors, and the server-issued displacement grace (charge, jump,
  knockback, pull, spell teleport) comes from the passive anticheat hook rather
  than from a client acknowledgement.
- Slow fall / feather fall auras no longer exempt a player from wall and door
  clipping detection.
- Root-break detection uses the server immobilisation state (`UNIT_STATE_ROOT`,
  stun, root aura). `Player::IsRooted()` cannot be used because the core strips
  `MOVEMENTFLAG_ROOT` from every client movement packet.
- `AcAegis.Risk.OffenseTierFloor` lets the persistent offense ladder raise the
  punishment floor for the current evidence. Without it the risk gate, which is
  in practice an event-rate gate, cancels every prior-tier promotion and
  intermittent cheaters are never escalated.
- `AcAegis.Risk.StrongEvidenceFloor` closes the remaining hole in that ladder.
  The offense floor needs an existing tier, and a tier only grows after a
  punishment, which needs risk, which an event interval above roughly 24 seconds
  never produces — so a first-time intermittent cheater could never be punished at
  all. With this switch on, Strong evidence whose own family floor is already jail
  or higher (coordinate teleport, stationary coordinate shift, unreachable micro
  path, low gravity jump, blocked wall climb) is treated as high risk on its own
  and is no longer cancelled by event frequency. It is restricted to the
  movement/geometry families; the behavioral gather heuristic stays fully gated,
  and bans still additionally require `Ban.StrongEvidenceRequired` and
  `Ban.MinOffenseCount`, so a first Strong event can reach at most Kick.
- The AFK detector fires when a character never leaves one spot across the whole
  window. That is deliberate policy on this realm: camping a spawn point is not
  allowed, so standing still is itself the violation, and the detector must clear it
  rather than block it. Two window-wide facts keep it off legitimate play:
  `maxMoveInWindow` is the largest distance reached at any counted action (not the
  final position), so "gather, step away, come back" cannot hide; and an action taken
  while in combat is not counted at all and also restarts the window, so a camp that
  fights can never accumulate the action/loot/gather minimums. A player who gathers
  while travelling leaves `MaxMoveDistance`, which resets the window entirely.
- `.aegis delete` and `.aegis purge` wait at most 5 seconds for the queued event rows
  to be handed to the asynchronous queue. On expiry the command still runs and logs a
  warning naming the guid and the timeout, so the operator is told the delete may not
  have covered rows still in flight rather than being left to assume it did.
- The detection state is only reset when a real movement boundary changes.
  `AcAegis` used to reset it unconditionally from the shared
  `AnticheatSetUnderACKmount` hook, which the core calls from 28 sites including
  every speed-aura amount recalculation
  (`AuraEffect::HandleAuraModIncreaseSpeed` / `HandleAuraModIncreaseFlightSpeed`
  cover `SPELL_AURA_MOD_INCREASE_SPEED`, `_MOUNTED_SPEED`, `_SPEED_ALWAYS`,
  `_MOUNTED_SPEED_ALWAYS`, `_SPEED_NOT_STACK`, `_MOUNTED_SPEED_NOT_STACK`,
  `_MINIMUM_SPEED` and the six flight speed auras). That cleared the sample chain
  and every hit window on each recalculation, which made all of the windowed
  detectors unreachable for anyone carrying a speed aura.
- Ground lookups are cached per player (`AcAegis.Sampling.GroundCacheTtlMs`,
  `AcAegis.Sampling.GroundCacheRadius`) because
  `Map::GetFullTerrainStatusForPosition` is a VMAP-heavy query; set the TTL to 0
  to disable the cache. **The radius must exceed the distance covered inside the
  TTL or the cache never hits while moving**: at `MOVE_RUN` 7 yd/s a 250 ms window
  moves 1.75 yards, so the previous 1.5 yard default missed on essentially every
  packet and the hot path kept paying one full terrain query per movement packet.
  Flight and transport movement do not use the cache at all.
- Geometry uses the real collision hit position from
  `MapCollisionData::GetStaticTree()/GetDynamicTree()`, so
  `AcAegis.Detector.NoClip.MinRemainingDistance` measures the real distance from
  the hit point to the destination.

## Known limits

- Core and script code that moves a player with `Unit::NearTeleportTo` does not
  run the `OnPlayerBeforeTeleport` hook (only `Player::TeleportTo` does), so Aegis
  gets no teleport grace for it. This covers battleground spawn and fence resets,
  vehicle relocations, transports, the Warlock demonic circle and a number of boss
  mechanics. A displacement produced that way can be classified as a coordinate
  teleport if it exceeds the axis or distance thresholds.
- `AcAegis.AutoAction.Ban.Mode = character` only writes the `character_banned`
  table, which does not by itself block login. Only `account` and the default
  `account-by-character` write `account_banned`, which is what the auth server
  checks. Keep the default unless you specifically want the weaker behaviour.
- On maps where mounts are allowed, `DetectMount` can only observe, not punish:
  the strong branch requires an instance template with `AllowMount = 0`.
- `DetectTime` only covers a clear client clock lead (ratio >= 1.35 and a lead of
  at least 180 ms); a mild time multiplier below that is not detected.
- `ac_aegis_event` has no retention policy and grows without bound. Plan an
  external cleanup such as
  `DELETE FROM ac_aegis_event WHERE created_at < NOW() - INTERVAL 30 DAY`.

## GM command security

- `.aegis clear` is available to gamemasters and clears the accumulated offense,
  the debuff and the jail sentence. Lifting the underlying core (BanMgr) ban
  requires SEC_ADMINISTRATOR.
- `.aegis delete` and `.aegis purge` require SEC_ADMINISTRATOR and also lift the
  core bans Aegis recorded. Ban state is only cleared when the stored ban mode is
  known, so an unrelated manual ban is never removed by accident.

## Shutdown behaviour

The background event writer and the file log appender are stopped from
`WORLDHOOK_ON_SHUTDOWN`, which runs before `CharacterDatabase.Close()`. Queued
event rows are handed to the asynchronous character database queue and buffered
log lines are flushed while the database is still open.