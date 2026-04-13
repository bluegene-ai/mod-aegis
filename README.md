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

Notes:

- The module is designed to work without AzerothCore core edits.
- The default mode is automatic handling enabled, but bans still require strong
  evidence and repeated offense history.
- Characters DB tables are used for event summaries and persistent offense
  state. Actual bans reuse AzerothCore's existing ban system via BanMgr.