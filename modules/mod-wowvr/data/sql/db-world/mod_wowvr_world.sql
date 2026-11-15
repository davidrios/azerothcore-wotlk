-- mod-wowvr: AntiDOS backstops for the WowVR opcodes (policy 1 = Kick, 5 = DropPacket; count per second)
DELETE FROM `antidos_opcode_policies` WHERE `Opcode` IN (1313, 1315, 1317);
INSERT INTO `antidos_opcode_policies` (`Opcode`, `Policy`, `MaxAllowedCount`) VALUES
(1313, 1, 5),  -- 0x521 CMSG_WOWVR_HELLO
(1315, 1, 10), -- 0x523 CMSG_WOWVR_STATE
(1317, 5, 60); -- 0x525 CMSG_WOWVR_POSE

DELETE FROM `command` WHERE `name` = 'wowvr status';
INSERT INTO `command` (`name`, `security`, `help`) VALUES
('wowvr status', 2, 'Syntax: .wowvr status\r\nList the sessions that negotiated the WowVR protocol, with their version, capabilities, state, and poses received and sent in the last second.');
