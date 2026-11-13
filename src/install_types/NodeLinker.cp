import bun from "bun";

const strings = bun.strings;

pub enum NodeLinker: u8 {
    auto,
    hoisted,
    isolated,

    pub static fn fromStr(input: []const u8): NodeLinker? {
        if (strings.eqlComptime(input, "hoisted")) {
            return .hoisted;
        }
        if (strings.eqlComptime(input, "isolated")) {
            return .isolated;
        }
        return null;
    }
}
