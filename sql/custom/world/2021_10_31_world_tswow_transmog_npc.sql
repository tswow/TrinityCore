-- Only execute if trinity_string table exists
SET @has_trinity_string := (SELECT COUNT(*) FROM information_schema.tables WHERE table_schema = DATABASE() AND table_name = 'trinity_string');

SET @TEXT_ID := 65000;
SET @sql1 := IF(@has_trinity_string > 0, 'REPLACE INTO `npc_text` (`ID`, `text0_0`) VALUES (65000, "Transmogrification allows you to change how your items look like without changing the stats of the items. Items used in transmogrification are no longer refundable, tradeable and are bound to you. Updating a menu updates the view and prices.")', 'SELECT 1');
PREPARE stmt1 FROM @sql1;
EXECUTE stmt1;
DEALLOCATE PREPARE stmt1;

SET @sql2 := IF(@has_trinity_string > 0, 'REPLACE INTO `npc_text` (`ID`, `text0_0`) VALUES (65001, "You can save your own transmogrification sets. To save, first you must transmogrify your equipped items. Then when you go to the set management menu and go to save set menu, all items you have transmogrified are displayed.")', 'SELECT 1');
PREPARE stmt2 FROM @sql2;
EXECUTE stmt2;
DEALLOCATE PREPARE stmt2;

SET @sql3 := IF(@has_trinity_string > 0, 'REPLACE INTO `trinity_string` (`entry`, `content_default`) VALUES (11100, "Item transmogrified"), (11101, "Equipment slot is empty"), (11102, "Invalid source item selected"), (11103, "Source item does not exist"), (11104, "Destination item does not exist"), (11105, "Selected items are invalid"), (11106, "Not enough money"), (11107, "You do not have enough tokens"), (11108, "Transmogrifications removed"), (11109, "There are no transmogrifications"), (11110, "Invalid name inserted")', 'SELECT 1');
PREPARE stmt3 FROM @sql3;
EXECUTE stmt3;
DEALLOCATE PREPARE stmt3;

SET @sql4 := IF(@has_trinity_string > 0, 'INSERT IGNORE INTO `creature_template` (`entry`, `modelid1`, `modelid2`, `name`, `subname`, `IconName`, `gossip_menu_id`, `minlevel`, `maxlevel`, `exp`, `faction`, `npcflag`, `scale`, `rank`, `dmgschool`, `baseattacktime`, `rangeattacktime`, `unit_class`, `unit_flags`, `type`, `type_flags`, `lootid`, `pickpocketloot`, `skinloot`, `AIName`, `MovementType`, `HoverHeight`, `RacialLeader`, `movementId`, `RegenHealth`, `mechanic_immune_mask`, `flags_extra`, `ScriptName`) VALUES (190010, 19646, 0, "Warpweaver", "Transmogrifier", NULL, 0, 80, 80, 2, 35, 1, 1, 0, 0, 2000, 0, 1, 0, 7, 138936390, 0, 0, 0, "", 0, 1, 0, 0, 1, 0, 0, "Creature_Transmogrify")', 'SELECT 1');
PREPARE stmt4 FROM @sql4;
EXECUTE stmt4;
DEALLOCATE PREPARE stmt4;