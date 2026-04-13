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
- Punishment world broadcasts are now controlled by a dedicated config switch
  and remain enabled by default.