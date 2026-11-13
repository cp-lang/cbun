import std from "std";

type NullableOpaque = *!?;

pub extern fn mi_malloc(size: usize): *!?;
pub extern fn mi_calloc(count: usize, size: usize): *!?;
pub extern fn mi_realloc(p: *!?, newsize: usize): *!?;
pub extern fn mi_expand(p: *!?, newsize: usize): *!?;
pub extern fn mi_free(p: *!?): void;
pub extern fn mi_strdup(s: [*c]const u8): [*c]u8;
pub extern fn mi_strndup(s: [*c]const u8, n: usize): [*c]u8;
pub extern fn mi_realpath(fname: [*c]const u8, resolved_name: [*c]u8): [*c]u8;
pub extern fn mi_malloc_small(size: usize): *!?;
pub extern fn mi_zalloc_small(size: usize): *!?;
pub extern fn mi_zalloc(size: usize): *!?;
pub extern fn mi_mallocn(count: usize, size: usize): *!?;
pub extern fn mi_reallocn(p: *!?, count: usize, size: usize): *!?;
pub extern fn mi_reallocf(p: *!?, newsize: usize): *!?;
pub extern fn mi_usable_size(p: (&*!)?): usize;
pub extern fn mi_good_size(size: usize): usize;
pub type mi_deferred_free_fun = *const @c fn (bool, c_ulonglong, *!?) void;
pub extern fn mi_register_deferred_free(deferred_free: mi_deferred_free_fun?, arg: *!?): void;
pub type mi_output_fun = *const @c fn (cstr, *!?) void;
pub extern fn mi_register_output(out: mi_output_fun?, arg: *!?): void;
pub type mi_error_fun = *const @c fn (c_int, *!?) void;
pub extern fn mi_register_error(fun: mi_error_fun?, arg: *!?): void;
pub extern fn mi_collect(force: bool): void;
pub extern fn mi_version(): c_int;
pub extern fn mi_stats_reset(): void;
pub extern fn mi_stats_merge(): void;
pub extern fn mi_stats_print(out: *!?): void;
pub extern fn mi_stats_print_out(out: mi_output_fun?, arg: *!?): void;
pub extern fn mi_process_init(): void;
pub extern fn mi_thread_init(): void;
pub extern fn mi_thread_done(): void;
pub extern fn mi_thread_stats_print_out(out: mi_output_fun?, arg: *!?): void;
pub extern fn mi_process_info(elapsed_msecs: [*c]usize, user_msecs: [*c]usize, system_msecs: [*c]usize, current_rss: [*c]usize, peak_rss: [*c]usize, current_commit: [*c]usize, peak_commit: [*c]usize, page_faults: [*c]usize): void;
pub extern fn mi_malloc_aligned(size: usize, alignment: usize): *!?;
pub extern fn mi_malloc_aligned_at(size: usize, alignment: usize, offset: usize): *!?;
pub extern fn mi_zalloc_aligned(size: usize, alignment: usize): *!?;
pub extern fn mi_zalloc_aligned_at(size: usize, alignment: usize, offset: usize): *!?;
pub extern fn mi_calloc_aligned(count: usize, size: usize, alignment: usize): *!?;
pub extern fn mi_calloc_aligned_at(count: usize, size: usize, alignment: usize, offset: usize): *!?;
pub extern fn mi_realloc_aligned(p: *!?, newsize: usize, alignment: usize): *!?;
pub extern fn mi_realloc_aligned_at(p: *!?, newsize: usize, alignment: usize, offset: usize): *!?;

pub @opaque struct Heap {
    pub static fn new(): (*Heap!)? {
        unsafe { return mi_heap_new(); }
    }

    pub fn delete(*this) {
        unsafe { mi_heap_delete(this); }
    }

    pub fn malloc(*this, size: usize): *!? {
        unsafe { return mi_heap_malloc(this, size); }
    }

    pub fn calloc(*this, count: usize, size: usize): *!? {
        unsafe { return mi_heap_calloc(this, count, size); }
    }

    pub fn realloc(*this, p: *!?, newsize: usize): *!? {
        unsafe { return mi_heap_realloc(this, p, newsize); }
    }

    pub fn isOwned(*this, p: (&*!)?): bool {
        unsafe { return mi_heap_contains(this, p); }
    }
}

pub extern fn mi_heap_new(): (*Heap!)?;
pub extern fn mi_heap_delete(heap: *Heap!): void;
pub extern fn mi_heap_destroy(heap: *Heap!): void;
pub extern fn mi_heap_main(): *Heap!;
pub extern fn mi_heap_contains(heap: &Heap, p: (&*!)?): bool;
pub extern fn mi_heap_collect(heap: *Heap!, force: bool): void;
pub extern fn mi_heap_malloc(heap: *Heap!, size: usize): *!?;
pub extern fn mi_heap_zalloc(heap: *Heap!, size: usize): *!?;
pub extern fn mi_heap_calloc(heap: *Heap!, count: usize, size: usize): *!?;
pub extern fn mi_heap_mallocn(heap: *Heap!, count: usize, size: usize): *!?;
pub extern fn mi_heap_malloc_small(heap: *Heap!, size: usize): *!?;
pub extern fn mi_heap_realloc(heap: *Heap!, p: *!?, newsize: usize): *!?;
pub extern fn mi_heap_reallocn(heap: *Heap!, p: *!?, count: usize, size: usize): *!?;
pub extern fn mi_heap_reallocf(heap: *Heap!, p: *!?, newsize: usize): *!?;
pub extern fn mi_heap_strdup(heap: *Heap!, s: [*c]const u8): [*c]u8;
pub extern fn mi_heap_strndup(heap: *Heap!, s: [*c]const u8, n: usize): [*c]u8;
pub extern fn mi_heap_realpath(heap: *Heap!, fname: [*c]const u8, resolved_name: [*c]u8): [*c]u8;
pub extern fn mi_heap_malloc_aligned(heap: *Heap!, size: usize, alignment: usize): *!?;
pub extern fn mi_heap_malloc_aligned_at(heap: *Heap!, size: usize, alignment: usize, offset: usize): *!?;
pub extern fn mi_heap_zalloc_aligned(heap: *Heap!, size: usize, alignment: usize): *!?;
pub extern fn mi_heap_zalloc_aligned_at(heap: *Heap!, size: usize, alignment: usize, offset: usize): *!?;
pub extern fn mi_heap_calloc_aligned(heap: *Heap!, count: usize, size: usize, alignment: usize): *!?;
pub extern fn mi_heap_calloc_aligned_at(heap: *Heap!, count: usize, size: usize, alignment: usize, offset: usize): *!?;
pub extern fn mi_heap_realloc_aligned(heap: *Heap!, p: *!?, newsize: usize, alignment: usize): *!?;
pub extern fn mi_heap_realloc_aligned_at(heap: *Heap!, p: *!?, newsize: usize, alignment: usize, offset: usize): *!?;
pub extern fn mi_rezalloc(p: *!?, newsize: usize): *!?;
pub extern fn mi_recalloc(p: *!?, newcount: usize, size: usize): *!?;
pub extern fn mi_rezalloc_aligned(p: *!?, newsize: usize, alignment: usize): *!?;
pub extern fn mi_rezalloc_aligned_at(p: *!?, newsize: usize, alignment: usize, offset: usize): *!?;
pub extern fn mi_recalloc_aligned(p: *!?, newcount: usize, size: usize, alignment: usize): *!?;
pub extern fn mi_recalloc_aligned_at(p: *!?, newcount: usize, size: usize, alignment: usize, offset: usize): *!?;
pub extern fn mi_heap_rezalloc(heap: *Heap!, p: *!?, newsize: usize): *!?;
pub extern fn mi_heap_recalloc(heap: *Heap!, p: *!?, newcount: usize, size: usize): *!?;
pub extern fn mi_heap_rezalloc_aligned(heap: *Heap!, p: *!?, newsize: usize, alignment: usize): *!?;
pub extern fn mi_heap_rezalloc_aligned_at(heap: *Heap!, p: *!?, newsize: usize, alignment: usize, offset: usize): *!?;
pub extern fn mi_heap_recalloc_aligned(heap: *Heap!, p: *!?, newcount: usize, size: usize, alignment: usize): *!?;
pub extern fn mi_heap_recalloc_aligned_at(heap: *Heap!, p: *!?, newcount: usize, size: usize, alignment: usize, offset: usize): *!?;
pub extern fn mi_check_owned(p: (&*!)?): bool;

pub @c struct struct_mi_heap_area_s {
    blocks: *!?,
    reserved: usize,
    committed: usize,
    used: usize,
    block_size: usize,
    full_block_size: usize,
    reserved1: *!?,
}
pub type mi_heap_area_t = struct_mi_heap_area_s;
pub type mi_block_visit_fun = *const @c fn ((&Heap)?, [*c]const mi_heap_area_t, *!?, usize, *!?) bool;
pub extern fn mi_heap_visit_blocks(heap: (&Heap)?, visit_all_blocks: bool, visitor: mi_block_visit_fun?, arg: *!?): bool;
pub extern fn mi_is_in_heap_region(p: (&*!)?): bool;
pub extern fn mi_is_redirected(): bool;
pub extern fn mi_reserve_huge_os_pages_interleave(pages: usize, numa_nodes: usize, timeout_msecs: usize): c_int;
pub extern fn mi_reserve_huge_os_pages_at(pages: usize, numa_node: c_int, timeout_msecs: usize): c_int;
pub extern fn mi_reserve_os_memory(size: usize, commit: bool, allow_large: bool): c_int;
pub extern fn mi_manage_os_memory(start: *!?, size: usize, is_committed: bool, is_large: bool, is_zero: bool, numa_node: c_int): bool;
pub extern fn mi_debug_show_arenas(): void;
pub type ArenaID = *!?;
pub extern fn mi_arena_area(arena_id: ArenaID, size: *usize!): *!?;
pub extern fn mi_reserve_huge_os_pages_at_ex(pages: usize, numa_node: c_int, timeout_msecs: usize, exclusive: bool, arena_id: *ArenaID!): c_int;
pub extern fn mi_reserve_os_memory_ex(size: usize, commit: bool, allow_large: bool, exclusive: bool, arena_id: *ArenaID!): c_int;
pub extern fn mi_manage_os_memory_ex(start: *!?, size: usize, is_committed: bool, is_large: bool, is_zero: bool, numa_node: c_int, exclusive: bool, arena_id: *ArenaID!): bool;
pub extern fn mi_heap_new_in_arena(arena_id: ArenaID): (*Heap!)?;
pub extern fn mi_reserve_huge_os_pages(pages: usize, max_secs: f64, pages_reserved: [*c]usize): c_int;
pub extern fn mi_thread_set_in_threadpool(): void;

pub enum Option: c_uint {
    show_errors = 0,
    show_stats = 1,
    verbose = 2,
    eager_commit = 3,
    arena_eager_commit = 4,
    purge_decommits = 5,
    allow_large_os_pages = 6,
    reserve_huge_os_pages = 7,
    reserve_huge_os_pages_at = 8,
    reserve_os_memory = 9,
    deprecated_segment_cache = 10,
    deprecated_page_reset = 11,
    abandoned_page_purge = 12,
    deprecated_segment_reset = 13,
    eager_commit_delay = 14,
    purge_delay = 15,
    use_numa_nodes = 16,
    disallow_os_alloc = 17,
    os_tag = 18,
    max_errors = 19,
    max_warnings = 20,
    deprecated_max_segment_reclaim = 21,
    destroy_on_exit = 22,
    arena_reserve = 23,
    arena_purge_mult = 24,
    deprecated_purge_extend_delay = 25,
    disallow_arena_alloc = 26,
    retry_on_oom = 27,
    visit_abandoned = 28,
    guarded_min = 29,
    guarded_max = 30,
    guarded_precise = 31,
    guarded_sample_rate = 32,
    guarded_sample_seed = 33,
    generic_collect = 34,
    page_reclaim_on_free = 35,
    page_full_retain = 36,
    page_max_candidates = 37,
    max_vabits = 38,
    pagemap_commit = 39,
    page_commit_on_demand = 40,
    page_max_reclaim = 41,
    page_cross_thread_max_reclaim = 42,
}

pub extern fn mi_option_is_enabled(option: Option): bool;
pub extern fn mi_option_enable(option: Option): void;
pub extern fn mi_option_disable(option: Option): void;
pub extern fn mi_option_set_enabled(option: Option, enable: bool): void;
pub extern fn mi_option_set_enabled_default(option: Option, enable: bool): void;
pub extern fn mi_option_get(option: Option): c_long;
pub extern fn mi_option_get_clamp(option: Option, min: c_long, max: c_long): c_long;
pub extern fn mi_option_set(option: Option, value: c_long): void;
pub extern fn mi_option_set_default(option: Option, value: c_long): void;
pub extern fn mi_cfree(p: *!?): void;
pub extern fn mi__expand(p: *!?, newsize: usize): *!?;
pub extern fn mi_malloc_size(p: (&*!)?): usize;
pub extern fn mi_malloc_good_size(size: usize): usize;
pub extern fn mi_malloc_usable_size(p: (&*!)?): usize;
pub extern fn mi_posix_memalign(p: [*c]NullableOpaque, alignment: usize, size: usize): c_int;
pub extern fn mi_memalign(alignment: usize, size: usize): *!?;
pub extern fn mi_valloc(size: usize): *!?;
pub extern fn mi_pvalloc(size: usize): *!?;
pub extern fn mi_aligned_alloc(alignment: usize, size: usize): *!?;
pub extern fn mi_reallocarray(p: *!?, count: usize, size: usize): *!?;
pub extern fn mi_reallocarr(p: *!?, count: usize, size: usize): c_int;
pub extern fn mi_aligned_recalloc(p: *!?, newcount: usize, size: usize, alignment: usize): *!?;
pub extern fn mi_aligned_offset_recalloc(p: *!?, newcount: usize, size: usize, alignment: usize, offset: usize): *!?;
pub extern fn mi_wcsdup(s: [*c]const c_ushort): [*c]c_ushort;
pub extern fn mi_mbsdup(s: [*c]const u8): [*c]u8;
pub extern fn mi_dupenv_s(buf: [*c][*c]u8, size: [*c]usize, name: [*c]const u8): c_int;
pub extern fn mi_wdupenv_s(buf: [*c][*c]c_ushort, size: [*c]usize, name: [*c]const c_ushort): c_int;
pub extern fn mi_free_size(p: *!?, size: usize): void;
pub extern fn mi_free_size_aligned(p: *!?, size: usize, alignment: usize): void;
pub extern fn mi_free_aligned(p: *!?, alignment: usize): void;
pub extern fn mi_new(size: usize): *!?;
pub extern fn mi_new_aligned(size: usize, alignment: usize): *!?;
pub extern fn mi_new_nothrow(size: usize): *!?;
pub extern fn mi_new_aligned_nothrow(size: usize, alignment: usize): *!?;
pub extern fn mi_new_n(count: usize, size: usize): *!?;
pub extern fn mi_new_realloc(p: *!?, newsize: usize): *!?;
pub extern fn mi_new_reallocn(p: *!?, newcount: usize, size: usize): *!?;

pub const MI_SMALL_WSIZE_MAX: c_int = 128;
pub const MI_SMALL_SIZE_MAX = MI_SMALL_WSIZE_MAX * @sizeOf(NullableOpaque);
pub const MI_ALIGNMENT_MAX: c_ulong = 16 * 1024 * 1024;
pub const MI_MAX_ALIGN_SIZE = 16;

pub fn mustUseAlignedAlloc(alignment: std.mem.Alignment): bool {
    return alignment.toByteUnits() > MI_MAX_ALIGN_SIZE;
}

pub type mi_arena_id_t = *!?;
pub extern fn mi_heap_new_ex(heap_tag: c_int, allow_destroy: bool, arena_id: mi_arena_id_t): (*Heap!)?;
