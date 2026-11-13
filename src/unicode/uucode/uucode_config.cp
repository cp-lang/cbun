import config from "./config.zig";
import config_x from "../uucode_lib/src/x/config.x.zig";

const grapheme_break_no_control = config_x.grapheme_break_no_control;

pub const tables: [1]config.Table = [
    config.Table{
        name: "buildtime",
        extensions: &[
            grapheme_break_no_control,
        ],
        fields: &[
            grapheme_break_no_control.field("grapheme_break_no_control"),
        ],
    },
];
