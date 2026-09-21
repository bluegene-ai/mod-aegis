-- Adds the index used by AcAegisPersistence::LoadExpiredTempBans, which runs every
-- 5 seconds on the world thread. Without it that query is a full table scan.
--
-- Idempotent on purpose: fresh installs take the index from
-- base/001_ac_aegis_tables.sql, and this file must not fail when it is re-applied
-- after the base file already created the key.
SET @ac_aegis_has_ban_until_index := (
    SELECT COUNT(*)
    FROM information_schema.STATISTICS
    WHERE TABLE_SCHEMA = DATABASE()
      AND TABLE_NAME = 'ac_aegis_offense'
      AND INDEX_NAME = 'idx_ac_aegis_offense_ban_until'
);

SET @ac_aegis_add_ban_until_index := IF(
    @ac_aegis_has_ban_until_index = 0,
    'ALTER TABLE `ac_aegis_offense` ADD KEY `idx_ac_aegis_offense_ban_until` (`permanent_ban`, `ban_until`)',
    'SELECT 1'
);

PREPARE ac_aegis_stmt FROM @ac_aegis_add_ban_until_index;
EXECUTE ac_aegis_stmt;
DEALLOCATE PREPARE ac_aegis_stmt;

