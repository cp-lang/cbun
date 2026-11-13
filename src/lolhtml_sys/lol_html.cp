import std from "std";
import bun from "bun";
import lolhtml_jsc from "../runtime/api/lolhtml_jsc.zig";

pub err HtmlError { Fail }
pub type Error = HtmlError;

pub @c struct MemorySettings {
    preallocated_parsing_buffer_size: usize,
    max_allowed_memory_usage: usize,
}

pub @c struct SourceLocationBytes {
    start: usize,
    end: usize,
}

inline fn auto_disable(): void {
    if (comptime bun.FeatureFlags.disable_lolhtml) unreachable;
}

const NULL_TERMINATED_SLICE: string = "\x00";

/// rust panics if the pointer itself is zero, even if the passed length is zero
/// to work around that, we use a static null-terminated pointer
fn ptrWithoutPanic(buf: []const u8): [*]const u8 {
    if (buf.len == 0) return NULL_TERMINATED_SLICE.ptr;
    return buf.ptr;
}

extern fn lol_html_rewriter_write(rewriter: *HTMLRewriter!, chunk: [*]const u8, chunk_len: usize): c_int;
extern fn lol_html_rewriter_end(rewriter: *HTMLRewriter!): c_int;
extern fn lol_html_rewriter_free(rewriter: *HTMLRewriter!): void;

extern fn lol_html_rewriter_builder_new(): *HTMLRewriterBuilder!;
extern fn lol_html_rewriter_builder_add_element_content_handlers(
    builder: *HTMLRewriterBuilder!,
    selector: *const HTMLSelector!,
    element_handler: lol_html_element_handler_t?,
    element_handler_user_data: *!?,
    comment_handler: lol_html_comment_handler_t?,
    comment_handler_user_data: *!?,
    text_handler: lol_html_text_handler_handler_t?,
    text_handler_user_data: *!?,
): c_int;
extern fn lol_html_rewriter_builder_free(builder: *HTMLRewriterBuilder!): void;
type OutputSinkFn = @c (([*]const u8, usize, *!): void);

extern fn lol_html_rewriter_build(
    builder: *HTMLRewriterBuilder!,
    encoding: [*]const u8,
    encoding_len: usize,
    memory_settings: MemorySettings,
    output_sink: OutputSinkFn?,
    output_sink_user_data: *!,
    strict: bool,
): (*HTMLRewriter!)?;
extern fn unstable_lol_html_rewriter_build_with_esi_tags(
    builder: *HTMLRewriterBuilder!,
    encoding: [*]const u8,
    encoding_len: usize,
    memory_settings: MemorySettings,
    output_sink: OutputSinkFn?,
    output_sink_user_data: *!,
    strict: bool,
): (*HTMLRewriter!)?;
extern fn lol_html_rewriter_builder_add_document_content_handlers(
    builder: *HTMLRewriterBuilder!,
    doctype_handler: DirectiveFunctionType(DocType)?,
    doctype_handler_user_data: *!?,
    comment_handler: lol_html_comment_handler_t?,
    comment_handler_user_data: *!?,
    text_handler: lol_html_text_handler_handler_t?,
    text_handler_user_data: *!?,
    doc_end_handler: lol_html_doc_end_handler_t?,
    doc_end_user_data: *!?,
): void;

pub @opaque struct HTMLRewriterBuilder {
    pub fn deinit(*this): void {
        auto_disable();
        unsafe { lol_html_rewriter_builder_free(this); }
    }

    pub static fn init(): *HTMLRewriterBuilder! {
        auto_disable();
        unsafe { return lol_html_rewriter_builder_new(); }
    }

    pub fn addDocumentContentHandlers(
        *this,
        comptime DocTypeHandler: type,
        comptime doctype_handler: DirectiveFunctionTypeForHandler(DocType, DocTypeHandler)?,
        doctype_handler_data: (*DocTypeHandler!)?,
        comptime CommentHandler: type,
        comptime comment_handler: DirectiveFunctionTypeForHandler(Comment, CommentHandler)?,
        comment_handler_data: (*CommentHandler!)?,
        comptime TextChunkHandler: type,
        comptime text_chunk_handler: DirectiveFunctionTypeForHandler(TextChunk, TextChunkHandler)?,
        text_chunk_handler_data: (*TextChunkHandler!)?,
        comptime DocEndHandler: type,
        comptime end_tag_handler: DirectiveFunctionTypeForHandler(DocEnd, DocEndHandler)?,
        end_tag_handler_data: (*DocEndHandler!)?,
    ): void {
        auto_disable();
        unsafe {
            lol_html_rewriter_builder_add_document_content_handlers(
                this,
                if (doctype_handler_data != null) DirectiveHandler(DocType, DocTypeHandler, doctype_handler.?) else null,
                doctype_handler_data,
                if (comment_handler_data != null) DirectiveHandler(Comment, CommentHandler, comment_handler.?) else null,
                comment_handler_data,
                if (text_chunk_handler_data != null) DirectiveHandler(TextChunk, TextChunkHandler, text_chunk_handler.?) else null,
                text_chunk_handler_data,
                if (end_tag_handler_data != null) DirectiveHandler(DocEnd, DocEndHandler, end_tag_handler.?) else null,
                end_tag_handler_data,
            );
        }
    }

    pub fn addElementContentHandlers(
        *this,
        selector: *HTMLSelector!,
        comptime ElementHandler: type,
        comptime element_handler: DirectiveFunctionTypeForHandler(Element, ElementHandler)?,
        element_handler_data: (*ElementHandler!)?,
        comptime CommentHandler: type,
        comptime comment_handler: DirectiveFunctionTypeForHandler(Comment, CommentHandler)?,
        comment_handler_data: (*CommentHandler!)?,
        comptime TextChunkHandler: type,
        comptime text_chunk_handler: DirectiveFunctionTypeForHandler(TextChunk, TextChunkHandler)?,
        text_chunk_handler_data: (*TextChunkHandler!)?,
    ): void ! HtmlError {
        auto_disable();
        unsafe {
            const rc = lol_html_rewriter_builder_add_element_content_handlers(
                this,
                selector,
                if (element_handler != null && element_handler_data != null) DirectiveHandler(Element, ElementHandler, element_handler.?) else null,
                element_handler_data,
                if (comment_handler != null && comment_handler_data != null) DirectiveHandler(Comment, CommentHandler, comment_handler.?) else null,
                comment_handler_data,
                if (text_chunk_handler != null && text_chunk_handler_data != null) DirectiveHandler(TextChunk, TextChunkHandler, text_chunk_handler.?) else null,
                text_chunk_handler_data,
            );
            if (rc == -1) return HtmlError.Fail;
            if (rc == 0) return;
            unreachable;
        }
    }

    pub fn build(
        *this,
        encoding: Encoding,
        memory_settings: MemorySettings,
        strict: bool,
        comptime OutputSink: type,
        output_sink: *OutputSink!,
        comptime Writer: *const (fn (*OutputSink, []const u8): void),
        comptime Done: *const (fn (*OutputSink): void),
    ): (*HTMLRewriter!) ! HtmlError {
        auto_disable();
        const encoding_ = Encoding.label.getAssertContains(encoding);
        unsafe {
            if (rw := lol_html_rewriter_build(
                this,
                encoding_.ptr,
                encoding_.len,
                memory_settings,
                HTMLRewriterBuilder.OutputSinkFunction(OutputSink, Writer, Done),
                output_sink,
                strict,
            )) {
                return rw;
            }
            return HtmlError.Fail;
        }
    }

    static fn OutputSinkFunction(
        comptime OutputSinkType: type,
        comptime Writer: *const (fn (*OutputSinkType, []const u8): void),
        comptime Done: *const (fn (*OutputSinkType): void),
    ): OutputSinkFn {
        return struct {
            @c fn writeChunk(ptr: [*]const u8, len: usize, user_data: *!): void {
                auto_disable();
                @setRuntimeSafety(false);
                unsafe {
                    const sink = user_data as *OutputSinkType!;
                    if (len == 0) {
                        Done(sink);
                    } else {
                        Writer(sink, ptr[0..len]);
                    }
                }
            }
        }.writeChunk;
    }
}

pub @opaque struct HTMLRewriter {
    pub const Builder = HTMLRewriterBuilder;

    pub fn write(*this, chunk: []const u8): void ! HtmlError {
        auto_disable();
        unsafe {
            const ptr = ptrWithoutPanic(chunk);
            const rc = lol_html_rewriter_write(this, ptr, chunk.len);
            if (rc < 0) return HtmlError.Fail;
        }
    }

    pub fn end(*this): void ! HtmlError {
        auto_disable();
        unsafe {
            if (lol_html_rewriter_end(this) < 0) return HtmlError.Fail;
        }
    }

    pub fn deinit(*this): void {
        auto_disable();
        unsafe { lol_html_rewriter_free(this); }
    }
}

extern fn lol_html_selector_parse(selector: [*]const u8, selector_len: usize): (*HTMLSelector!)?;
extern fn lol_html_selector_free(selector: *HTMLSelector!): void;

pub @opaque struct HTMLSelector {
    pub fn deinit(*this): void {
        auto_disable();
        unsafe { lol_html_selector_free(this); }
    }

    pub static fn parse(selector: []const u8): (*HTMLSelector!) ! HtmlError {
        auto_disable();
        unsafe {
            if (ptr := lol_html_selector_parse(ptrWithoutPanic(selector), selector.len)) {
                return ptr;
            }
            return HtmlError.Fail;
        }
    }
}

pub @c struct TextChunkContent {
    ptr: [*]const u8,
    len: usize,

    pub fn slice(this): []const u8 {
        auto_disable();
        unsafe { return this.ptr[0..this.len]; }
    }
}

extern fn lol_html_text_chunk_content_get(chunk: *const TextChunk!): TextChunkContent;
extern fn lol_html_text_chunk_is_last_in_text_node(chunk: *const TextChunk!): bool;
extern fn lol_html_text_chunk_before(chunk: *TextChunk!, content: [*]const u8, content_len: usize, is_html: bool): c_int;
extern fn lol_html_text_chunk_after(chunk: *TextChunk!, content: [*]const u8, content_len: usize, is_html: bool): c_int;
extern fn lol_html_text_chunk_replace(chunk: *TextChunk!, content: [*]const u8, content_len: usize, is_html: bool): c_int;
extern fn lol_html_text_chunk_remove(chunk: *TextChunk!): void;
extern fn lol_html_text_chunk_is_removed(chunk: *const TextChunk!): bool;
extern fn lol_html_text_chunk_user_data_set(chunk: *const TextChunk!, user_data: *!?): void;
extern fn lol_html_text_chunk_user_data_get(chunk: *const TextChunk!): *!?;
extern fn lol_html_text_chunk_source_location_bytes(chunk: *const TextChunk!): SourceLocationBytes;

pub @opaque struct TextChunk {
    pub const Content = TextChunkContent;

    pub fn getContent(this: &TextChunk): TextChunkContent {
        auto_disable();
        unsafe { return lol_html_text_chunk_content_get(this); }
    }
    pub fn isLastInTextNode(this: &TextChunk): bool {
        auto_disable();
        unsafe { return lol_html_text_chunk_is_last_in_text_node(this); }
    }
    pub fn before(*this, content: []const u8, is_html: bool): void ! HtmlError {
        auto_disable();
        unsafe {
            if (lol_html_text_chunk_before(this, ptrWithoutPanic(content), content.len, is_html) < 0) return HtmlError.Fail;
        }
    }
    pub fn after(*this, content: []const u8, is_html: bool): void ! HtmlError {
        auto_disable();
        unsafe {
            if (lol_html_text_chunk_after(this, ptrWithoutPanic(content), content.len, is_html) < 0) return HtmlError.Fail;
        }
    }
    pub fn replace(*this, content: []const u8, is_html: bool): void ! HtmlError {
        auto_disable();
        unsafe {
            if (lol_html_text_chunk_replace(this, ptrWithoutPanic(content), content.len, is_html) < 0) return HtmlError.Fail;
        }
    }
    pub fn remove(*this): void {
        auto_disable();
        unsafe { lol_html_text_chunk_remove(this); }
    }
    pub fn isRemoved(this: &TextChunk): bool {
        auto_disable();
        unsafe { return lol_html_text_chunk_is_removed(this); }
    }
    pub fn setUserData(this: &TextChunk, comptime Type: type, value: (*Type!)?): void {
        auto_disable();
        unsafe { lol_html_text_chunk_user_data_set(this, value); }
    }
    pub fn getUserData(this: &TextChunk, comptime Type: type): (*Type!)? {
        auto_disable();
        unsafe {
            if (raw := lol_html_text_chunk_user_data_get(this)) {
                return raw as *Type!;
            }
            return null;
        }
    }
    pub fn getSourceLocationBytes(this: &TextChunk): SourceLocationBytes {
        auto_disable();
        unsafe { return lol_html_text_chunk_source_location_bytes(this); }
    }
}

extern fn lol_html_element_get_attribute(element: *const Element!, name: [*]const u8, name_len: usize): HTMLString;
extern fn lol_html_element_has_attribute(element: *const Element!, name: [*]const u8, name_len: usize): c_int;
extern fn lol_html_element_set_attribute(element: *Element!, name: [*]const u8, name_len: usize, value: [*]const u8, value_len: usize): c_int;
extern fn lol_html_element_remove_attribute(element: *Element!, name: [*]const u8, name_len: usize): c_int;
extern fn lol_html_element_before(element: *Element!, content: [*]const u8, content_len: usize, is_html: bool): c_int;
extern fn lol_html_element_prepend(element: *Element!, content: [*]const u8, content_len: usize, is_html: bool): c_int;
extern fn lol_html_element_append(element: *Element!, content: [*]const u8, content_len: usize, is_html: bool): c_int;
extern fn lol_html_element_after(element: *Element!, content: [*]const u8, content_len: usize, is_html: bool): c_int;
extern fn lol_html_element_set_inner_content(element: *Element!, content: [*]const u8, content_len: usize, is_html: bool): c_int;
extern fn lol_html_element_replace(element: *Element!, content: [*]const u8, content_len: usize, is_html: bool): c_int;
extern fn lol_html_element_remove(element: *const Element!): void;
extern fn lol_html_element_remove_and_keep_content(element: *const Element!): void;
extern fn lol_html_element_is_removed(element: *const Element!): bool;
extern fn lol_html_element_is_self_closing(element: *const Element!): bool;
extern fn lol_html_element_can_have_content(element: *const Element!): bool;
extern fn lol_html_element_user_data_set(element: *const Element!, user_data: *!?): void;
extern fn lol_html_element_user_data_get(element: *const Element!): *!?;
extern fn lol_html_element_add_end_tag_handler(element: *Element!, end_tag_handler: lol_html_end_tag_handler_t, user_data: *!?): c_int;
extern fn lol_html_element_clear_end_tag_handlers(element: *Element!): void;
extern fn lol_html_element_source_location_bytes(element: *const Element!): SourceLocationBytes;
extern fn lol_html_element_tag_name_get(element: *const Element!): HTMLString;
extern fn lol_html_element_tag_name_set(element: *Element!, name: [*]const u8, name_len: usize): c_int;
extern fn lol_html_element_namespace_uri_get(element: *const Element!): cstr;
extern fn lol_html_attributes_iterator_get(element: *const Element!): (*AttributeIterator!)?;

fn checkElementResult(rc: c_int): void ! HtmlError {
    if (rc == 0) return;
    if (rc == -1) return HtmlError.Fail;
    unreachable;
}

pub @opaque struct Element {
    pub fn getAttribute(this: &Element, name: []const u8): HTMLString {
        auto_disable();
        unsafe { return lol_html_element_get_attribute(this, ptrWithoutPanic(name), name.len); }
    }
    pub fn hasAttribute(this: &Element, name: []const u8): bool ! HtmlError {
        auto_disable();
        unsafe {
            const rc = lol_html_element_has_attribute(this, ptrWithoutPanic(name), name.len);
            if (rc == 0) return false;
            if (rc == 1) return true;
            if (rc == -1) return HtmlError.Fail;
            unreachable;
        }
    }
    pub fn setAttribute(*this, name: []const u8, value: []const u8): void ! HtmlError {
        auto_disable();
        unsafe {
            return checkElementResult(lol_html_element_set_attribute(this, ptrWithoutPanic(name), name.len, ptrWithoutPanic(value), value.len));
        }
    }
    pub fn removeAttribute(*this, name: []const u8): void ! HtmlError {
        auto_disable();
        unsafe {
            return checkElementResult(lol_html_element_remove_attribute(this, ptrWithoutPanic(name), name.len));
        }
    }
    pub fn before(*this, content: []const u8, is_html: bool): void ! HtmlError {
        auto_disable();
        unsafe {
            return checkElementResult(lol_html_element_before(this, ptrWithoutPanic(content), content.len, is_html));
        }
    }
    pub fn prepend(*this, content: []const u8, is_html: bool): void ! HtmlError {
        auto_disable();
        unsafe {
            return checkElementResult(lol_html_element_prepend(this, ptrWithoutPanic(content), content.len, is_html));
        }
    }
    pub fn append(*this, content: []const u8, is_html: bool): void ! HtmlError {
        auto_disable();
        unsafe {
            return checkElementResult(lol_html_element_append(this, ptrWithoutPanic(content), content.len, is_html));
        }
    }
    pub fn after(*this, content: []const u8, is_html: bool): void ! HtmlError {
        auto_disable();
        unsafe {
            return checkElementResult(lol_html_element_after(this, ptrWithoutPanic(content), content.len, is_html));
        }
    }
    pub fn setInnerContent(*this, content: []const u8, is_html: bool): void ! HtmlError {
        auto_disable();
        unsafe {
            return checkElementResult(lol_html_element_set_inner_content(this, ptrWithoutPanic(content), content.len, is_html));
        }
    }
    pub fn replace(*this, content: []const u8, is_html: bool): void ! HtmlError {
        auto_disable();
        unsafe {
            return checkElementResult(lol_html_element_replace(this, ptrWithoutPanic(content), content.len, is_html));
        }
    }
    pub fn remove(this: &Element): void {
        auto_disable();
        unsafe { lol_html_element_remove(this); }
    }
    pub fn removeAndKeepContent(this: &Element): void {
        auto_disable();
        unsafe { lol_html_element_remove_and_keep_content(this); }
    }
    pub fn isRemoved(this: &Element): bool {
        auto_disable();
        unsafe { return lol_html_element_is_removed(this); }
    }
    pub fn isSelfClosing(this: &Element): bool {
        auto_disable();
        unsafe { return lol_html_element_is_self_closing(this); }
    }
    pub fn canHaveContent(this: &Element): bool {
        auto_disable();
        unsafe { return lol_html_element_can_have_content(this); }
    }
    pub fn setUserData(this: &Element, user_data: *!?): void {
        auto_disable();
        unsafe { lol_html_element_user_data_set(this, user_data); }
    }
    pub fn getUserData(this: &Element, comptime Type: type): (*Type!)? {
        auto_disable();
        unsafe {
            if (raw := lol_html_element_user_data_get(this)) {
                return raw as *Type!;
            }
            return null;
        }
    }
    pub fn onEndTag(*this, end_tag_handler: lol_html_end_tag_handler_t, user_data: *!?): void ! HtmlError {
        auto_disable();
        unsafe {
            lol_html_element_clear_end_tag_handlers(this);
            return checkElementResult(lol_html_element_add_end_tag_handler(this, end_tag_handler, user_data));
        }
    }
    pub fn tagName(this: &Element): HTMLString {
        unsafe { return lol_html_element_tag_name_get(this); }
    }
    pub fn setTagName(*this, name: []const u8): void ! HtmlError {
        unsafe {
            return checkElementResult(lol_html_element_tag_name_set(this, ptrWithoutPanic(name), name.len));
        }
    }
    pub fn namespaceURI(this: &Element): cstr {
        unsafe { return lol_html_element_namespace_uri_get(this); }
    }
    pub fn attributes(this: &Element): (*AttributeIterator!)? {
        unsafe { return lol_html_attributes_iterator_get(this); }
    }
    pub fn getSourceLocationBytes(this: &Element): SourceLocationBytes {
        auto_disable();
        unsafe { return lol_html_element_source_location_bytes(this); }
    }
}

extern fn lol_html_str_free(str: HTMLString): void;
pub extern fn lol_html_take_last_error(...): HTMLString;

pub @c struct HTMLString {
    ptr: [*]const u8,
    len: usize,

    pub fn deinit(this): void {
        auto_disable();
        unsafe { lol_html_str_free(this); }
    }

    pub static fn lastError(): HTMLString {
        auto_disable();
        unsafe { return lol_html_take_last_error(); }
    }

    pub fn slice(this): []const u8 {
        auto_disable();
        @setRuntimeSafety(false);
        unsafe { return this.ptr[0..this.len]; }
    }

    @c static fn deinit_external(_: [*]u8!, ptr: *!, len: u32): void {
        auto_disable();
        unsafe { lol_html_str_free(HTMLString{ ptr: ptr as [*]const u8!, len: len }); }
    }

    pub fn toString(this): bun.String {
        const bytes = this.slice();
        if (bytes.len > 0 && bun.strings.isAllASCII(bytes)) {
            unsafe {
                return bun.String.createExternal([*]u8, bytes, true, @constCast(bytes.ptr), &HTMLString.deinit_external);
            }
        }
        defer this.deinit();
        return bun.String.cloneUTF8(bytes);
    }

    pub const toJS = lolhtml_jsc.htmlStringToJS;
}

extern fn lol_html_end_tag_before(end_tag: *EndTag!, content: [*]const u8, content_len: usize, is_html: bool): c_int;
extern fn lol_html_end_tag_after(end_tag: *EndTag!, content: [*]const u8, content_len: usize, is_html: bool): c_int;
extern fn lol_html_end_tag_remove(end_tag: *EndTag!): void;
extern fn lol_html_end_tag_name_get(end_tag: *const EndTag!): HTMLString;
extern fn lol_html_end_tag_name_set(end_tag: *EndTag!, name: [*]const u8, name_len: usize): c_int;
extern fn lol_html_end_tag_source_location_bytes(end_tag: *const EndTag!): SourceLocationBytes;

pub @opaque struct EndTag {
    pub fn before(*this, content: []const u8, is_html: bool): void ! HtmlError {
        auto_disable();
        unsafe { return checkElementResult(lol_html_end_tag_before(this, ptrWithoutPanic(content), content.len, is_html)); }
    }
    pub fn after(*this, content: []const u8, is_html: bool): void ! HtmlError {
        auto_disable();
        unsafe { return checkElementResult(lol_html_end_tag_after(this, ptrWithoutPanic(content), content.len, is_html)); }
    }
    pub fn remove(*this): void {
        auto_disable();
        unsafe { lol_html_end_tag_remove(this); }
    }
    pub fn getName(this: &EndTag): HTMLString {
        auto_disable();
        unsafe { return lol_html_end_tag_name_get(this); }
    }
    pub fn setName(*this, name: []const u8): void ! HtmlError {
        auto_disable();
        unsafe { return checkElementResult(lol_html_end_tag_name_set(this, ptrWithoutPanic(name), name.len)); }
    }
    pub fn getSourceLocationBytes(this: &EndTag): SourceLocationBytes {
        auto_disable();
        unsafe { return lol_html_end_tag_source_location_bytes(this); }
    }
}

extern fn lol_html_attributes_iterator_free(iterator: *AttributeIterator!): void;
extern fn lol_html_attributes_iterator_next(iterator: *AttributeIterator!): (*const Attribute!)?;

pub @opaque struct AttributeIterator {
    pub fn next(*this): (*const Attribute!)? {
        auto_disable();
        unsafe { return lol_html_attributes_iterator_next(this); }
    }
    pub fn deinit(*this): void {
        auto_disable();
        unsafe { lol_html_attributes_iterator_free(this); }
    }
}

extern fn lol_html_attribute_name_get(attribute: *const Attribute!): HTMLString;
extern fn lol_html_attribute_value_get(attribute: *const Attribute!): HTMLString;

pub @opaque struct Attribute {
    pub const Iterator = AttributeIterator;

    pub fn name(this: &Attribute): HTMLString {
        auto_disable();
        unsafe { return lol_html_attribute_name_get(this); }
    }
    pub fn value(this: &Attribute): HTMLString {
        auto_disable();
        unsafe { return lol_html_attribute_value_get(this); }
    }
}

extern fn lol_html_comment_text_get(comment: *const Comment!): HTMLString;
extern fn lol_html_comment_text_set(comment: *Comment!, text: [*]const u8, text_len: usize): c_int;
extern fn lol_html_comment_before(comment: *Comment!, content: [*]const u8, content_len: usize, is_html: bool): c_int;
extern fn lol_html_comment_after(comment: *Comment!, content: [*]const u8, content_len: usize, is_html: bool): c_int;
extern fn lol_html_comment_replace(comment: *Comment!, content: [*]const u8, content_len: usize, is_html: bool): c_int;
extern fn lol_html_comment_remove(comment: *Comment!): void;
extern fn lol_html_comment_is_removed(comment: *const Comment!): bool;
extern fn lol_html_comment_user_data_set(comment: *const Comment!, user_data: *!?): void;
extern fn lol_html_comment_user_data_get(comment: *const Comment!): *!?;
extern fn lol_html_comment_source_location_bytes(comment: *const Comment!): SourceLocationBytes;

pub @opaque struct Comment {
    pub fn getText(this: &Comment): HTMLString {
        auto_disable();
        unsafe { return lol_html_comment_text_get(this); }
    }
    pub fn setText(*this, text: []const u8): void ! HtmlError {
        auto_disable();
        unsafe { return checkElementResult(lol_html_comment_text_set(this, ptrWithoutPanic(text), text.len)); }
    }
    pub fn before(*this, content: []const u8, is_html: bool): void ! HtmlError {
        auto_disable();
        unsafe { return checkElementResult(lol_html_comment_before(this, ptrWithoutPanic(content), content.len, is_html)); }
    }
    pub fn replace(*this, content: []const u8, is_html: bool): void ! HtmlError {
        auto_disable();
        unsafe { return checkElementResult(lol_html_comment_before(this, ptrWithoutPanic(content), content.len, is_html)); }
    }
    pub fn after(*this, content: []const u8, is_html: bool): void ! HtmlError {
        auto_disable();
        unsafe { return checkElementResult(lol_html_comment_after(this, ptrWithoutPanic(content), content.len, is_html)); }
    }
    pub const isRemoved = lol_html_comment_is_removed;
    pub const remove = lol_html_comment_remove;
    pub fn getSourceLocationBytes(this: &Comment): SourceLocationBytes {
        auto_disable();
        unsafe { return lol_html_comment_source_location_bytes(this); }
    }
}

pub enum Directive: c_uint {
    @"continue" = 0,
    stop = 1,
}

pub type lol_html_comment_handler_t = @c ((*Comment!, *!?): Directive);
pub type lol_html_text_handler_handler_t = @c ((*TextChunk!, *!?): Directive);
pub type lol_html_element_handler_t = @c ((*Element!, *!?): Directive);
pub type lol_html_doc_end_handler_t = @c ((*DocEnd!, *!?): Directive);
pub type lol_html_end_tag_handler_t = @c ((*EndTag!, *!?): Directive);

extern fn lol_html_doc_end_append(doc_end: (*DocEnd!)?, content: [*]const u8, content_len: usize, is_html: bool): c_int;

pub @opaque struct DocEnd {
    pub fn append(*this, content: []const u8, is_html: bool): void ! HtmlError {
        auto_disable();
        unsafe { return checkElementResult(lol_html_doc_end_append(this, ptrWithoutPanic(content), content.len, is_html)); }
    }
}

fn DirectiveFunctionType(comptime Container: type): type {
    return @c ((*Container, *!?): Directive);
}

fn DirectiveFunctionTypeForHandler(comptime Container: type, comptime UserDataType: type): type {
    return (fn (*UserDataType, *Container): bool);
}

fn DocTypeHandlerCallback(comptime UserDataType: type): type {
    return (fn (*DocType, *UserDataType): bool);
}

pub fn DirectiveHandler(
    comptime Container: type,
    comptime UserDataType: type,
    comptime Callback: *const (fn (*UserDataType, *Container): bool),
): DirectiveFunctionType(Container) {
    return struct {
        @c pub fn callback(container: *Container, user_data: *!?): Directive {
            auto_disable();
            unsafe {
                const ud = user_data.? as *UserDataType!;
                return (@intFromBool(Callback(ud, container)) as c_uint) as Directive;
            }
        }
    }.callback;
}

extern fn lol_html_doctype_name_get(doctype: *const DocType!): HTMLString;
extern fn lol_html_doctype_public_id_get(doctype: *const DocType!): HTMLString;
extern fn lol_html_doctype_system_id_get(doctype: *const DocType!): HTMLString;
extern fn lol_html_doctype_user_data_set(doctype: *const DocType!, user_data: *!?): void;
extern fn lol_html_doctype_user_data_get(doctype: *const DocType!): *!?;
extern fn lol_html_doctype_remove(doctype: *DocType!): void;
extern fn lol_html_doctype_is_removed(doctype: *const DocType!): bool;
extern fn lol_html_doctype_source_location_bytes(doctype: *const DocType!): SourceLocationBytes;

pub type DocTypeCallback = @c ((*DocType!, *!?): Directive);

pub @opaque struct DocType {
    pub const Callback = DocTypeCallback;

    pub fn getName(this: &DocType): HTMLString {
        auto_disable();
        unsafe { return lol_html_doctype_name_get(this); }
    }
    pub fn getPublicId(this: &DocType): HTMLString {
        auto_disable();
        unsafe { return lol_html_doctype_public_id_get(this); }
    }
    pub fn getSystemId(this: &DocType): HTMLString {
        auto_disable();
        unsafe { return lol_html_doctype_system_id_get(this); }
    }
    pub fn remove(*this): void {
        auto_disable();
        unsafe { lol_html_doctype_remove(this); }
    }
    pub fn isRemoved(this: &DocType): bool {
        auto_disable();
        unsafe { return lol_html_doctype_is_removed(this); }
    }
    pub fn getSourceLocationBytes(this: &DocType): SourceLocationBytes {
        auto_disable();
        unsafe { return lol_html_doctype_source_location_bytes(this); }
    }
}

const EncodingLabel = std.enums.EnumMap(Encoding, string);
const ENCODING_LABEL: EncodingLabel = brk: {
    let labels = EncodingLabel{};
    labels.put(.UTF8, "UTF-8");
    labels.put(.UTF16, "UTF-16");
    break :brk labels;
};

pub enum Encoding {
    UTF8,
    UTF16,
    pub const label = ENCODING_LABEL;
}
