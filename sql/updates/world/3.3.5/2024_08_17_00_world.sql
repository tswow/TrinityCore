-- TDB 335.24081 world
SET @has_version := (
	SELECT COUNT(*)
	FROM information_schema.tables
	WHERE table_schema = DATABASE()
	  AND table_name = 'version'
);
SET @version_update_sql := IF(
	@has_version > 0,
	'UPDATE `version` SET `db_version`=''TDB 335.24081'', `cache_id`=24081 LIMIT 1',
	'SELECT 1'
);
PREPARE stmt FROM @version_update_sql;
EXECUTE stmt;
DEALLOCATE PREPARE stmt;
UPDATE `updates` SET `state`='ARCHIVED';
