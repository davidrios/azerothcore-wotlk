-- wowVR: self-service commands for every player (security 0).
-- .maxme, .maxtalents, .getitem and .finditem are implemented in src/server/scripts/Custom/cs_wowvr*.cpp.
-- Help texts use MySQL adjacent-literal concatenation to stay readable.
DELETE FROM `command` WHERE `name` IN ('maxme', 'maxtalents', 'getitem', 'finditem');
INSERT INTO `command` (`name`, `security`, `help`) VALUES
('maxme', 0,
    'Syntax: .maxme\r\n'
    'Reach the level cap and learn every class spell and weapon skill.\r\n'
    'Your talent points are left free to spend in the talent window.'),
('maxtalents', 0,
    'Syntax: .maxtalents\r\n'
    'Learn every talent of your class at its highest rank, in all three trees.'),
('getitem', 0,
    'Syntax: .getitem #itemlink|#itemid [#count]\r\n'
    'Put an item in your own bags (up to 1000 at once).\r\n'
    'Find items with .finditem, then shift-click the result into the chat box after .getitem.\r\n'
    'Example: .getitem 19019'),
('finditem', 0,
    'Syntax: .finditem #words [filters]\r\n'
    'Search items whose name contains all the words, in any order. Best matches come first.\r\n'
    'Filters:\r\n'
    '  q:#quality  at least this quality: poor, common, uncommon, rare, epic, legendary, heirloom\r\n'
    '  slot:#slot  head, neck, shoulder, back, chest, wrist, hands, waist, legs, feet, ring, trinket,\r\n'
    '              onehand, twohand, mainhand, offhand, shield, ranged, relic\r\n'
    '  ilvl:#n     item level #n or higher\r\n'
    '  usable      only items your character can use\r\n'
    '  p:#n        show page #n of the results\r\n'
    'Examples: .finditem thunderfury | .finditem sword q:epic usable | .finditem slot:head ilvl:226');

-- `.teleport <location>` only ever moves the player using it; its sub-commands keep their GM levels.
UPDATE `command` SET `security` = 0 WHERE `name` = 'teleport';

-- `.lookup item <name>` is read-only and prints item links that work with .getitem.
UPDATE `command` SET `security` = 0 WHERE `name` = 'lookup item';
