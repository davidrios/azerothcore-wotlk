-- wowVR: let every player keep the extra talents from .maxtalents.
-- Without permission 21 (Skip check: more talents than allowed), the next login or level change
-- resets talents that exceed the character's talent points. 199 is the player commands role,
-- which every account inherits; permission 21 gates nothing else.
DELETE FROM `rbac_linked_permissions` WHERE `id` = 199 AND `linkedId` = 21;
INSERT INTO `rbac_linked_permissions` (`id`, `linkedId`) VALUES (199, 21);
