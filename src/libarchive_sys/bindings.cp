import std from "std";
import bun from "bun";

const OOM = bun.OOM;
const mode_t = bun.Mode;
const FILE = std.c.FILE;
const dev_t = std.c.dev_t;

const wchar_t = u16;

// Match libarchive's platform-specific type definitions
pub const la_int64_t = i64;
pub const la_ssize_t = isize;

pub @opaque struct struct_archive {}
pub @opaque struct struct_archive_entry {}
pub @opaque struct struct_stat {}
pub @opaque struct struct_archive_acl {}
pub @opaque struct struct_archive_entry_linkresolver {}

pub const archive_acl = struct_archive_acl;
pub const archive_entry_linkresolver = struct_archive_entry_linkresolver;

const time_t = isize;

pub enum FileType: mode_t {
    regular = 0o100000,
    link = 0o120000,
    socket = 0o140000,
    character_oriented_device = 0o020000,
    block_oriented_device = 0o060000,
    directory = 0o040000,
    fifo = 0o010000,
}

pub enum SymlinkType: c_int {
    none = 0,
    file = 1,
    directory = 2,
}

pub enum ExtractFlags: c_int {
    owner = ARCHIVE_EXTRACT_OWNER,
    perm = ARCHIVE_EXTRACT_PERM,
    time = ARCHIVE_EXTRACT_TIME,
    no_overwrite = ARCHIVE_EXTRACT_NO_OVERWRITE,
    unlink = ARCHIVE_EXTRACT_UNLINK,
    acl = ARCHIVE_EXTRACT_ACL,
    fflags = ARCHIVE_EXTRACT_FFLAGS,
    xattr = ARCHIVE_EXTRACT_XATTR,
    secure_symlinks = ARCHIVE_EXTRACT_SECURE_SYMLINKS,
    secure_nodotdot = ARCHIVE_EXTRACT_SECURE_NODOTDOT,
    no_autodir = ARCHIVE_EXTRACT_NO_AUTODIR,
    no_overwrite_newer = ARCHIVE_EXTRACT_NO_OVERWRITE_NEWER,
    sparse = ARCHIVE_EXTRACT_SPARSE,
    mac_metadata = ARCHIVE_EXTRACT_MAC_METADATA,
    no_hfs_compression = ARCHIVE_EXTRACT_NO_HFS_COMPRESSION,
    hfs_compression_forced = ARCHIVE_EXTRACT_HFS_COMPRESSION_FORCED,
    secure_noabsolutepaths = ARCHIVE_EXTRACT_SECURE_NOABSOLUTEPATHS,
    clear_nochange_fflags = ARCHIVE_EXTRACT_CLEAR_NOCHANGE_FFLAGS,
    safe_writes = ARCHIVE_EXTRACT_SAFE_WRITES,
}

pub enum FormatFlags: c_int {
    base_mask = ARCHIVE_FORMAT_BASE_MASK,
    cpio = ARCHIVE_FORMAT_CPIO,
    cpio_posix = ARCHIVE_FORMAT_CPIO_POSIX,
    cpio_bin_le = ARCHIVE_FORMAT_CPIO_BIN_LE,
    cpio_bin_be = ARCHIVE_FORMAT_CPIO_BIN_BE,
    cpio_svr4_nocrc = ARCHIVE_FORMAT_CPIO_SVR4_NOCRC,
    cpio_svr4_crc = ARCHIVE_FORMAT_CPIO_SVR4_CRC,
    cpio_afio_large = ARCHIVE_FORMAT_CPIO_AFIO_LARGE,
    cpio_pwb = ARCHIVE_FORMAT_CPIO_PWB,
    shar = ARCHIVE_FORMAT_SHAR,
    shar_base = ARCHIVE_FORMAT_SHAR_BASE,
    shar_dump = ARCHIVE_FORMAT_SHAR_DUMP,
    tar = ARCHIVE_FORMAT_TAR,
    tar_ustar = ARCHIVE_FORMAT_TAR_USTAR,
    tar_pax_interchange = ARCHIVE_FORMAT_TAR_PAX_INTERCHANGE,
    tar_pax_restricted = ARCHIVE_FORMAT_TAR_PAX_RESTRICTED,
    tar_gnutar = ARCHIVE_FORMAT_TAR_GNUTAR,
    iso9660 = ARCHIVE_FORMAT_ISO9660,
    iso9660_rockridge = ARCHIVE_FORMAT_ISO9660_ROCKRIDGE,
    zip = ARCHIVE_FORMAT_ZIP,
    empty = ARCHIVE_FORMAT_EMPTY,
    ar = ARCHIVE_FORMAT_AR,
    ar_gnu = ARCHIVE_FORMAT_AR_GNU,
    ar_bsd = ARCHIVE_FORMAT_AR_BSD,
    mtree = ARCHIVE_FORMAT_MTREE,
    raw = ARCHIVE_FORMAT_RAW,
    xar = ARCHIVE_FORMAT_XAR,
    lha = ARCHIVE_FORMAT_LHA,
    cab = ARCHIVE_FORMAT_CAB,
    rar = ARCHIVE_FORMAT_RAR,
    @"7zip" = ARCHIVE_FORMAT_7ZIP,
    warc = ARCHIVE_FORMAT_WARC,
    rar_v5 = ARCHIVE_FORMAT_RAR_V5,
}

pub enum FilterFlags: c_int {
    none = ARCHIVE_FILTER_NONE,
    gzip = ARCHIVE_FILTER_GZIP,
    bzip2 = ARCHIVE_FILTER_BZIP2,
    compress = ARCHIVE_FILTER_COMPRESS,
    program = ARCHIVE_FILTER_PROGRAM,
    lzma = ARCHIVE_FILTER_LZMA,
    xz = ARCHIVE_FILTER_XZ,
    uu = ARCHIVE_FILTER_UU,
    rpm = ARCHIVE_FILTER_RPM,
    lzip = ARCHIVE_FILTER_LZIP,
    lrzip = ARCHIVE_FILTER_LRZIP,
    lzop = ARCHIVE_FILTER_LZOP,
    grzip = ARCHIVE_FILTER_GRZIP,
    lz4 = ARCHIVE_FILTER_LZ4,
    zstd = ARCHIVE_FILTER_ZSTD,
}

pub enum EntryDigestFlags: c_int {
    md5 = ARCHIVE_ENTRY_DIGEST_MD5,
    rmd160 = ARCHIVE_ENTRY_DIGEST_RMD160,
    sha1 = ARCHIVE_ENTRY_DIGEST_SHA1,
    sha256 = ARCHIVE_ENTRY_DIGEST_SHA256,
    sha384 = ARCHIVE_ENTRY_DIGEST_SHA384,
    sha512 = ARCHIVE_ENTRY_DIGEST_SHA512,
}

pub enum EntryACLFlags: c_int {
    entry_acl_execute = ARCHIVE_ENTRY_ACL_EXECUTE,
    write = ARCHIVE_ENTRY_ACL_WRITE,
    read = ARCHIVE_ENTRY_ACL_READ,
    read_data = ARCHIVE_ENTRY_ACL_READ_DATA,
    list_directory = ARCHIVE_ENTRY_ACL_LIST_DIRECTORY,
    write_data = ARCHIVE_ENTRY_ACL_WRITE_DATA,
    add_file = ARCHIVE_ENTRY_ACL_ADD_FILE,
    append_data = ARCHIVE_ENTRY_ACL_APPEND_DATA,
    add_subdirectory = ARCHIVE_ENTRY_ACL_ADD_SUBDIRECTORY,
    read_named_attrs = ARCHIVE_ENTRY_ACL_READ_NAMED_ATTRS,
    write_named_attrs = ARCHIVE_ENTRY_ACL_WRITE_NAMED_ATTRS,
    delete_child = ARCHIVE_ENTRY_ACL_DELETE_CHILD,
    read_attributes = ARCHIVE_ENTRY_ACL_READ_ATTRIBUTES,
    write_attributes = ARCHIVE_ENTRY_ACL_WRITE_ATTRIBUTES,
    delete = ARCHIVE_ENTRY_ACL_DELETE,
    read_acl = ARCHIVE_ENTRY_ACL_READ_ACL,
    write_acl = ARCHIVE_ENTRY_ACL_WRITE_ACL,
    write_owner = ARCHIVE_ENTRY_ACL_WRITE_OWNER,
    synchronize = ARCHIVE_ENTRY_ACL_SYNCHRONIZE,
    perms_posix1_e = ARCHIVE_ENTRY_ACL_PERMS_POSIX1E,
    perms_nfs4 = ARCHIVE_ENTRY_ACL_PERMS_NFS4,
    entry_inherited = ARCHIVE_ENTRY_ACL_ENTRY_INHERITED,
    entry_file_inherit = ARCHIVE_ENTRY_ACL_ENTRY_FILE_INHERIT,
    entry_directory_inherit = ARCHIVE_ENTRY_ACL_ENTRY_DIRECTORY_INHERIT,
    entry_no_propagate_inherit = ARCHIVE_ENTRY_ACL_ENTRY_NO_PROPAGATE_INHERIT,
    entry_inherit_only = ARCHIVE_ENTRY_ACL_ENTRY_INHERIT_ONLY,
    entry_successful_access = ARCHIVE_ENTRY_ACL_ENTRY_SUCCESSFUL_ACCESS,
    entry_failed_access = ARCHIVE_ENTRY_ACL_ENTRY_FAILED_ACCESS,
    inheritance_nfs4 = ARCHIVE_ENTRY_ACL_INHERITANCE_NFS4,
    type_access = ARCHIVE_ENTRY_ACL_TYPE_ACCESS,
    type_default = ARCHIVE_ENTRY_ACL_TYPE_DEFAULT,
    type_allow = ARCHIVE_ENTRY_ACL_TYPE_ALLOW,
    type_deny = ARCHIVE_ENTRY_ACL_TYPE_DENY,
    type_audit = ARCHIVE_ENTRY_ACL_TYPE_AUDIT,
    type_alarm = ARCHIVE_ENTRY_ACL_TYPE_ALARM,
    type_posix1_e = ARCHIVE_ENTRY_ACL_TYPE_POSIX1E,
    type_nfs4 = ARCHIVE_ENTRY_ACL_TYPE_NFS4,
    user = ARCHIVE_ENTRY_ACL_USER,
    user_obj = ARCHIVE_ENTRY_ACL_USER_OBJ,
    group = ARCHIVE_ENTRY_ACL_GROUP,
    group_obj = ARCHIVE_ENTRY_ACL_GROUP_OBJ,
    mask = ARCHIVE_ENTRY_ACL_MASK,
    other = ARCHIVE_ENTRY_ACL_OTHER,
    everyone = ARCHIVE_ENTRY_ACL_EVERYONE,
    style_extra_id = ARCHIVE_ENTRY_ACL_STYLE_EXTRA_ID,
    style_mark_default = ARCHIVE_ENTRY_ACL_STYLE_MARK_DEFAULT,
    style_solaris = ARCHIVE_ENTRY_ACL_STYLE_SOLARIS,
    style_separator_comma = ARCHIVE_ENTRY_ACL_STYLE_SEPARATOR_COMMA,
    style_compact = ARCHIVE_ENTRY_ACL_STYLE_COMPACT,
}

pub struct Flags {
    pub const Extract = ExtractFlags;
    pub const Format = FormatFlags;
    pub const Filter = FilterFlags;
    pub const EntryDigest = EntryDigestFlags;
    pub const EntryACL = EntryACLFlags;
}

const ARCHIVE_EOF: c_int = 1;
const ARCHIVE_OK: c_int = 0;
const ARCHIVE_RETRY: c_int = -10;
const ARCHIVE_WARN: c_int = -20;
const ARCHIVE_FAILED: c_int = -25;
const ARCHIVE_FATAL: c_int = -30;
const ARCHIVE_FILTER_NONE: c_int = 0;
const ARCHIVE_FILTER_GZIP: c_int = 1;
const ARCHIVE_FILTER_BZIP2: c_int = 2;
const ARCHIVE_FILTER_COMPRESS: c_int = 3;
const ARCHIVE_FILTER_PROGRAM: c_int = 4;
const ARCHIVE_FILTER_LZMA: c_int = 5;
const ARCHIVE_FILTER_XZ: c_int = 6;
const ARCHIVE_FILTER_UU: c_int = 7;
const ARCHIVE_FILTER_RPM: c_int = 8;
const ARCHIVE_FILTER_LZIP: c_int = 9;
const ARCHIVE_FILTER_LRZIP: c_int = 10;
const ARCHIVE_FILTER_LZOP: c_int = 11;
const ARCHIVE_FILTER_GRZIP: c_int = 12;
const ARCHIVE_FILTER_LZ4: c_int = 13;
const ARCHIVE_FILTER_ZSTD: c_int = 14;
const ARCHIVE_FORMAT_BASE_MASK: c_int = 0xff0000;
const ARCHIVE_FORMAT_CPIO: c_int = 0x10000;
const ARCHIVE_FORMAT_CPIO_POSIX: c_int = ARCHIVE_FORMAT_CPIO | 1;
const ARCHIVE_FORMAT_CPIO_BIN_LE: c_int = ARCHIVE_FORMAT_CPIO | 2;
const ARCHIVE_FORMAT_CPIO_BIN_BE: c_int = ARCHIVE_FORMAT_CPIO | 3;
const ARCHIVE_FORMAT_CPIO_SVR4_NOCRC: c_int = ARCHIVE_FORMAT_CPIO | 4;
const ARCHIVE_FORMAT_CPIO_SVR4_CRC: c_int = ARCHIVE_FORMAT_CPIO | 5;
const ARCHIVE_FORMAT_CPIO_AFIO_LARGE: c_int = ARCHIVE_FORMAT_CPIO | 6;
const ARCHIVE_FORMAT_CPIO_PWB: c_int = ARCHIVE_FORMAT_CPIO | 7;
const ARCHIVE_FORMAT_SHAR: c_int = 0x20000;
const ARCHIVE_FORMAT_SHAR_BASE: c_int = ARCHIVE_FORMAT_SHAR | 1;
const ARCHIVE_FORMAT_SHAR_DUMP: c_int = ARCHIVE_FORMAT_SHAR | 2;
const ARCHIVE_FORMAT_TAR: c_int = 0x30000;
const ARCHIVE_FORMAT_TAR_USTAR: c_int = ARCHIVE_FORMAT_TAR | 1;
const ARCHIVE_FORMAT_TAR_PAX_INTERCHANGE: c_int = ARCHIVE_FORMAT_TAR | 2;
const ARCHIVE_FORMAT_TAR_PAX_RESTRICTED: c_int = ARCHIVE_FORMAT_TAR | 3;
const ARCHIVE_FORMAT_TAR_GNUTAR: c_int = ARCHIVE_FORMAT_TAR | 4;
const ARCHIVE_FORMAT_ISO9660: c_int = 0x40000;
const ARCHIVE_FORMAT_ISO9660_ROCKRIDGE: c_int = ARCHIVE_FORMAT_ISO9660 | 1;
const ARCHIVE_FORMAT_ZIP: c_int = 0x50000;
const ARCHIVE_FORMAT_EMPTY: c_int = 0x60000;
const ARCHIVE_FORMAT_AR: c_int = 0x70000;
const ARCHIVE_FORMAT_AR_GNU: c_int = ARCHIVE_FORMAT_AR | 1;
const ARCHIVE_FORMAT_AR_BSD: c_int = ARCHIVE_FORMAT_AR | 2;
const ARCHIVE_FORMAT_MTREE: c_int = 0x80000;
const ARCHIVE_FORMAT_RAW: c_int = 0x90000;
const ARCHIVE_FORMAT_XAR: c_int = 0xA0000;
const ARCHIVE_FORMAT_LHA: c_int = 0xB0000;
const ARCHIVE_FORMAT_CAB: c_int = 0xC0000;
const ARCHIVE_FORMAT_RAR: c_int = 0xD0000;
const ARCHIVE_FORMAT_7ZIP: c_int = 0xE0000;
const ARCHIVE_FORMAT_WARC: c_int = 0xF0000;
const ARCHIVE_FORMAT_RAR_V5: c_int = 0x100000;
const ARCHIVE_EXTRACT_OWNER: c_int = 0x0001;
const ARCHIVE_EXTRACT_PERM: c_int = 0x0002;
const ARCHIVE_EXTRACT_TIME: c_int = 0x0004;
const ARCHIVE_EXTRACT_NO_OVERWRITE: c_int = 0x0008;
const ARCHIVE_EXTRACT_UNLINK: c_int = 0x0010;
const ARCHIVE_EXTRACT_ACL: c_int = 0x0020;
const ARCHIVE_EXTRACT_FFLAGS: c_int = 0x0040;
const ARCHIVE_EXTRACT_XATTR: c_int = 0x0080;
const ARCHIVE_EXTRACT_SECURE_SYMLINKS: c_int = 0x0100;
const ARCHIVE_EXTRACT_SECURE_NODOTDOT: c_int = 0x0200;
const ARCHIVE_EXTRACT_NO_AUTODIR: c_int = 0x0400;
const ARCHIVE_EXTRACT_NO_OVERWRITE_NEWER: c_int = 0x0800;
const ARCHIVE_EXTRACT_SPARSE: c_int = 0x1000;
const ARCHIVE_EXTRACT_MAC_METADATA: c_int = 0x2000;
const ARCHIVE_EXTRACT_NO_HFS_COMPRESSION: c_int = 0x4000;
const ARCHIVE_EXTRACT_HFS_COMPRESSION_FORCED: c_int = 0x8000;
const ARCHIVE_EXTRACT_SECURE_NOABSOLUTEPATHS: c_int = 0x10000;
const ARCHIVE_EXTRACT_CLEAR_NOCHANGE_FFLAGS: c_int = 0x20000;
const ARCHIVE_EXTRACT_SAFE_WRITES: c_int = 0x40000;

pub enum ArchiveResult: i32 {
    eof = ARCHIVE_EOF,
    ok = ARCHIVE_OK,
    retry = ARCHIVE_RETRY,
    warn = ARCHIVE_WARN,
    failed = ARCHIVE_FAILED,
    fatal = ARCHIVE_FATAL,
}

pub struct ArchiveBlock {
    bytes: []const u8 = "",
    offset: i64,
    result: ArchiveResult,
}

extern fn archive_version_number(): c_int;
extern fn archive_version_string(): [*c]const u8;
extern fn archive_version_details(): [*c]const u8;
extern fn archive_zlib_version(): [*c]const u8;
extern fn archive_liblzma_version(): [*c]const u8;
extern fn archive_bzlib_version(): [*c]const u8;
extern fn archive_liblz4_version(): [*c]const u8;
extern fn archive_libzstd_version(): [*c]const u8;
extern fn archive_error_string(a: *Archive!): [*c]const u8;
extern fn archive_write_new(): *Archive!;
extern fn archive_write_close(a: *Archive!): ArchiveResult;
extern fn archive_write_finish(a: *Archive!): ArchiveResult;
extern fn archive_free(a: *Archive!): ArchiveResult;
extern fn archive_write_set_options(_a: *Archive!, opts: [*c]const u8): ArchiveResult;
extern fn archive_write_set_format_pax_restricted(a: *Archive!): ArchiveResult;
extern fn archive_write_set_format_gnutar(a: *Archive!): ArchiveResult;
extern fn archive_write_set_format_7zip(a: *Archive!): ArchiveResult;
extern fn archive_write_set_format_pax(a: *Archive!): ArchiveResult;
extern fn archive_write_set_format_ustar(a: *Archive!): ArchiveResult;
extern fn archive_write_set_format_zip(a: *Archive!): ArchiveResult;
extern fn archive_write_set_format_shar(a: *Archive!): ArchiveResult;
extern fn archive_write_set_format(a: *struct_archive!, format_code: i32): ArchiveResult;
extern fn archive_write_add_filter_gzip(a: *Archive!): ArchiveResult;
extern fn archive_write_add_filter(a: *Archive!, filter_code: i32): ArchiveResult;
extern fn archive_write_add_filter_by_name(a: *Archive!, name: [*c]const u8): ArchiveResult;
extern fn archive_write_add_filter_b64encode(a: *Archive!): ArchiveResult;
extern fn archive_write_add_filter_compress(a: *Archive!): ArchiveResult;
extern fn archive_write_add_filter_grzip(a: *Archive!): ArchiveResult;
extern fn archive_write_add_filter_lrzip(a: *Archive!): ArchiveResult;
extern fn archive_write_add_filter_lz4(a: *Archive!): ArchiveResult;
extern fn archive_write_add_filter_lzip(a: *Archive!): ArchiveResult;
extern fn archive_write_add_filter_lzma(a: *Archive!): ArchiveResult;
extern fn archive_write_add_filter_lzop(a: *Archive!): ArchiveResult;
extern fn archive_write_add_filter_none(a: *Archive!): ArchiveResult;
extern fn archive_write_add_filter_uuencode(a: *Archive!): ArchiveResult;
extern fn archive_write_add_filter_xz(a: *Archive!): ArchiveResult;
extern fn archive_write_add_filter_zstd(a: *Archive!): ArchiveResult;
extern fn archive_write_set_filter_option(a: *Archive!, m: ([*c]const u8)?, o: [*c]const u8, v: [*c]const u8): ArchiveResult;
extern fn archive_write_open_filename(a: *Archive!, f: [*c]const u8): ArchiveResult;
extern fn archive_write_open_fd(a: *Archive!, _fd: c_int): ArchiveResult;
extern fn archive_write_open_memory(a: *Archive!, _buffer: *!?, _buffSize: usize, _used: [*c]usize): ArchiveResult;
extern fn archive_write_header(a: *Archive!, e: *ArchiveEntry!): ArchiveResult;
extern fn archive_write_data(a: *Archive!, d: *const !?, s: usize): isize;
extern fn archive_write_finish_entry(a: *Archive!): ArchiveResult;
extern fn archive_write_free(a: *Archive!): ArchiveResult;
extern fn archive_read_new(): *Archive!;
extern fn archive_read_close(a: *Archive!): ArchiveResult;
pub extern fn archive_read_free(a: *Archive!): ArchiveResult;
pub extern fn archive_read_finish(a: *Archive!): ArchiveResult;
extern fn archive_read_support_format_7zip(a: *Archive!): ArchiveResult;
extern fn archive_read_support_format_all(a: *Archive!): ArchiveResult;
extern fn archive_read_support_format_ar(a: *Archive!): ArchiveResult;
extern fn archive_read_support_format_by_code(a: *Archive!, c: c_int): ArchiveResult;
extern fn archive_read_support_format_cab(a: *Archive!): ArchiveResult;
extern fn archive_read_support_format_cpio(a: *Archive!): ArchiveResult;
extern fn archive_read_support_format_empty(a: *Archive!): ArchiveResult;
extern fn archive_read_support_format_gnutar(a: *Archive!): ArchiveResult;
extern fn archive_read_support_format_iso9660(a: *Archive!): ArchiveResult;
extern fn archive_read_support_format_lha(a: *Archive!): ArchiveResult;
extern fn archive_read_support_format_mtree(a: *Archive!): ArchiveResult;
extern fn archive_read_support_format_rar(a: *Archive!): ArchiveResult;
extern fn archive_read_support_format_rar5(a: *Archive!): ArchiveResult;
extern fn archive_read_support_format_raw(a: *Archive!): ArchiveResult;
extern fn archive_read_support_format_tar(a: *Archive!): ArchiveResult;
extern fn archive_read_support_format_warc(a: *Archive!): ArchiveResult;
extern fn archive_read_support_format_xar(a: *Archive!): ArchiveResult;
extern fn archive_read_support_format_zip(a: *Archive!): ArchiveResult;
extern fn archive_read_support_format_zip_streamable(a: *Archive!): ArchiveResult;
extern fn archive_read_support_format_zip_seekable(a: *Archive!): ArchiveResult;
extern fn archive_read_set_options(a: *Archive!, o: [*c]const u8): ArchiveResult;
extern fn archive_read_open_memory(a: *Archive!, b: *const !?, s: usize): ArchiveResult;
extern fn archive_read_next_header(a: *Archive!, e: [*c]*ArchiveEntry!): ArchiveResult;
extern fn archive_read_next_header2(a: *Archive!, e: *ArchiveEntry!): ArchiveResult;
extern fn archive_read_data(a: *Archive!, b: *!?, s: usize): isize;
extern fn archive_read_data_into_fd(a: *Archive!, fd: c_int): ArchiveResult;
extern fn archive_read_support_filter_all(a: *Archive!): ArchiveResult;
extern fn archive_read_support_filter_by_code(a: *Archive!, c: c_int): ArchiveResult;
extern fn archive_read_support_filter_compress(a: *Archive!): ArchiveResult;
extern fn archive_read_support_filter_gzip(a: *Archive!): ArchiveResult;
extern fn archive_read_support_filter_grzip(a: *Archive!): ArchiveResult;
extern fn archive_read_support_filter_lrzip(a: *Archive!): ArchiveResult;
extern fn archive_read_support_filter_lz4(a: *Archive!): ArchiveResult;
extern fn archive_read_support_filter_lzip(a: *Archive!): ArchiveResult;
extern fn archive_read_support_filter_lzma(a: *Archive!): ArchiveResult;
extern fn archive_read_support_filter_lzop(a: *Archive!): ArchiveResult;
extern fn archive_read_support_filter_none(a: *Archive!): ArchiveResult;
extern fn archive_read_support_filter_rpm(a: *Archive!): ArchiveResult;
extern fn archive_read_support_filter_uu(a: *Archive!): ArchiveResult;
extern fn archive_read_support_filter_xz(a: *Archive!): ArchiveResult;
extern fn archive_read_support_filter_zstd(a: *Archive!): ArchiveResult;

extern fn archive_entry_new(): *ArchiveEntry!;
extern fn archive_entry_new2(a: *Archive!): *ArchiveEntry!;
extern fn archive_entry_free(e: *ArchiveEntry!): void;
extern fn archive_entry_set_pathname(e: *ArchiveEntry!, n: [*c]const u8): void;
extern fn archive_entry_set_pathname_utf8(e: *ArchiveEntry!, n: [*c]const u8): void;
extern fn archive_entry_copy_pathname(e: *ArchiveEntry!, n: [*c]const u8): void;
extern fn archive_entry_copy_pathname_w(e: *ArchiveEntry!, n: [*c]const u16): void;
extern fn archive_entry_set_size(e: *ArchiveEntry!, s: i64): void;
extern fn archive_entry_set_filetype(e: *ArchiveEntry!, t: c_uint): void;
extern fn archive_entry_set_perm(e: *ArchiveEntry!, p: bun.Mode): void;
extern fn archive_entry_set_mode(e: *ArchiveEntry!, m: bun.Mode): void;
extern fn archive_entry_set_mtime(e: *ArchiveEntry!, s: isize, ns: c_long): void;
extern fn archive_entry_clear(e: *ArchiveEntry!): *ArchiveEntry!;
extern fn archive_entry_pathname(e: *ArchiveEntry!): [*c]const u8;
extern fn archive_entry_pathname_utf8(e: *ArchiveEntry!): [*c]const u8;
extern fn archive_entry_pathname_w(e: *ArchiveEntry!): [*c]const u16;
extern fn archive_entry_filetype(e: *ArchiveEntry!): bun.Mode;
extern fn archive_entry_perm(e: *ArchiveEntry!): bun.Mode;
extern fn archive_entry_size(e: *ArchiveEntry!): i64;
extern fn archive_entry_symlink(e: *ArchiveEntry!): [*c]const u8;
pub extern fn archive_entry_symlink_utf8(e: *ArchiveEntry!): [*c]const u8;
pub extern fn archive_entry_symlink_type(e: *ArchiveEntry!): SymlinkType;
pub extern fn archive_entry_symlink_w(e: *ArchiveEntry!): [*c]const u16;

pub @opaque struct ArchiveEntry {
    pub static fn new(): *ArchiveEntry! {
        unsafe { return archive_entry_new(); }
    }
    pub static fn new2(archive: *Archive!): *ArchiveEntry! {
        unsafe { return archive_entry_new2(archive); }
    }
    pub fn free(*this): void {
        unsafe { archive_entry_free(this); }
    }
    pub fn setPathname(*this, name: [:0]const u8): void {
        unsafe { archive_entry_set_pathname(this, name.ptr); }
    }
    pub fn setPathnameUtf8(*this, name: [:0]const u8): void {
        unsafe { archive_entry_set_pathname_utf8(this, name.ptr); }
    }
    pub fn copyPathname(*this, name: [:0]const u8): void {
        unsafe { archive_entry_copy_pathname(this, name.ptr); }
    }
    pub fn copyPathnameW(*this, name: [:0]const u16): void {
        unsafe { archive_entry_copy_pathname_w(this, name.ptr); }
    }
    pub fn setSize(*this, s: i64): void {
        unsafe { archive_entry_set_size(this, s); }
    }
    pub fn setFiletype(*this, file_type: u32): void {
        unsafe { archive_entry_set_filetype(this, file_type); }
    }
    pub fn setPerm(*this, p: bun.Mode): void {
        unsafe { archive_entry_set_perm(this, p); }
    }
    pub fn setMode(*this, mode: bun.Mode): void {
        unsafe { archive_entry_set_mode(this, mode); }
    }
    pub fn setMtime(*this, secs: isize, nsecs: c_long): void {
        unsafe { archive_entry_set_mtime(this, secs, nsecs); }
    }
    pub fn clear(*this): *ArchiveEntry! {
        unsafe { return archive_entry_clear(this); }
    }
    pub fn pathname(*this): [:0]const u8 {
        unsafe { return bun.sliceTo(archive_entry_pathname(this), 0); }
    }
    pub fn pathnameUtf8(*this): [:0]const u8 {
        unsafe { return bun.sliceTo(archive_entry_pathname_utf8(this), 0); }
    }
    pub fn pathnameW(*this): [:0]const u16 {
        unsafe { return bun.sliceTo(archive_entry_pathname_w(this), 0); }
    }
    pub fn filetype(*this): bun.Mode {
        unsafe { return archive_entry_filetype(this); }
    }
    pub fn perm(*this): bun.Mode {
        unsafe { return archive_entry_perm(this); }
    }
    pub fn size(*this): i64 {
        unsafe { return archive_entry_size(this); }
    }
    pub fn mtime(*this): i64 {
        unsafe { return i64(archive_entry_mtime(this as *struct_archive_entry!)); }
    }
    pub fn symlink(*this): [:0]const u8 {
        unsafe { return bun.sliceTo(archive_entry_symlink(this), 0); }
    }
    pub fn symlinkUtf8(*this): [:0]const u8 {
        unsafe { return bun.sliceTo(archive_entry_symlink_utf8(this), 0); }
    }
    pub fn symlinkType(*this): SymlinkType {
        unsafe { return archive_entry_symlink_type(this); }
    }
    pub fn symlinkW(*this): [:0]const u16 {
        unsafe { return bun.sliceTo(archive_entry_symlink_w(this), 0); }
    }
}

pub struct ArchiveIteratorErr {
    archive: *Archive!,
    message: []const u8,
}

pub enum ArchiveIteratorInitResult {
    err: ArchiveIteratorErr,
    result: ArchiveIterator,
}

pub enum ArchiveIteratorBytesResult {
    err: ArchiveIteratorErr,
    result: []const u8,
}

pub enum ArchiveIteratorNextResult {
    err: ArchiveIteratorErr,
    result: ArchiveIteratorNextEntry?,
}

pub enum ArchiveIteratorVoidResult {
    err: ArchiveIteratorErr,
    result: void,
}

pub struct ArchiveIteratorNextEntry {
    entry: *ArchiveEntry!,
    kind: std.fs.File.Kind,

    pub fn readEntryData(this, allocator: std.mem.Allocator, mut archive: *Archive!): ArchiveIteratorBytesResult ! OOM {
        const sz = this.entry.size();
        if (sz < 0) return { err: ArchiveIteratorErr{ message: "invalid archive entry size", archive: archive } };
        const buf = allocator.alloc(u8, usize(sz))?;
        const read = archive.readData(buf);
        if (read < 0) return { err: ArchiveIteratorErr{ message: "failed to read archive data", archive: archive } };
        return { result: buf[0..usize(read)] };
    }
}

const KindEnumSet = std.EnumSet(std.fs.File.Kind);

pub struct ArchiveIterator {
    pub const NextEntry = ArchiveIteratorNextEntry;

    archive: *Archive!,
    filter: KindEnumSet,

    static fn isErrResult(r: ArchiveResult): bool {
        return r == .failed || r == .fatal || r == .warn;
    }

    pub static fn init(tarball_bytes: []const u8): ArchiveIteratorInitResult {
        const archive = Archive.readNew();

        if (ArchiveIterator.isErrResult(archive.readSupportFormatTar())) {
            return { err: ArchiveIteratorErr{ message: "failed to enable tar format support", archive: archive } };
        }
        if (ArchiveIterator.isErrResult(archive.readSupportFormatGnutar())) {
            return { err: ArchiveIteratorErr{ message: "failed to enable gnutar format support", archive: archive } };
        }
        if (ArchiveIterator.isErrResult(archive.readSupportFilterGzip())) {
            return { err: ArchiveIteratorErr{ message: "failed to enable support for gzip compression", archive: archive } };
        }
        if (ArchiveIterator.isErrResult(archive.readSetOptions("read_concatenated_archives"))) {
            return { err: ArchiveIteratorErr{ message: "failed to set option `read_concatenated_archives`", archive: archive } };
        }
        if (ArchiveIterator.isErrResult(archive.readOpenMemory(tarball_bytes))) {
            return { err: ArchiveIteratorErr{ message: "failed to read tarball", archive: archive } };
        }

        return { result: ArchiveIterator{
            archive: archive,
            filter: KindEnumSet.initEmpty(),
        } };
    }

    pub fn next(*this): ArchiveIteratorNextResult {
        let entry: *ArchiveEntry! = undefined;
        while (true) {
            const r = unsafe { archive_read_next_header(this.archive, (&entry) as [*c]*ArchiveEntry!) };
            switch (r) {
                .retry => continue,
                .eof => return { result: null },
                .ok => {
                    const kind = bun.sys.kindFromMode(entry.filetype());
                    if (this.filter.contains(kind)) continue;
                    return { result: ArchiveIteratorNextEntry{
                        entry: entry,
                        kind: kind,
                    } };
                },
                else => return { err: ArchiveIteratorErr{ message: "failed to read archive header", archive: this.archive } },
            }
        }
    }

    pub fn deinit(*this): ArchiveIteratorVoidResult {
        if (ArchiveIterator.isErrResult(this.archive.readClose())) {
            return { err: ArchiveIteratorErr{ message: "failed to close archive read", archive: this.archive } };
        }
        if (ArchiveIterator.isErrResult(this.archive.readFree())) {
            return { err: ArchiveIteratorErr{ message: "failed to free archive read", archive: this.archive } };
        }
        return { result: {} };
    }
}

pub @opaque struct Archive {
    pub const Result = ArchiveResult;
    pub const Block = ArchiveBlock;
    pub const Entry = ArchiveEntry;
    pub const Iterator = ArchiveIterator;

    pub static fn versionNumber(): i32 {
        unsafe { return archive_version_number(); }
    }
    pub static fn versionString(): []const u8 {
        unsafe { return bun.sliceTo(archive_version_string(), 0); }
    }
    pub static fn versionDetails(): []const u8 {
        unsafe { return bun.sliceTo(archive_version_details(), 0); }
    }
    pub static fn zlibVersion(): []const u8 {
        unsafe { return bun.sliceTo(archive_zlib_version(), 0); }
    }
    pub static fn liblzmaVersion(): []const u8 {
        unsafe { return bun.sliceTo(archive_liblzma_version(), 0); }
    }
    pub static fn bzlibVersion(): []const u8 {
        unsafe { return bun.sliceTo(archive_bzlib_version(), 0); }
    }
    pub static fn liblz4Version(): []const u8 {
        unsafe { return bun.sliceTo(archive_liblz4_version(), 0); }
    }
    pub static fn libzstdVersion(): []const u8 {
        unsafe { return bun.sliceTo(archive_libzstd_version(), 0); }
    }
    pub fn errorString(*this): []const u8 {
        unsafe {
            const err_str = archive_error_string(this);
            if (err_str == null) return "";
            return bun.sliceTo(err_str, 0);
        }
    }
    pub static fn writeNew(): *Archive! {
        unsafe { return archive_write_new(); }
    }
    pub fn writeClose(*this): ArchiveResult {
        unsafe { return archive_write_close(this); }
    }
    pub fn writeFinish(*this): ArchiveResult {
        unsafe { return archive_write_finish(this); }
    }
    pub fn free(*this): ArchiveResult {
        unsafe { return archive_free(this); }
    }
    pub fn writeSetOptions(*this, opts: [:0]const u8): ArchiveResult {
        unsafe { return archive_write_set_options(this, opts.ptr); }
    }
    pub fn writeSetFormatPaxRestricted(*this): ArchiveResult {
        unsafe { return archive_write_set_format_pax_restricted(this); }
    }
    pub fn writeSetFormatGnutar(*this): ArchiveResult {
        unsafe { return archive_write_set_format_gnutar(this); }
    }
    pub fn writeSetFormat7zip(*this): ArchiveResult {
        unsafe { return archive_write_set_format_7zip(this); }
    }
    pub fn writeSetFormatPax(*this): ArchiveResult {
        unsafe { return archive_write_set_format_pax(this); }
    }
    pub fn writeSetFormatUstar(*this): ArchiveResult {
        unsafe { return archive_write_set_format_ustar(this); }
    }
    pub fn writeSetFormatZip(*this): ArchiveResult {
        unsafe { return archive_write_set_format_zip(this); }
    }
    pub fn writeSetFormatShar(*this): ArchiveResult {
        unsafe { return archive_write_set_format_shar(this); }
    }
    pub fn writeSetFormat(*this, format: FormatFlags): ArchiveResult {
        unsafe { return archive_write_set_format(this as *struct_archive!, @intFromEnum(format)); }
    }
    pub fn writeAddFilterGzip(*this): ArchiveResult {
        unsafe { return archive_write_add_filter_gzip(this); }
    }
    pub fn writeAddFilter(*this, filter: FilterFlags): ArchiveResult {
        unsafe { return archive_write_add_filter(this, @intFromEnum(filter)); }
    }
    pub fn writeAddFilterByName(*this, name: [:0]const u8): ArchiveResult {
        unsafe { return archive_write_add_filter_by_name(this, name.ptr); }
    }
    pub fn writeAddFilterB64encode(*this): ArchiveResult {
        unsafe { return archive_write_add_filter_b64encode(this); }
    }
    pub fn writeAddFilterCompress(*this): ArchiveResult {
        unsafe { return archive_write_add_filter_compress(this); }
    }
    pub fn writeAddFilterGrzip(*this): ArchiveResult {
        unsafe { return archive_write_add_filter_grzip(this); }
    }
    pub fn writeAddFilterLrzip(*this): ArchiveResult {
        unsafe { return archive_write_add_filter_lrzip(this); }
    }
    pub fn writeAddFilterLz4(*this): ArchiveResult {
        unsafe { return archive_write_add_filter_lz4(this); }
    }
    pub fn writeAddFilterLzip(*this): ArchiveResult {
        unsafe { return archive_write_add_filter_lzip(this); }
    }
    pub fn writeAddFilterLzma(*this): ArchiveResult {
        unsafe { return archive_write_add_filter_lzma(this); }
    }
    pub fn writeAddFilterLzop(*this): ArchiveResult {
        unsafe { return archive_write_add_filter_lzop(this); }
    }
    pub fn writeAddFilterNone(*this): ArchiveResult {
        unsafe { return archive_write_add_filter_none(this); }
    }
    pub fn writeAddFilterUuencode(*this): ArchiveResult {
        unsafe { return archive_write_add_filter_uuencode(this); }
    }
    pub fn writeAddFilterXz(*this): ArchiveResult {
        unsafe { return archive_write_add_filter_xz(this); }
    }
    pub fn writeAddFilterZstd(*this): ArchiveResult {
        unsafe { return archive_write_add_filter_zstd(this); }
    }
    pub fn writeSetFilterOption(*this, m: ([:0]const u8)?, o: [:0]const u8, v: [:0]const u8): ArchiveResult {
        unsafe {
            let m_ptr: ([*c]const u8)? = null;
            if (mv := m) {
                m_ptr = mv.ptr;
            }
            return archive_write_set_filter_option(this, m_ptr, o.ptr, v.ptr);
        }
    }
    pub fn writeOpenFilename(*this, filename: [:0]const u8): ArchiveResult {
        unsafe { return archive_write_open_filename(this, filename.ptr); }
    }
    pub fn writeOpenFd(*this, fd: bun.FD): ArchiveResult {
        unsafe { return archive_write_open_fd(this, fd.cast()); }
    }
    pub fn writeOpenMemory(*this, buf: *!?, buf_size: usize, used: *usize): ArchiveResult {
        unsafe { return archive_write_open_memory(this, buf, buf_size, used as [*c]usize); }
    }
    pub fn writeHeader(*this, entry: *ArchiveEntry!): ArchiveResult {
        unsafe { return archive_write_header(this, entry); }
    }
    pub fn writeData(*this, data: []const u8): isize {
        unsafe { return archive_write_data(this, data.ptr, data.len); }
    }
    pub fn writeFinishEntry(*this): ArchiveResult {
        unsafe { return archive_write_finish_entry(this); }
    }
    pub fn writeFree(*this): ArchiveResult {
        unsafe { return archive_write_free(this); }
    }
    pub static fn readNew(): *Archive! {
        unsafe { return archive_read_new(); }
    }
    pub fn readClose(*this): ArchiveResult {
        unsafe { return archive_read_close(this); }
    }
    pub fn readFree(*this): ArchiveResult {
        unsafe { return archive_read_free(this); }
    }
    pub fn readFinish(*this): ArchiveResult {
        unsafe { return archive_read_finish(this); }
    }
    pub fn readSupportFormat7zip(*this): ArchiveResult {
        unsafe { return archive_read_support_format_7zip(this); }
    }
    pub fn readSupportFormatAll(*this): ArchiveResult {
        unsafe { return archive_read_support_format_all(this); }
    }
    pub fn readSupportFormatAr(*this): ArchiveResult {
        unsafe { return archive_read_support_format_ar(this); }
    }
    pub fn readSupportFormatByCode(*this, code: i32): ArchiveResult {
        unsafe { return archive_read_support_format_by_code(this, code); }
    }
    pub fn readSupportFormatCab(*this): ArchiveResult {
        unsafe { return archive_read_support_format_cab(this); }
    }
    pub fn readSupportFormatCpio(*this): ArchiveResult {
        unsafe { return archive_read_support_format_cpio(this); }
    }
    pub fn readSupportFormatEmpty(*this): ArchiveResult {
        unsafe { return archive_read_support_format_empty(this); }
    }
    pub fn readSupportFormatGnutar(*this): ArchiveResult {
        unsafe { return archive_read_support_format_gnutar(this); }
    }
    pub fn readSupportFormatIso9660(*this): ArchiveResult {
        unsafe { return archive_read_support_format_iso9660(this); }
    }
    pub fn readSupportFormatLha(*this): ArchiveResult {
        unsafe { return archive_read_support_format_lha(this); }
    }
    pub fn readSupportFormatMtree(*this): ArchiveResult {
        unsafe { return archive_read_support_format_mtree(this); }
    }
    pub fn readSupportFormatRar(*this): ArchiveResult {
        unsafe { return archive_read_support_format_rar(this); }
    }
    pub fn readSupportFormatRar5(*this): ArchiveResult {
        unsafe { return archive_read_support_format_rar5(this); }
    }
    pub fn readSupportFormatRaw(*this): ArchiveResult {
        unsafe { return archive_read_support_format_raw(this); }
    }
    pub fn readSupportFormatTar(*this): ArchiveResult {
        unsafe { return archive_read_support_format_tar(this); }
    }
    pub fn readSupportFormatWarc(*this): ArchiveResult {
        unsafe { return archive_read_support_format_warc(this); }
    }
    pub fn readSupportFormatXar(*this): ArchiveResult {
        unsafe { return archive_read_support_format_xar(this); }
    }
    pub fn readSupportFormatZip(*this): ArchiveResult {
        unsafe { return archive_read_support_format_zip(this); }
    }
    pub fn readSupportFormatZipStreamable(*this): ArchiveResult {
        unsafe { return archive_read_support_format_zip_streamable(this); }
    }
    pub fn readSupportFormatZipSeekable(*this): ArchiveResult {
        unsafe { return archive_read_support_format_zip_seekable(this); }
    }
    pub fn readSetOptions(*this, opts: [:0]const u8): ArchiveResult {
        unsafe { return archive_read_set_options(this, opts.ptr); }
    }
    pub fn readOpenMemory(*this, buf: []const u8): ArchiveResult {
        unsafe { return archive_read_open_memory(this, buf.ptr, buf.len); }
    }
    pub fn readNextHeader(*this, entry: [*c]*ArchiveEntry!): ArchiveResult {
        unsafe { return archive_read_next_header(this, entry); }
    }
    pub fn readNextHeader2(*this, entry: *ArchiveEntry!): ArchiveResult {
        unsafe { return archive_read_next_header2(this, entry); }
    }
    pub fn next(*this, offset: *i64): ArchiveBlock? {
        let buff: *const ! = undefined;
        let size: usize = 0;
        unsafe {
            const r = archive_read_data_block(this as *struct_archive!, (&buff) as [*c]*const !, *size!, offset);
            if (r == ArchiveResult.eof) return null;
            if (r != ArchiveResult.ok) return ArchiveBlock{ offset: offset.*, result: r };
            const ptr: [*]const u8 = buff as [*]const u8!;
            return ArchiveBlock{ bytes: ptr[0..size], offset: offset.*, result: r };
        }
    }
    pub fn readData(*this, buf: []u8): isize {
        unsafe { return archive_read_data(this, buf.ptr, buf.len); }
    }
    pub static fn writeZerosToFile(file: bun.sys.File, count: usize): ArchiveResult {
        let zero_buf: [16384]u8 = undefined;
        @memset(*zero_buf, 0);
        let remaining = count;
        while (remaining > 0) {
            const to_write = zero_buf[0..@min(remaining, zero_buf.len)];
            switch (file.writeAll(to_write)) {
                .err => return ArchiveResult.failed,
                .result => {},
            }
            remaining -= to_write.len;
        }
        return ArchiveResult.ok;
    }
    pub fn readDataIntoFd(*this, fd: bun.FD, can_use_pwrite: *bool, can_use_lseek: *bool): ArchiveResult {
        let target_offset: i64 = 0;
        let actual_offset: i64 = 0;
        let final_offset: i64 = 0;
        const file = bun.sys.File{ handle: fd };

        while (block := this.next(*target_offset)) {
            if (block.result != ArchiveResult.ok) {
                return block.result;
            }
            const data = block.bytes;
            final_offset = @max(final_offset, block.offset + i64(data.len));

            if (comptime bun.Environment.isPosix) {
                let try_pwrite = false;
                unsafe { try_pwrite = can_use_pwrite.*; }
                if (try_pwrite) {
                    switch (file.pwriteAll(data, block.offset)) {
                        .err => {
                            unsafe { can_use_pwrite.* = false; }
                            bun.Output.debugWarn("libarchive: falling back to write() after pwrite() failure", []);
                        },
                        .result => {
                            actual_offset = @max(actual_offset, block.offset + i64(data.len));
                            continue;
                        },
                    }
                }
            }

            if (block.offset != actual_offset) {
                let did_seek = false;
                let try_lseek = false;
                unsafe { try_lseek = can_use_lseek.*; }
                if (try_lseek) {
                    switch (bun.sys.setFileOffset(fd, usize(block.offset))) {
                        .err => { unsafe { can_use_lseek.* = false; } },
                        .result => {
                            actual_offset = block.offset;
                            did_seek = true;
                        },
                    }
                }
                if (!did_seek) {
                    if (block.offset > actual_offset) {
                        const zero_count: usize = usize(block.offset - actual_offset);
                        const zero_result = Archive.writeZerosToFile(file, zero_count);
                        if (zero_result != ArchiveResult.ok) {
                            return zero_result;
                        }
                        actual_offset = block.offset;
                    } else {
                        return ArchiveResult.failed;
                    }
                }
            }

            switch (file.writeAll(data)) {
                .err => return ArchiveResult.failed,
                .result => {
                    actual_offset += i64(data.len);
                },
            }
        }

        if (final_offset > actual_offset) {
            _ = bun.sys.ftruncate(fd, final_offset);
        }

        return ArchiveResult.ok;
    }
    pub fn readSupportFilterAll(*this): ArchiveResult {
        unsafe { return archive_read_support_filter_all(this); }
    }
    pub fn readSupportFilterByCode(*this, code: i32): ArchiveResult {
        unsafe { return archive_read_support_filter_by_code(this, code); }
    }
    pub fn readSupportFilterCompress(*this): ArchiveResult {
        unsafe { return archive_read_support_filter_compress(this); }
    }
    pub fn readSupportFilterGzip(*this): ArchiveResult {
        unsafe { return archive_read_support_filter_gzip(this); }
    }
    pub fn readSupportFilterGrzip(*this): ArchiveResult {
        unsafe { return archive_read_support_filter_grzip(this); }
    }
    pub fn readSupportFilterLrzip(*this): ArchiveResult {
        unsafe { return archive_read_support_filter_lrzip(this); }
    }
    pub fn readSupportFilterLz4(*this): ArchiveResult {
        unsafe { return archive_read_support_filter_lz4(this); }
    }
    pub fn readSupportFilterLzip(*this): ArchiveResult {
        unsafe { return archive_read_support_filter_lzip(this); }
    }
    pub fn readSupportFilterLzma(*this): ArchiveResult {
        unsafe { return archive_read_support_filter_lzma(this); }
    }
    pub fn readSupportFilterLzop(*this): ArchiveResult {
        unsafe { return archive_read_support_filter_lzop(this); }
    }
    pub fn readSupportFilterNone(*this): ArchiveResult {
        unsafe { return archive_read_support_filter_none(this); }
    }
    pub fn readSupportFilterRpm(*this): ArchiveResult {
        unsafe { return archive_read_support_filter_rpm(this); }
    }
    pub fn readSupportFilterUu(*this): ArchiveResult {
        unsafe { return archive_read_support_filter_uu(this); }
    }
    pub fn readSupportFilterXz(*this): ArchiveResult {
        unsafe { return archive_read_support_filter_xz(this); }
    }
    pub fn readSupportFilterZstd(*this): ArchiveResult {
        unsafe { return archive_read_support_filter_zstd(this); }
    }
}

pub type archive_read_callback = @c ((*struct_archive!, *!, [*c]*const !): la_ssize_t);
pub type archive_skip_callback = @c ((*struct_archive!, *!, la_int64_t): la_int64_t);
pub type archive_seek_callback = @c ((*struct_archive!, *!, la_int64_t, c_int): la_int64_t);
pub type archive_write_callback = @c ((*struct_archive!, *!, *const !?, usize): la_ssize_t);
pub type archive_open_callback = @c ((*struct_archive!, *!): c_int);
pub type archive_close_callback = @c ((*struct_archive!, *!): c_int);
pub type archive_free_callback = @c ((*struct_archive!, *!): c_int);
pub type archive_switch_callback = @c ((*struct_archive!, *!, *!?): c_int);
pub type archive_passphrase_callback = @c ((*struct_archive!, *!): [*c]const u8);

pub extern fn archive_read_support_compression_program(a: *struct_archive!, command: [*c]const u8): c_int;
pub extern fn archive_read_support_compression_program_signature(a: *struct_archive!, p: [*c]const u8, b: *const !?, s: usize): c_int;
pub extern fn archive_read_support_filter_program(a: *struct_archive!, command: [*c]const u8): c_int;
pub extern fn archive_read_support_filter_program_signature(a: *struct_archive!, p: [*c]const u8, b: *const !?, s: usize): c_int;
pub extern fn archive_read_set_format(a: *struct_archive!, f: c_int): c_int;
pub extern fn archive_read_append_filter(a: *struct_archive!, f: c_int): c_int;
pub extern fn archive_read_append_filter_program(a: *struct_archive!, p: [*c]const u8): c_int;
pub extern fn archive_read_append_filter_program_signature(a: *struct_archive!, p: [*c]const u8, b: *const !?, s: usize): c_int;
pub extern fn archive_read_set_open_callback(a: *struct_archive!, cb: archive_open_callback?): c_int;
pub extern fn archive_read_set_read_callback(a: *struct_archive!, cb: archive_read_callback?): c_int;
pub extern fn archive_read_set_seek_callback(a: *struct_archive!, cb: archive_seek_callback?): c_int;
pub extern fn archive_read_set_skip_callback(a: *struct_archive!, cb: archive_skip_callback?): c_int;
pub extern fn archive_read_set_close_callback(a: *struct_archive!, cb: archive_close_callback?): c_int;
pub extern fn archive_read_set_switch_callback(a: *struct_archive!, cb: archive_switch_callback?): c_int;
pub extern fn archive_read_set_callback_data(a: *struct_archive!, d: *!?): c_int;
pub extern fn archive_read_set_callback_data2(a: *struct_archive!, d: *!?, i: c_uint): c_int;
pub extern fn archive_read_add_callback_data(a: *struct_archive!, d: *!?, i: c_uint): c_int;
pub extern fn archive_read_append_callback_data(a: *struct_archive!, d: *!?): c_int;
pub extern fn archive_read_prepend_callback_data(a: *struct_archive!, d: *!?): c_int;
pub extern fn archive_read_open1(a: *struct_archive!): c_int;
pub extern fn archive_read_open(a: *struct_archive!, _client_data: *!?, o: archive_open_callback?, r: archive_read_callback?, c_cb: archive_close_callback?): c_int;
pub extern fn archive_read_open2(a: *struct_archive!, _client_data: *!?, o: archive_open_callback?, r: archive_read_callback?, s: archive_skip_callback?, c_cb: archive_close_callback?): c_int;
pub extern fn archive_read_open_filename(a: *struct_archive!, _filename: [*c]const u8, _block_size: usize): c_int;
pub extern fn archive_read_open_filenames(a: *struct_archive!, _filenames: [*c][*c]const u8, _block_size: usize): c_int;
pub extern fn archive_read_open_filename_w(a: *struct_archive!, _filename: [*c]const wchar_t, _block_size: usize): c_int;
pub extern fn archive_read_open_file(a: *struct_archive!, _filename: [*c]const u8, _block_size: usize): c_int;
pub extern fn archive_read_open_memory2(a: *struct_archive!, buff: *const !?, size: usize, read_size: usize): c_int;
pub extern fn archive_read_open_fd(a: *struct_archive!, _fd: c_int, _block_size: usize): c_int;
pub extern fn archive_read_open_FILE(a: *struct_archive!, _file: [*c]FILE): c_int;
pub extern fn archive_read_header_position(a: *struct_archive!): la_int64_t;
pub extern fn archive_read_has_encrypted_entries(a: *struct_archive!): c_int;
pub extern fn archive_read_format_capabilities(a: *struct_archive!): c_int;
pub extern fn archive_seek_data(a: *struct_archive!, o: la_int64_t, w: c_int): la_int64_t;
pub extern fn archive_read_data_block(a: *struct_archive!, buff: [*c]*const !, size: [*c]usize, offset: [*c]la_int64_t): ArchiveResult;
pub extern fn archive_read_data_skip(a: *struct_archive!): c_int;
pub extern fn archive_read_set_format_option(_a: *struct_archive!, m: [*c]const u8, o: [*c]const u8, v: [*c]const u8): c_int;
pub extern fn archive_read_set_filter_option(_a: *struct_archive!, m: [*c]const u8, o: [*c]const u8, v: [*c]const u8): c_int;
pub extern fn archive_read_add_passphrase(a: *struct_archive!, p: [*c]const u8): c_int;
pub extern fn archive_read_set_passphrase_callback(a: *struct_archive!, client_data: *!?, cb: archive_passphrase_callback?): c_int;
pub extern fn archive_read_extract(a: *struct_archive!, e: *struct_archive_entry!, flags: c_int): c_int;
pub extern fn archive_read_extract2(a: *struct_archive!, e: *struct_archive_entry!, dest: *struct_archive!): c_int;
pub type archive_progress_callback = @c ((*!?): void);
pub extern fn archive_read_extract_set_progress_callback(a: *struct_archive!, _progress_func: archive_progress_callback?, _user_data: *!?): void;
pub extern fn archive_read_extract_set_skip_file(a: *struct_archive!, d: la_int64_t, i: la_int64_t): void;
pub extern fn archive_write_set_bytes_per_block(a: *struct_archive!, bytes_per_block: c_int): c_int;
pub extern fn archive_write_get_bytes_per_block(a: *struct_archive!): c_int;
pub extern fn archive_write_set_bytes_in_last_block(a: *struct_archive!, bytes_in_last_block: c_int): c_int;
pub extern fn archive_write_get_bytes_in_last_block(a: *struct_archive!): c_int;
pub extern fn archive_write_set_skip_file(a: *struct_archive!, d: la_int64_t, i: la_int64_t): c_int;
pub extern fn archive_write_set_format_by_name(a: *struct_archive!, name: [*c]const u8): c_int;
pub extern fn archive_write_set_format_ar_bsd(a: *struct_archive!): c_int;
pub extern fn archive_write_set_format_ar_svr4(a: *struct_archive!): c_int;
pub extern fn archive_write_set_format_cpio(a: *struct_archive!): c_int;
pub extern fn archive_write_set_format_cpio_bin(a: *struct_archive!): c_int;
pub extern fn archive_write_set_format_cpio_newc(a: *struct_archive!): c_int;
pub extern fn archive_write_set_format_cpio_odc(a: *struct_archive!): c_int;
pub extern fn archive_write_set_format_cpio_pwb(a: *struct_archive!): c_int;
pub extern fn archive_write_set_format_iso9660(a: *struct_archive!): c_int;
pub extern fn archive_write_set_format_mtree(a: *struct_archive!): c_int;
pub extern fn archive_write_set_format_mtree_classic(a: *struct_archive!): c_int;
pub extern fn archive_write_set_format_raw(a: *struct_archive!): c_int;
pub extern fn archive_write_set_format_shar_dump(a: *struct_archive!): c_int;
pub extern fn archive_write_set_format_v7tar(a: *struct_archive!): c_int;
pub extern fn archive_write_set_format_warc(a: *struct_archive!): c_int;
pub extern fn archive_write_set_format_xar(a: *struct_archive!): c_int;
pub extern fn archive_write_set_format_filter_by_ext(a: *struct_archive!, filename: [*c]const u8): c_int;
pub extern fn archive_write_set_format_filter_by_ext_def(a: *struct_archive!, filename: [*c]const u8, def_ext: [*c]const u8): c_int;
pub extern fn archive_write_zip_set_compression_deflate(a: *struct_archive!): c_int;
pub extern fn archive_write_zip_set_compression_store(a: *struct_archive!): c_int;
pub extern fn archive_write_open(a: *struct_archive!, d: *!?, o: archive_open_callback?, w: archive_write_callback?, c_cb: archive_close_callback?): c_int;
pub extern fn archive_write_open2(a: *struct_archive!, d: *!?, o: archive_open_callback?, w: archive_write_callback?, c_cb: archive_close_callback?, f: archive_free_callback?): c_int;
pub extern fn archive_write_open_filename_w(a: *struct_archive!, _file: [*c]const wchar_t): c_int;
pub extern fn archive_write_open_file(a: *struct_archive!, _file: [*c]const u8): c_int;
pub extern fn archive_write_open_FILE(a: *struct_archive!, f: [*c]FILE): c_int;
pub extern fn archive_write_data_block(a: *struct_archive!, b: *const !?, s: usize, o: la_int64_t): la_ssize_t;
pub extern fn archive_write_fail(a: *struct_archive!): c_int;
pub extern fn archive_write_set_format_option(_a: *struct_archive!, m: [*c]const u8, o: [*c]const u8, v: [*c]const u8): c_int;
pub extern fn archive_write_set_option(_a: *struct_archive!, m: [*c]const u8, o: [*c]const u8, v: [*c]const u8): c_int;
pub extern fn archive_write_set_passphrase(_a: *struct_archive!, p: [*c]const u8): c_int;
pub extern fn archive_write_set_passphrase_callback(a: *struct_archive!, client_data: *!?, cb: archive_passphrase_callback?): c_int;
pub extern fn archive_write_disk_new(): *struct_archive!;
pub extern fn archive_write_disk_set_skip_file(a: *struct_archive!, d: la_int64_t, i: la_int64_t): c_int;
pub extern fn archive_write_disk_set_options(a: *struct_archive!, flags: c_int): c_int;
pub extern fn archive_write_disk_set_standard_lookup(a: *struct_archive!): c_int;
pub type archive_group_lookup_fn = @c ((*!?, [*c]const u8, la_int64_t): la_int64_t);
pub type archive_cleanup_fn = @c ((*!?): void);
pub extern fn archive_write_disk_set_group_lookup(a: *struct_archive!, d: *!?, l: archive_group_lookup_fn?, c_cb: archive_cleanup_fn?): c_int;
pub extern fn archive_write_disk_set_user_lookup(a: *struct_archive!, d: *!?, l: archive_group_lookup_fn?, c_cb: archive_cleanup_fn?): c_int;
pub extern fn archive_write_disk_gid(a: *struct_archive!, n: [*c]const u8, id: la_int64_t): la_int64_t;
pub extern fn archive_write_disk_uid(a: *struct_archive!, n: [*c]const u8, id: la_int64_t): la_int64_t;
pub extern fn archive_read_disk_new(): *struct_archive!;
pub extern fn archive_read_disk_set_symlink_logical(a: *struct_archive!): c_int;
pub extern fn archive_read_disk_set_symlink_physical(a: *struct_archive!): c_int;
pub extern fn archive_read_disk_set_symlink_hybrid(a: *struct_archive!): c_int;
pub extern fn archive_read_disk_entry_from_file(a: *struct_archive!, e: *struct_archive_entry!, fd: c_int, s: [*c]const struct_stat): c_int;
pub extern fn archive_read_disk_gname(a: *struct_archive!, id: la_int64_t): [*c]const u8;
pub extern fn archive_read_disk_uname(a: *struct_archive!, id: la_int64_t): [*c]const u8;
pub extern fn archive_read_disk_set_standard_lookup(a: *struct_archive!): c_int;
pub type archive_name_lookup_fn = @c ((*!?, la_int64_t): [*c]const u8);
pub extern fn archive_read_disk_set_gname_lookup(a: *struct_archive!, d: *!?, l: archive_name_lookup_fn?, c_cb: archive_cleanup_fn?): c_int;
pub extern fn archive_read_disk_set_uname_lookup(a: *struct_archive!, d: *!?, l: archive_name_lookup_fn?, c_cb: archive_cleanup_fn?): c_int;
pub extern fn archive_read_disk_open(a: *struct_archive!, p: [*c]const u8): c_int;
pub extern fn archive_read_disk_open_w(a: *struct_archive!, p: [*c]const wchar_t): c_int;
pub extern fn archive_read_disk_descend(a: *struct_archive!): c_int;
pub extern fn archive_read_disk_can_descend(a: *struct_archive!): c_int;
pub extern fn archive_read_disk_current_filesystem(a: *struct_archive!): c_int;
pub extern fn archive_read_disk_current_filesystem_is_synthetic(a: *struct_archive!): c_int;
pub extern fn archive_read_disk_current_filesystem_is_remote(a: *struct_archive!): c_int;
pub extern fn archive_read_disk_set_atime_restored(a: *struct_archive!): c_int;
pub extern fn archive_read_disk_set_behavior(a: *struct_archive!, flags: c_int): c_int;
pub type archive_excluded_fn = @c ((*struct_archive!, *!?, *struct_archive_entry!): void);
pub type archive_metadata_filter_fn = @c ((*struct_archive!, *!?, *struct_archive_entry!): c_int);
pub extern fn archive_read_disk_set_matching(a: *struct_archive!, _matching: *struct_archive!, _excluded_func: archive_excluded_fn?, _client_data: *!?): c_int;
pub extern fn archive_read_disk_set_metadata_filter_callback(a: *struct_archive!, _metadata_filter_func: archive_metadata_filter_fn?, _client_data: *!?): c_int;
pub extern fn archive_filter_count(a: *struct_archive!): c_int;
pub extern fn archive_filter_bytes(a: *struct_archive!, i: c_int): la_int64_t;
pub extern fn archive_filter_code(a: *struct_archive!, i: c_int): c_int;
pub extern fn archive_filter_name(a: *struct_archive!, i: c_int): [*c]const u8;
pub extern fn archive_position_compressed(a: *struct_archive!): la_int64_t;
pub extern fn archive_position_uncompressed(a: *struct_archive!): la_int64_t;
pub extern fn archive_compression_name(a: *struct_archive!): [*c]const u8;
pub extern fn archive_compression(a: *struct_archive!): c_int;
pub extern fn archive_errno(a: *struct_archive!): c_int;
pub extern fn archive_format_name(a: *struct_archive!): [*c]const u8;
pub extern fn archive_format(a: *struct_archive!): c_int;
pub extern fn archive_clear_error(a: *struct_archive!): void;
pub extern fn archive_set_error(a: *struct_archive!, _err: c_int, fmt: [*c]const u8, ...): void;
pub extern fn archive_copy_error(dest: *struct_archive!, src: *struct_archive!): void;
pub extern fn archive_file_count(a: *struct_archive!): c_int;
pub extern fn archive_match_new(): *struct_archive!;
pub extern fn archive_match_free(a: *struct_archive!): c_int;
pub extern fn archive_match_excluded(a: *struct_archive!, e: *struct_archive_entry!): c_int;
pub extern fn archive_match_path_excluded(a: *struct_archive!, e: *struct_archive_entry!): c_int;
pub extern fn archive_match_set_inclusion_recursion(a: *struct_archive!, v: c_int): c_int;
pub extern fn archive_match_exclude_pattern(a: *struct_archive!, p: [*c]const u8): c_int;
pub extern fn archive_match_exclude_pattern_w(a: *struct_archive!, p: [*c]const wchar_t): c_int;
pub extern fn archive_match_exclude_pattern_from_file(a: *struct_archive!, p: [*c]const u8, _nullSeparator: c_int): c_int;
pub extern fn archive_match_exclude_pattern_from_file_w(a: *struct_archive!, p: [*c]const wchar_t, _nullSeparator: c_int): c_int;
pub extern fn archive_match_include_pattern(a: *struct_archive!, p: [*c]const u8): c_int;
pub extern fn archive_match_include_pattern_w(a: *struct_archive!, p: [*c]const wchar_t): c_int;
pub extern fn archive_match_include_pattern_from_file(a: *struct_archive!, p: [*c]const u8, _nullSeparator: c_int): c_int;
pub extern fn archive_match_include_pattern_from_file_w(a: *struct_archive!, p: [*c]const wchar_t, _nullSeparator: c_int): c_int;
pub extern fn archive_match_path_unmatched_inclusions(a: *struct_archive!): c_int;
pub extern fn archive_match_path_unmatched_inclusions_next(a: *struct_archive!, p: [*c][*c]const u8): c_int;
pub extern fn archive_match_path_unmatched_inclusions_next_w(a: *struct_archive!, p: [*c][*c]const wchar_t): c_int;
pub extern fn archive_match_time_excluded(a: *struct_archive!, e: *struct_archive_entry!): c_int;
pub extern fn archive_match_include_time(a: *struct_archive!, _flag: c_int, _sec: time_t, _nsec: c_long): c_int;
pub extern fn archive_match_include_date(a: *struct_archive!, _flag: c_int, _datestr: [*c]const u8): c_int;
pub extern fn archive_match_include_date_w(a: *struct_archive!, _flag: c_int, _datestr: [*c]const wchar_t): c_int;
pub extern fn archive_match_include_file_time(a: *struct_archive!, _flag: c_int, _pathname: [*c]const u8): c_int;
pub extern fn archive_match_include_file_time_w(a: *struct_archive!, _flag: c_int, _pathname: [*c]const wchar_t): c_int;
pub extern fn archive_match_exclude_entry(a: *struct_archive!, _flag: c_int, e: *struct_archive_entry!): c_int;
pub extern fn archive_match_owner_excluded(a: *struct_archive!, e: *struct_archive_entry!): c_int;
pub extern fn archive_match_include_uid(a: *struct_archive!, id: la_int64_t): c_int;
pub extern fn archive_match_include_gid(a: *struct_archive!, id: la_int64_t): c_int;
pub extern fn archive_match_include_uname(a: *struct_archive!, n: [*c]const u8): c_int;
pub extern fn archive_match_include_uname_w(a: *struct_archive!, n: [*c]const wchar_t): c_int;
pub extern fn archive_match_include_gname(a: *struct_archive!, n: [*c]const u8): c_int;
pub extern fn archive_match_include_gname_w(a: *struct_archive!, n: [*c]const wchar_t): c_int;
pub extern fn archive_utility_string_sort(s: [*c][*c]u8): c_int;

pub extern fn archive_entry_clone(e: *struct_archive_entry!): *struct_archive_entry!;
pub extern fn archive_entry_atime(e: *struct_archive_entry!): time_t;
pub extern fn archive_entry_atime_nsec(e: *struct_archive_entry!): c_long;
pub extern fn archive_entry_atime_is_set(e: *struct_archive_entry!): c_int;
pub extern fn archive_entry_birthtime(e: *struct_archive_entry!): time_t;
pub extern fn archive_entry_birthtime_nsec(e: *struct_archive_entry!): c_long;
pub extern fn archive_entry_birthtime_is_set(e: *struct_archive_entry!): c_int;
pub extern fn archive_entry_ctime(e: *struct_archive_entry!): time_t;
pub extern fn archive_entry_ctime_nsec(e: *struct_archive_entry!): c_long;
pub extern fn archive_entry_ctime_is_set(e: *struct_archive_entry!): c_int;
pub extern fn archive_entry_dev(e: *struct_archive_entry!): dev_t;
pub extern fn archive_entry_dev_is_set(e: *struct_archive_entry!): c_int;
pub extern fn archive_entry_devmajor(e: *struct_archive_entry!): dev_t;
pub extern fn archive_entry_devminor(e: *struct_archive_entry!): dev_t;
pub extern fn archive_entry_fflags(e: *struct_archive_entry!, s: [*c]u64, c_f: [*c]u64): void;
pub extern fn archive_entry_fflags_text(e: *struct_archive_entry!): [*c]const u8;
pub extern fn archive_entry_gid(e: *struct_archive_entry!): la_int64_t;
pub extern fn archive_entry_gname(e: *struct_archive_entry!): [*c]const u8;
pub extern fn archive_entry_gname_utf8(e: *struct_archive_entry!): [*c]const u8;
pub extern fn archive_entry_gname_w(e: *struct_archive_entry!): [*c]const wchar_t;
pub extern fn archive_entry_hardlink(e: *struct_archive_entry!): [*c]const u8;
pub extern fn archive_entry_hardlink_utf8(e: *struct_archive_entry!): [*c]const u8;
pub extern fn archive_entry_hardlink_w(e: *struct_archive_entry!): [*c]const wchar_t;
pub extern fn archive_entry_ino(e: *struct_archive_entry!): la_int64_t;
pub extern fn archive_entry_ino64(e: *struct_archive_entry!): la_int64_t;
pub extern fn archive_entry_ino_is_set(e: *struct_archive_entry!): c_int;
pub extern fn archive_entry_mode(e: *struct_archive_entry!): mode_t;
pub extern fn archive_entry_mtime(e: *struct_archive_entry!): time_t;
pub extern fn archive_entry_mtime_nsec(e: *struct_archive_entry!): c_long;
pub extern fn archive_entry_mtime_is_set(e: *struct_archive_entry!): c_int;
pub extern fn archive_entry_nlink(e: *struct_archive_entry!): c_uint;
pub extern fn archive_entry_rdev(e: *struct_archive_entry!): dev_t;
pub extern fn archive_entry_rdevmajor(e: *struct_archive_entry!): dev_t;
pub extern fn archive_entry_rdevminor(e: *struct_archive_entry!): dev_t;
pub extern fn archive_entry_sourcepath(e: *struct_archive_entry!): [*c]const u8;
pub extern fn archive_entry_sourcepath_w(e: *struct_archive_entry!): [*c]const wchar_t;
pub extern fn archive_entry_size_is_set(e: *struct_archive_entry!): c_int;
pub extern fn archive_entry_strmode(e: *struct_archive_entry!): [*c]const u8;
pub extern fn archive_entry_uid(e: *struct_archive_entry!): la_int64_t;
pub extern fn archive_entry_uname(e: *struct_archive_entry!): [*c]const u8;
pub extern fn archive_entry_uname_utf8(e: *struct_archive_entry!): [*c]const u8;
pub extern fn archive_entry_uname_w(e: *struct_archive_entry!): [*c]const wchar_t;
pub extern fn archive_entry_is_data_encrypted(e: *struct_archive_entry!): c_int;
pub extern fn archive_entry_is_metadata_encrypted(e: *struct_archive_entry!): c_int;
pub extern fn archive_entry_is_encrypted(e: *struct_archive_entry!): c_int;
pub extern fn archive_entry_set_atime(e: *struct_archive_entry!, s: time_t, ns: c_long): void;
pub extern fn archive_entry_unset_atime(e: *struct_archive_entry!): void;
pub extern fn archive_entry_set_birthtime(e: *struct_archive_entry!, s: time_t, ns: c_long): void;
pub extern fn archive_entry_unset_birthtime(e: *struct_archive_entry!): void;
pub extern fn archive_entry_set_ctime(e: *struct_archive_entry!, s: time_t, ns: c_long): void;
pub extern fn archive_entry_unset_ctime(e: *struct_archive_entry!): void;
pub extern fn archive_entry_set_dev(e: *struct_archive_entry!, d: dev_t): void;
pub extern fn archive_entry_set_devmajor(e: *struct_archive_entry!, d: dev_t): void;
pub extern fn archive_entry_set_devminor(e: *struct_archive_entry!, d: dev_t): void;
pub extern fn archive_entry_set_fflags(e: *struct_archive_entry!, s: u64, c_f: u64): void;
pub extern fn archive_entry_copy_fflags_text(e: *struct_archive_entry!, t: [*c]const u8): [*c]const u8;
pub extern fn archive_entry_copy_fflags_text_w(e: *struct_archive_entry!, t: [*c]const wchar_t): [*c]const wchar_t;
pub extern fn archive_entry_set_gid(e: *struct_archive_entry!, g: la_int64_t): void;
pub extern fn archive_entry_set_gname(e: *struct_archive_entry!, n: [*c]const u8): void;
pub extern fn archive_entry_set_gname_utf8(e: *struct_archive_entry!, n: [*c]const u8): void;
pub extern fn archive_entry_copy_gname(e: *struct_archive_entry!, n: [*c]const u8): void;
pub extern fn archive_entry_copy_gname_w(e: *struct_archive_entry!, n: [*c]const wchar_t): void;
pub extern fn archive_entry_update_gname_utf8(e: *struct_archive_entry!, n: [*c]const u8): c_int;
pub extern fn archive_entry_set_hardlink(e: *struct_archive_entry!, n: [*c]const u8): void;
pub extern fn archive_entry_set_hardlink_utf8(e: *struct_archive_entry!, n: [*c]const u8): void;
pub extern fn archive_entry_copy_hardlink(e: *struct_archive_entry!, n: [*c]const u8): void;
pub extern fn archive_entry_copy_hardlink_w(e: *struct_archive_entry!, n: [*c]const wchar_t): void;
pub extern fn archive_entry_update_hardlink_utf8(e: *struct_archive_entry!, n: [*c]const u8): c_int;
pub extern fn archive_entry_set_ino(e: *struct_archive_entry!, i: la_int64_t): void;
pub extern fn archive_entry_set_ino64(e: *struct_archive_entry!, i: la_int64_t): void;
pub extern fn archive_entry_set_link(e: *struct_archive_entry!, n: [*c]const u8): void;
pub extern fn archive_entry_set_link_utf8(e: *struct_archive_entry!, n: [*c]const u8): void;
pub extern fn archive_entry_copy_link(e: *struct_archive_entry!, n: [*c]const u8): void;
pub extern fn archive_entry_copy_link_w(e: *struct_archive_entry!, n: [*c]const wchar_t): void;
pub extern fn archive_entry_update_link_utf8(e: *struct_archive_entry!, n: [*c]const u8): c_int;
pub extern fn archive_entry_unset_mtime(e: *struct_archive_entry!): void;
pub extern fn archive_entry_set_nlink(e: *struct_archive_entry!, n: c_uint): void;
pub extern fn archive_entry_update_pathname_utf8(e: *struct_archive_entry!, n: [*c]const u8): c_int;
pub extern fn archive_entry_set_rdev(e: *struct_archive_entry!, d: dev_t): void;
pub extern fn archive_entry_set_rdevmajor(e: *struct_archive_entry!, d: dev_t): void;
pub extern fn archive_entry_set_rdevminor(e: *struct_archive_entry!, d: dev_t): void;
pub extern fn archive_entry_unset_size(e: *struct_archive_entry!): void;
pub extern fn archive_entry_copy_sourcepath(e: *struct_archive_entry!, n: [*c]const u8): void;
pub extern fn archive_entry_copy_sourcepath_w(e: *struct_archive_entry!, n: [*c]const wchar_t): void;
pub extern fn archive_entry_set_symlink(e: *struct_archive_entry!, n: [*c]const u8): void;
pub extern fn archive_entry_set_symlink_type(e: *struct_archive_entry!, t: c_int): void;
pub extern fn archive_entry_set_symlink_utf8(e: *struct_archive_entry!, n: [*c]const u8): void;
pub extern fn archive_entry_copy_symlink(e: *struct_archive_entry!, n: [*c]const u8): void;
pub extern fn archive_entry_copy_symlink_w(e: *struct_archive_entry!, n: [*c]const wchar_t): void;
pub extern fn archive_entry_update_symlink_utf8(e: *struct_archive_entry!, n: [*c]const u8): c_int;
pub extern fn archive_entry_set_uid(e: *struct_archive_entry!, u: la_int64_t): void;
pub extern fn archive_entry_set_uname(e: *struct_archive_entry!, n: [*c]const u8): void;
pub extern fn archive_entry_set_uname_utf8(e: *struct_archive_entry!, n: [*c]const u8): void;
pub extern fn archive_entry_copy_uname(e: *struct_archive_entry!, n: [*c]const u8): void;
pub extern fn archive_entry_copy_uname_w(e: *struct_archive_entry!, n: [*c]const wchar_t): void;
pub extern fn archive_entry_update_uname_utf8(e: *struct_archive_entry!, n: [*c]const u8): c_int;
pub extern fn archive_entry_set_is_data_encrypted(e: *struct_archive_entry!, is_encrypted: u8): void;
pub extern fn archive_entry_set_is_metadata_encrypted(e: *struct_archive_entry!, is_encrypted: u8): void;
pub extern fn archive_entry_stat(e: *struct_archive_entry!): (*const struct_stat!)?;
pub extern fn archive_entry_copy_stat(e: *struct_archive_entry!, s: (*const struct_stat!)?): void;
pub extern fn archive_entry_mac_metadata(e: *struct_archive_entry!, s: [*c]usize): *const !?;
pub extern fn archive_entry_copy_mac_metadata(e: *struct_archive_entry!, d: *const !?, s: usize): void;
pub extern fn archive_entry_digest(e: *struct_archive_entry!, t: c_int): [*c]const u8;
pub extern fn archive_entry_acl_clear(e: *struct_archive_entry!): void;
pub extern fn archive_entry_acl_add_entry(e: *struct_archive_entry!, t: c_int, p: c_int, s: c_int, q: c_int, n: [*c]const u8): c_int;
pub extern fn archive_entry_acl_add_entry_w(e: *struct_archive_entry!, t: c_int, p: c_int, s: c_int, q: c_int, n: [*c]const wchar_t): c_int;
pub extern fn archive_entry_acl_reset(e: *struct_archive_entry!, t: c_int): c_int;
pub extern fn archive_entry_acl_next(e: *struct_archive_entry!, t: c_int, rt: [*c]c_int, rp: [*c]c_int, rs: [*c]c_int, rq: [*c]c_int, rn: [*c][*c]const u8): c_int;
pub extern fn archive_entry_acl_to_text_w(e: *struct_archive_entry!, l: [*c]la_ssize_t, f: c_int): [*c]wchar_t;
pub extern fn archive_entry_acl_to_text(e: *struct_archive_entry!, l: [*c]la_ssize_t, f: c_int): [*c]u8;
pub extern fn archive_entry_acl_from_text_w(e: *struct_archive_entry!, t: [*c]const wchar_t, f: c_int): c_int;
pub extern fn archive_entry_acl_from_text(e: *struct_archive_entry!, t: [*c]const u8, f: c_int): c_int;
pub extern fn archive_entry_acl_text_w(e: *struct_archive_entry!, f: c_int): [*c]const wchar_t;
pub extern fn archive_entry_acl_text(e: *struct_archive_entry!, f: c_int): [*c]const u8;
pub extern fn archive_entry_acl_types(e: *struct_archive_entry!): c_int;
pub extern fn archive_entry_acl_count(e: *struct_archive_entry!, t: c_int): c_int;
pub extern fn archive_entry_acl(e: *struct_archive_entry!): *struct_archive_acl!;
pub extern fn archive_entry_xattr_clear(e: *struct_archive_entry!): void;
pub extern fn archive_entry_xattr_add_entry(e: *struct_archive_entry!, n: [*c]const u8, v: *const !?, s: usize): void;
pub extern fn archive_entry_xattr_count(e: *struct_archive_entry!): c_int;
pub extern fn archive_entry_xattr_reset(e: *struct_archive_entry!): c_int;
pub extern fn archive_entry_xattr_next(e: *struct_archive_entry!, n: [*c][*c]const u8, v: [*c]*const !?, s: [*c]usize): c_int;
pub extern fn archive_entry_sparse_clear(e: *struct_archive_entry!): void;
pub extern fn archive_entry_sparse_add_entry(e: *struct_archive_entry!, o: la_int64_t, l: la_int64_t): void;
pub extern fn archive_entry_sparse_count(e: *struct_archive_entry!): c_int;
pub extern fn archive_entry_sparse_reset(e: *struct_archive_entry!): c_int;
pub extern fn archive_entry_sparse_next(e: *struct_archive_entry!, o: [*c]la_int64_t, l: [*c]la_int64_t): c_int;
pub extern fn archive_entry_linkresolver_new(): *struct_archive_entry_linkresolver!;
pub extern fn archive_entry_linkresolver_set_strategy(r: *struct_archive_entry_linkresolver!, f: c_int): void;
pub extern fn archive_entry_linkresolver_free(r: *struct_archive_entry_linkresolver!): void;
pub extern fn archive_entry_linkify(r: *struct_archive_entry_linkresolver!, e1: [*c]*struct_archive_entry!, e2: [*c]*struct_archive_entry!): void;
pub extern fn archive_entry_partial_links(res: *struct_archive_entry_linkresolver!, links: [*c]c_uint): *struct_archive_entry!;

pub const AE_SYMLINK_TYPE_UNDEFINED: c_int = 0;
pub const AE_SYMLINK_TYPE_FILE: c_int = 1;
pub const AE_SYMLINK_TYPE_DIRECTORY: c_int = 2;
pub const ARCHIVE_ENTRY_DIGEST_MD5: c_int = 0x00000001;
pub const ARCHIVE_ENTRY_DIGEST_RMD160: c_int = 0x00000002;
pub const ARCHIVE_ENTRY_DIGEST_SHA1: c_int = 0x00000003;
pub const ARCHIVE_ENTRY_DIGEST_SHA256: c_int = 0x00000004;
pub const ARCHIVE_ENTRY_DIGEST_SHA384: c_int = 0x00000005;
pub const ARCHIVE_ENTRY_DIGEST_SHA512: c_int = 0x00000006;
pub const ARCHIVE_ENTRY_ACL_EXECUTE: c_int = 0x00000001;
pub const ARCHIVE_ENTRY_ACL_WRITE: c_int = 0x00000002;
pub const ARCHIVE_ENTRY_ACL_READ: c_int = 0x00000004;
pub const ARCHIVE_ENTRY_ACL_READ_DATA: c_int = 0x00000008;
pub const ARCHIVE_ENTRY_ACL_LIST_DIRECTORY: c_int = 0x00000008;
pub const ARCHIVE_ENTRY_ACL_WRITE_DATA: c_int = 0x00000010;
pub const ARCHIVE_ENTRY_ACL_ADD_FILE: c_int = 0x00000010;
pub const ARCHIVE_ENTRY_ACL_APPEND_DATA: c_int = 0x00000020;
pub const ARCHIVE_ENTRY_ACL_ADD_SUBDIRECTORY: c_int = 0x00000020;
pub const ARCHIVE_ENTRY_ACL_READ_NAMED_ATTRS: c_int = 0x00000040;
pub const ARCHIVE_ENTRY_ACL_WRITE_NAMED_ATTRS: c_int = 0x00000080;
pub const ARCHIVE_ENTRY_ACL_DELETE_CHILD: c_int = 0x00000100;
pub const ARCHIVE_ENTRY_ACL_READ_ATTRIBUTES: c_int = 0x00000200;
pub const ARCHIVE_ENTRY_ACL_WRITE_ATTRIBUTES: c_int = 0x00000400;
pub const ARCHIVE_ENTRY_ACL_DELETE: c_int = 0x00000800;
pub const ARCHIVE_ENTRY_ACL_READ_ACL: c_int = 0x00001000;
pub const ARCHIVE_ENTRY_ACL_WRITE_ACL: c_int = 0x00002000;
pub const ARCHIVE_ENTRY_ACL_WRITE_OWNER: c_int = 0x00004000;
pub const ARCHIVE_ENTRY_ACL_SYNCHRONIZE: c_int = 0x00008000;
pub const ARCHIVE_ENTRY_ACL_PERMS_POSIX1E: c_int = (ARCHIVE_ENTRY_ACL_EXECUTE | ARCHIVE_ENTRY_ACL_WRITE) | ARCHIVE_ENTRY_ACL_READ;
pub const ARCHIVE_ENTRY_ACL_PERMS_NFS4: c_int = (((((((((((((((ARCHIVE_ENTRY_ACL_EXECUTE | ARCHIVE_ENTRY_ACL_READ_DATA) | ARCHIVE_ENTRY_ACL_LIST_DIRECTORY) | ARCHIVE_ENTRY_ACL_WRITE_DATA) | ARCHIVE_ENTRY_ACL_ADD_FILE) | ARCHIVE_ENTRY_ACL_APPEND_DATA) | ARCHIVE_ENTRY_ACL_ADD_SUBDIRECTORY) | ARCHIVE_ENTRY_ACL_READ_NAMED_ATTRS) | ARCHIVE_ENTRY_ACL_WRITE_NAMED_ATTRS) | ARCHIVE_ENTRY_ACL_DELETE_CHILD) | ARCHIVE_ENTRY_ACL_READ_ATTRIBUTES) | ARCHIVE_ENTRY_ACL_WRITE_ATTRIBUTES) | ARCHIVE_ENTRY_ACL_DELETE) | ARCHIVE_ENTRY_ACL_READ_ACL) | ARCHIVE_ENTRY_ACL_WRITE_ACL) | ARCHIVE_ENTRY_ACL_WRITE_OWNER) | ARCHIVE_ENTRY_ACL_SYNCHRONIZE;
pub const ARCHIVE_ENTRY_ACL_ENTRY_INHERITED: c_int = 0x01000000;
pub const ARCHIVE_ENTRY_ACL_ENTRY_FILE_INHERIT: c_int = 0x02000000;
pub const ARCHIVE_ENTRY_ACL_ENTRY_DIRECTORY_INHERIT: c_int = 0x04000000;
pub const ARCHIVE_ENTRY_ACL_ENTRY_NO_PROPAGATE_INHERIT: c_int = 0x08000000;
pub const ARCHIVE_ENTRY_ACL_ENTRY_INHERIT_ONLY: c_int = 0x10000000;
pub const ARCHIVE_ENTRY_ACL_ENTRY_SUCCESSFUL_ACCESS: c_int = 0x20000000;
pub const ARCHIVE_ENTRY_ACL_ENTRY_FAILED_ACCESS: c_int = 0x40000000;
pub const ARCHIVE_ENTRY_ACL_INHERITANCE_NFS4: c_int = (((((ARCHIVE_ENTRY_ACL_ENTRY_FILE_INHERIT | ARCHIVE_ENTRY_ACL_ENTRY_DIRECTORY_INHERIT) | ARCHIVE_ENTRY_ACL_ENTRY_NO_PROPAGATE_INHERIT) | ARCHIVE_ENTRY_ACL_ENTRY_INHERIT_ONLY) | ARCHIVE_ENTRY_ACL_ENTRY_SUCCESSFUL_ACCESS) | ARCHIVE_ENTRY_ACL_ENTRY_FAILED_ACCESS) | ARCHIVE_ENTRY_ACL_ENTRY_INHERITED;
pub const ARCHIVE_ENTRY_ACL_TYPE_ACCESS: c_int = 0x00000100;
pub const ARCHIVE_ENTRY_ACL_TYPE_DEFAULT: c_int = 0x00000200;
pub const ARCHIVE_ENTRY_ACL_TYPE_ALLOW: c_int = 0x00000400;
pub const ARCHIVE_ENTRY_ACL_TYPE_DENY: c_int = 0x00000800;
pub const ARCHIVE_ENTRY_ACL_TYPE_AUDIT: c_int = 0x00001000;
pub const ARCHIVE_ENTRY_ACL_TYPE_ALARM: c_int = 0x00002000;
pub const ARCHIVE_ENTRY_ACL_TYPE_POSIX1E: c_int = ARCHIVE_ENTRY_ACL_TYPE_ACCESS | ARCHIVE_ENTRY_ACL_TYPE_DEFAULT;
pub const ARCHIVE_ENTRY_ACL_TYPE_NFS4: c_int = ((ARCHIVE_ENTRY_ACL_TYPE_ALLOW | ARCHIVE_ENTRY_ACL_TYPE_DENY) | ARCHIVE_ENTRY_ACL_TYPE_AUDIT) | ARCHIVE_ENTRY_ACL_TYPE_ALARM;
pub const ARCHIVE_ENTRY_ACL_USER: c_int = 10001;
pub const ARCHIVE_ENTRY_ACL_USER_OBJ: c_int = 10002;
pub const ARCHIVE_ENTRY_ACL_GROUP: c_int = 10003;
pub const ARCHIVE_ENTRY_ACL_GROUP_OBJ: c_int = 10004;
pub const ARCHIVE_ENTRY_ACL_MASK: c_int = 10005;
pub const ARCHIVE_ENTRY_ACL_OTHER: c_int = 10006;
pub const ARCHIVE_ENTRY_ACL_EVERYONE: c_int = 10107;
pub const ARCHIVE_ENTRY_ACL_STYLE_EXTRA_ID: c_int = 0x00000001;
pub const ARCHIVE_ENTRY_ACL_STYLE_MARK_DEFAULT: c_int = 0x00000002;
pub const ARCHIVE_ENTRY_ACL_STYLE_SOLARIS: c_int = 0x00000004;
pub const ARCHIVE_ENTRY_ACL_STYLE_SEPARATOR_COMMA: c_int = 0x00000008;
pub const ARCHIVE_ENTRY_ACL_STYLE_COMPACT: c_int = 0x00000010;
pub const OLD_ARCHIVE_ENTRY_ACL_STYLE_EXTRA_ID: c_int = 1024;
pub const OLD_ARCHIVE_ENTRY_ACL_STYLE_MARK_DEFAULT: c_int = 2048;

const ByteList = std.ArrayListUnmanaged(u8);

pub struct GrowingBuffer {
    list: ByteList = .empty,
    allocator: std.mem.Allocator,
    had_error: bool = false,

    pub static fn init(allocator: std.mem.Allocator): GrowingBuffer {
        return GrowingBuffer{ allocator: allocator };
    }

    pub fn deinit(*this): void {
        this.list.deinit(this.allocator);
    }

    pub fn toOwnedSlice(*this): ([]u8) ! OOM {
        if (this.had_error) return err.OutOfMemory;
        return this.list.toOwnedSlice(this.allocator);
    }

    @c pub static fn openCallback(_: *struct_archive!, client_data: *!): c_int {
        unsafe {
            const self = client_data as *GrowingBuffer!;
            self.list.clearRetainingCapacity();
            self.had_error = false;
            return 0;
        }
    }

    @c pub static fn writeCallback(_: *struct_archive!, client_data: *!, buff: *const !?, length: usize): la_ssize_t {
        unsafe {
            const self = client_data as *GrowingBuffer!;
            if (buff == null || length == 0) return 0;
            const data: [*]const u8 = buff.? as [*]const u8!;
            self.list.appendSlice(self.allocator, data[0..length]) catch {
                self.had_error = true;
                return -1;
            };
            return la_ssize_t(length);
        }
    }

    @c pub static fn closeCallback(_: *struct_archive!, _: *!): c_int {
        return 0;
    }
}
