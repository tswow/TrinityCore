SET @trainer_has_table := (SELECT COUNT(*) FROM information_schema.tables WHERE table_schema = DATABASE() AND table_name = 'trainer');
SET @trainer_spell_has_table := (SELECT COUNT(*) FROM information_schema.tables WHERE table_schema = DATABASE() AND table_name = 'trainer_spell');

SET @alter_trainer_1 := IF(@trainer_has_table > 0, 'ALTER TABLE `trainer` ADD COLUMN `raceMask` INT(10) UNSIGNED NOT NULL DEFAULT "0"', 'SELECT 1');
PREPARE stmt1 FROM @alter_trainer_1;
EXECUTE stmt1;
DEALLOCATE PREPARE stmt1;

SET @alter_trainer_2 := IF(@trainer_has_table > 0, 'ALTER TABLE `trainer` ADD COLUMN `classMask` INT(10) UNSIGNED NOT NULL DEFAULT "0"', 'SELECT 1');
PREPARE stmt2 FROM @alter_trainer_2;
EXECUTE stmt2;
DEALLOCATE PREPARE stmt2;

SET @alter_trainer_spell_1 := IF(@trainer_spell_has_table > 0, 'ALTER TABLE `trainer_spell` ADD COLUMN `raceMask` INT(10) UNSIGNED NOT NULL DEFAULT "0"', 'SELECT 1');
PREPARE stmt3 FROM @alter_trainer_spell_1;
EXECUTE stmt3;
DEALLOCATE PREPARE stmt3;

SET @alter_trainer_spell_2 := IF(@trainer_spell_has_table > 0, 'ALTER TABLE `trainer_spell` ADD COLUMN `classMask` INT(10) UNSIGNED NOT NULL DEFAULT "0"', 'SELECT 1');
PREPARE stmt4 FROM @alter_trainer_spell_2;
EXECUTE stmt4;
DEALLOCATE PREPARE stmt4;