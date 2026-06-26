-- make types larger so we can fit more values
SET @has_table := (SELECT COUNT(*) FROM information_schema.tables WHERE table_schema = DATABASE() AND table_name = 'smart_scripts');

SET @alter_sql_1 := IF(@has_table > 0, 'ALTER TABLE `smart_scripts` MODIFY COLUMN `action_type` int unsigned', 'SELECT 1');
PREPARE stmt1 FROM @alter_sql_1;
EXECUTE stmt1;
DEALLOCATE PREPARE stmt1;

SET @alter_sql_2 := IF(@has_table > 0, 'ALTER TABLE `smart_scripts` MODIFY COLUMN `event_type` int unsigned', 'SELECT 1');
PREPARE stmt2 FROM @alter_sql_2;
EXECUTE stmt2;
DEALLOCATE PREPARE stmt2;