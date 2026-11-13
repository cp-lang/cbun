export enum PacketType: u8 {
    OK = 0x00,
    EOF = 0xfe,
    ERROR = 0xff,
    LOCAL_INFILE = 0xfb,

    HANDSHAKE = 0x0a,
    MORE_DATA = 0x01,

    _,
    pub const AUTH_SWITCH = 0xfe;
}
