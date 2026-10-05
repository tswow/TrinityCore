SET @npc_vendor_has_table := (SELECT COUNT(*) FROM information_schema.tables WHERE table_schema = DATABASE() AND table_name = 'npc_vendor');

SET @alter_vendor_1 := IF(@npc_vendor_has_table > 0, 'ALTER TABLE `npc_vendor` ADD COLUMN `raceMask` INT(10) UNSIGNED NOT NULL DEFAULT "0"', 'SELECT 1');
PREPARE stmt1 FROM @alter_vendor_1;
EXECUTE stmt1;
DEALLOCATE PREPARE stmt1;

SET @alter_vendor_2 := IF(@npc_vendor_has_table > 0, 'ALTER TABLE `npc_vendor` ADD COLUMN `classMask` INT(10) UNSIGNED NOT NULL DEFAULT "0"', 'SELECT 1');
PREPARE stmt2 FROM @alter_vendor_2;
EXECUTE stmt2;
DEALLOCATE PREPARE stmt2;