/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef MOD_WOWVR_PROTOCOL_H
#define MOD_WOWVR_PROTOCOL_H

#include "Define.h"
#include <array>
#include <cstddef>

// WowVR multiplayer protocol extension, v1. The contract lives in the WowVR repository,
// docs/vr-multiplayer-protocol.md. Everything is little-endian.
namespace WowVR
{
    constexpr uint16 PROTOCOL_VERSION = 1;

    constexpr char const* ANNOUNCE_PREFIX = "WOWVR";

    // CMSG_WOWVR_HELLO / SMSG_WOWVR_HELLO capabilities
    enum Capability : uint32
    {
        CAP_SENDS_POSES = 0x1,
        CAP_WANTS_POSES = 0x2,
        CAP_MASK_V1     = CAP_SENDS_POSES | CAP_WANTS_POSES
    };

    // State byte, in CMSG_WOWVR_STATE and in every pose
    enum StateFlag : uint8
    {
        STATE_IN_VR         = 0x01,
        STATE_SEATED        = 0x02,
        STATE_LEFT_TRACKED  = 0x04,
        STATE_RIGHT_TRACKED = 0x08,
        STATE_POINTING      = 0x10
    };

    enum PosePart : uint8
    {
        PART_BODY       = 0,
        PART_HEAD       = 1,
        PART_LEFT_HAND  = 2,
        PART_RIGHT_HAND = 3,
        PART_TRIGGERS   = 4,
        PART_FINGERS    = 5,
        PART_POINTER     = 6,
        PART_HIPS        = 7,
        PART_CHEST       = 8,
        PART_LEFT_FOOT   = 9,
        PART_RIGHT_FOOT  = 10,
        PART_LEFT_KNEE   = 11,
        PART_RIGHT_KNEE  = 12,
        PART_LEFT_ELBOW  = 13,
        PART_RIGHT_ELBOW = 14,
        PART_COUNT_V1    = 15
    };

    // Bit 15 is refused.
    constexpr uint16 PARTS_MASK_V1 = (1 << PART_COUNT_V1) - 1;

    constexpr std::array<uint8, PART_COUNT_V1> PART_SIZES = { 6, 10, 10, 10, 4, 10, 3, 10, 10, 10, 10, 6, 6, 6, 6 };

    // sequence (2), time (2), state (1), parts (2)
    constexpr std::size_t POSE_HEADER_SIZE = 7;
    constexpr std::size_t POSE_STATE_OFFSET = 4;
    constexpr std::size_t POSE_PARTS_OFFSET = 5;
    constexpr std::size_t POSE_MAX_SIZE = POSE_HEADER_SIZE + 6 + 10 * 3 + 4 + 10 + 3 + 10 * 4 + 6 * 4;
    static_assert(POSE_MAX_SIZE == 124, "the largest v1 pose is 124 bytes");

    constexpr std::size_t CMSG_HELLO_SIZE = 6; // version (2), capabilities (4)
    constexpr std::size_t CMSG_STATE_SIZE = 1; // state (1)

    enum class PoseCheck
    {
        Ok,
        BadLength,
        UnknownPart,
        OutOfBounds
    };

    inline int16 ReadInt16(uint8 const* p)
    {
        return static_cast<int16>(static_cast<uint16>(p[0]) | static_cast<uint16>(p[1]) << 8);
    }

    inline uint16 ReadUInt16(uint8 const* p)
    {
        return static_cast<uint16>(p[0]) | static_cast<uint16>(p[1]) << 8;
    }

    inline uint32 ReadUInt32(uint8 const* p)
    {
        return static_cast<uint32>(p[0]) | static_cast<uint32>(p[1]) << 8
            | static_cast<uint32>(p[2]) << 16 | static_cast<uint32>(p[3]) << 24;
    }

    // Checks a CMSG_WOWVR_POSE payload's length, part bits and position bounds (section 3.3).
    // maxOffsetMilli is WowVR.MaxOffset in thousandths of a yard. Rotations are not checked.
    inline PoseCheck CheckPose(uint8 const* data, std::size_t size, int32 maxOffsetMilli)
    {
        if (size < POSE_HEADER_SIZE)
            return PoseCheck::BadLength;

        uint16 const parts = ReadUInt16(data + POSE_PARTS_OFFSET);
        if (parts & ~PARTS_MASK_V1)
            return PoseCheck::UnknownPart;

        std::size_t expected = POSE_HEADER_SIZE;
        for (uint8 part = 0; part < PART_COUNT_V1; ++part)
            if (parts & (1 << part))
                expected += PART_SIZES[part];

        if (size != expected)
            return PoseCheck::BadLength;

        int64 const maxSq = static_cast<int64>(maxOffsetMilli) * maxOffsetMilli;
        uint8 const* p = data + POSE_HEADER_SIZE;
        for (uint8 part = 0; part < PART_COUNT_V1; ++part)
        {
            if (!(parts & (1 << part)))
                continue;

            switch (part)
            {
                case PART_BODY:
                {
                    int64 const x = ReadInt16(p);
                    int64 const y = ReadInt16(p + 2);
                    if (x * x + y * y > maxSq)
                        return PoseCheck::OutOfBounds;
                    break;
                }
                case PART_TRIGGERS:
                case PART_FINGERS:
                case PART_POINTER:
                    break;
                default: // every other part starts with a position
                {
                    int64 const x = ReadInt16(p);
                    int64 const y = ReadInt16(p + 2);
                    int64 const z = ReadInt16(p + 4);
                    if (x * x + y * y + z * z > maxSq)
                        return PoseCheck::OutOfBounds;
                    break;
                }
            }

            p += PART_SIZES[part];
        }

        return PoseCheck::Ok;
    }
}

#endif
