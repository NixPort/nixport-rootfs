#ifndef NXP2_PROTOCOL_H
#define NXP2_PROTOCOL_H

#define NXP2_MAGIC "NXP2"
/* 版本语义（v3 起生效）：每个实现有两个常量——MIN 是它还愿意说的最老版本，
 * VERSION 是它会说的最新版本。协商规则：
 *   - HELLO 帧头写 MIN（发方此时还不知道对方的范围，写自己最老能说的版本，
 *     任何还愿意说 MIN 的对端都收得下）；握手之后的所有帧写协商结果。
 *   - 协商结果 = min(对端 max, 我方 max)，前提是双方区间有重叠。
 *   - 只在 v3 才存在的报文类型（NXP2_RUMBLE）只在协商结果 >= 3 时使用。
 * Version semantics (since v3): every implementation has two constants — MIN
 * is the oldest version it still speaks, VERSION the newest one it speaks.
 * The HELLO frame carries MIN in its header (the sender does not know the
 * peer's range yet), every frame after the handshake carries the negotiated
 * version = min(peer max, own max), provided the two ranges overlap. A
 * message type that only exists in v3 (NXP2_RUMBLE) is used only when the
 * negotiated version is >= 3. */
#define NXP2_VERSION_MIN 2
#define NXP2_VERSION 3
#define NXP2_HEADER_SIZE 28
#define NXP2_MAX_PACKET_SIZE 4096
#define NXP2_MAX_PAYLOAD_SIZE (NXP2_MAX_PACKET_SIZE - NXP2_HEADER_SIZE)
#define NXP2_MAX_PLAYERS 4
#define NXP2_MAX_TOUCHES 2
#define NXP2_PLAYER_RECORD_SIZE 48

enum nxp2_message_type {
    NXP2_HELLO = 1,
    NXP2_WELCOME = 2,
    NXP2_READY = 3,
    NXP2_SNAPSHOT = 4,
    NXP2_ACK = 5,
    NXP2_ERROR = 6,
    /* v3：客机 → 宿主的震动请求，载荷 4 字节（见下）。旧端不认识它，
     * 因此它只在协商结果 >= 3 时才出现。
     * v3: the guest's rumble request (guest -> host). Older peers do not know
     * this type, so it only appears on a link that negotiated >= 3. */
    NXP2_RUMBLE = 7
};

/* 震动报文的载荷（客机 → 宿主）：player(u8) + flags(u8, 保留，必须为 0)
 * + 强马达(u16) + 弱马达(u16)，均为小端。强度是 SDL 的 0..65535。
 * The rumble payload (guest -> host): player (u8), flags (u8, reserved, must
 * be 0), strong motor (u16), weak motor (u16), little endian. The intensities
 * are SDL's 0..65535. */
#define NXP2_RUMBLE_PAYLOAD_SIZE 6

enum nxp2_capability {
    NXP2_CAP_MULTI_PLAYER = 1u << 0,
    NXP2_CAP_TOUCH_CONTACTS = 1u << 1,
    NXP2_CAP_TOUCH_CLICK = 1u << 2,
    /* 能力位掩码不动：v3 的新特性按协商版本门控，不占能力位。
     * The capability mask is unchanged: a v3-only feature is gated by the
     * negotiated version, not by a capability bit. */
    NXP2_CAP_ALL = (1u << 3) - 1
};

enum nxp2_error {
    NXP2_ERR_BAD_MAGIC = 1,
    NXP2_ERR_BAD_VERSION = 2,
    NXP2_ERR_BAD_LENGTH = 3,
    NXP2_ERR_BAD_CAPABILITY = 4,
    NXP2_ERR_BAD_VALUE = 5,
    NXP2_ERR_UNSUPPORTED_MESSAGE = 6,
    NXP2_ERR_BAD_SEQUENCE = 7,
    NXP2_ERR_BAD_GENERATION = 8
};

enum nxp2_button {
    NXP2_BUTTON_SOUTH = 0,
    NXP2_BUTTON_EAST = 1,
    NXP2_BUTTON_WEST = 2,
    NXP2_BUTTON_NORTH = 3,
    NXP2_BUTTON_LEFT_SHOULDER = 4,
    NXP2_BUTTON_RIGHT_SHOULDER = 5,
    NXP2_BUTTON_LEFT_STICK = 6,
    NXP2_BUTTON_RIGHT_STICK = 7,
    NXP2_BUTTON_BACK = 8,
    NXP2_BUTTON_START = 9,
    NXP2_BUTTON_GUIDE = 10,
    NXP2_BUTTON_DPAD_UP = 11,
    NXP2_BUTTON_DPAD_DOWN = 12,
    NXP2_BUTTON_DPAD_LEFT = 13,
    NXP2_BUTTON_DPAD_RIGHT = 14,
    NXP2_BUTTON_MASK = (1u << 15) - 1
};

#endif
