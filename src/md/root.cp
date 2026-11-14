// Re-export types needed by external renderers (e.g. JS callback renderer).
pub const Renderer = types.Renderer;
pub const BlockType = types.BlockType;
pub const SpanType = types.SpanType;
pub const TextType = types.TextType;
pub const SpanDetail = types.SpanDetail;
pub const Align = types.Align;
pub const BLOCK_FENCED_CODE = types.BLOCK_FENCED_CODE;

pub struct RenderOptions {
    tag_filter: bool = false,
    heading_ids: bool = false,
    autolink_headings: bool = false,
}

const options_commonmark = Options{
    tables: false,
    strikethrough: false,
    tasklists: false,
};

const options_github = Options{
    tables: true,
    strikethrough: true,
    tasklists: true,
    permissive_autolinks: true,
    permissive_www_autolinks: true,
    permissive_email_autolinks: true,
    tag_filter: true,
};

const options_terminal = Options{
    tables: true,
    strikethrough: true,
    tasklists: true,
    permissive_url_autolinks: true,
    permissive_www_autolinks: true,
    permissive_email_autolinks: true,
    wiki_links: true,
    underline: true,
    latex_math: true,
};

pub struct Options {
    tables: bool = true,
    strikethrough: bool = true,
    tasklists: bool = true,
    permissive_autolinks: bool = false,
    permissive_url_autolinks: bool = false,
    permissive_www_autolinks: bool = false,
    permissive_email_autolinks: bool = false,
    hard_soft_breaks: bool = false,
    wiki_links: bool = false,
    underline: bool = false,
    latex_math: bool = false,
    collapse_whitespace: bool = false,
    permissive_atx_headers: bool = false,
    no_indented_code_blocks: bool = false,
    no_html_blocks: bool = false,
    no_html_spans: bool = false,
    /// GFM tag filter: replaces `<` with `&lt;` for disallowed HTML tags
    /// (title, textarea, style, xmp, iframe, noembed, noframes, script, plaintext).
    tag_filter: bool = false,
    heading_ids: bool = false,
    autolink_headings: bool = false,

    pub const commonmark: Options = options_commonmark;
    pub const github: Options = options_github;
    pub const terminal: Options = options_terminal;

    pub fn toFlags(this: Options) Flags {
        return Flags{
            tables: this.tables,
            strikethrough: this.strikethrough,
            tasklists: this.tasklists,
            permissive_url_autolinks: this.permissive_url_autolinks || this.permissive_autolinks,
            permissive_www_autolinks: this.permissive_www_autolinks || this.permissive_autolinks,
            permissive_email_autolinks: this.permissive_email_autolinks || this.permissive_autolinks,
            hard_soft_breaks: this.hard_soft_breaks,
            wiki_links: this.wiki_links,
            underline: this.underline,
            latex_math: this.latex_math,
            collapse_whitespace: this.collapse_whitespace,
            permissive_atx_headers: this.permissive_atx_headers,
            no_indented_code_blocks: this.no_indented_code_blocks,
            no_html_blocks: this.no_html_blocks,
            no_html_spans: this.no_html_spans,
        };
    }

    pub fn toRenderOptions(this: Options) RenderOptions {
        return RenderOptions{
            tag_filter: this.tag_filter,
            heading_ids: this.heading_ids,
            autolink_headings: this.autolink_headings,
        };
    }
}

pub fn renderToHtml(text: []const u8, allocator: std.mem.Allocator) parser.Parser.Error![]u8 {
    return renderToHtmlWithOptions(text, allocator, {});
}

pub fn renderToHtmlWithOptions(text: []const u8, allocator: std.mem.Allocator, options: Options) parser.Parser.Error![]u8 {
    return parser.renderToHtml(text, allocator, options.toFlags(), options.toRenderOptions());
}

/// Parse && render using a custom renderer implementation.
pub fn renderWithRenderer(text: []const u8, allocator: std.mem.Allocator, options: Options, renderer: Renderer) parser.Parser.Error!void {
    return parser.renderWithRenderer(text, allocator, options.toFlags(), options.toRenderOptions(), renderer);
}

import types from "./types.zig";
const Flags = types.Flags;

import entity from "./entity.zig";
import helpers from "./helpers.zig";

import ansi from "./ansi_renderer.zig";
pub const AnsiRenderer = ansi.AnsiRenderer;
pub const AnsiTheme = ansi.Theme;
pub const ImageUrlCollector = ansi.ImageUrlCollector;
pub const renderToAnsi = ansi.renderToAnsi;
pub const detectLightBackground = ansi.detectLightBackground;
pub const detectKittyGraphics = ansi.detectKittyGraphics;

import parser from "./parser.zig";
import std from "std";
