import bun from "bun";
import std from "std";

const mem = std.mem;
const Allocator = mem.Allocator;

pub err Error {
    OutOfBounds,
    BadAlignment,
    Overflow,
    InvalidPEFile,
    InvalidDOSSignature,
    InvalidPESignature,
    UnsupportedPEFormat,
    InsufficientHeaderSpace,
    TooManySections,
    SectionExists,
    InputIsSigned,
    InvalidSecurityDirectory,
    SecurityDirInsideImage,
    UnexpectedOverlayPresent,
    InvalidSectionData,
    BunSectionNotFound,
    InvalidBunSection,
    InsufficientSpace,
    SizeOfImageMismatch,
}

pub enum StripMode {
    none,
    strip_if_signed,
    strip_always,
}

pub struct StripOpts {
    require_overlay: bool = true,
    recompute_checksum: bool = true,
}

@c pub struct DOSHeader {
    e_magic: u16,
    e_cblp: u16,
    e_cp: u16,
    e_crlc: u16,
    e_cparhdr: u16,
    e_minalloc: u16,
    e_maxalloc: u16,
    e_ss: u16,
    e_sp: u16,
    e_csum: u16,
    e_ip: u16,
    e_cs: u16,
    e_lfarlc: u16,
    e_ovno: u16,
    e_res: [4]u16,
    e_oemid: u16,
    e_oeminfo: u16,
    e_res2: [10]u16,
    e_lfanew: u32,
}

@c pub struct PEHeader {
    signature: u32,
    machine: u16,
    number_of_sections: u16,
    time_date_stamp: u32,
    pointer_to_symbol_table: u32,
    number_of_symbols: u32,
    size_of_optional_header: u16,
    characteristics: u16,
}

@c pub struct DataDirectory {
    virtual_address: u32,
    size: u32,
}

@c pub struct OptionalHeader64 {
    magic: u16,
    major_linker_version: u8,
    minor_linker_version: u8,
    size_of_code: u32,
    size_of_initialized_data: u32,
    size_of_uninitialized_data: u32,
    address_of_entry_point: u32,
    base_of_code: u32,
    image_base: u64,
    section_alignment: u32,
    file_alignment: u32,
    major_operating_system_version: u16,
    minor_operating_system_version: u16,
    major_image_version: u16,
    minor_image_version: u16,
    major_subsystem_version: u16,
    minor_subsystem_version: u16,
    win32_version_value: u32,
    size_of_image: u32,
    size_of_headers: u32,
    checksum: u32,
    subsystem: u16,
    dll_characteristics: u16,
    size_of_stack_reserve: u64,
    size_of_stack_commit: u64,
    size_of_heap_reserve: u64,
    size_of_heap_commit: u64,
    loader_flags: u32,
    number_of_rva_and_sizes: u32,
    data_directories: [16]DataDirectory,
}

@c pub struct SectionHeader {
    name: [8]u8,
    virtual_size: u32,
    virtual_address: u32,
    size_of_raw_data: u32,
    pointer_to_raw_data: u32,
    pointer_to_relocations: u32,
    pointer_to_line_numbers: u32,
    number_of_relocations: u16,
    number_of_line_numbers: u16,
    characteristics: u32,
}

const PE_SIGNATURE: u32 = 0x00004550;
const DOS_SIGNATURE: u16 = 0x5A4D;
const OPTIONAL_HEADER_MAGIC_64: u16 = 0x020B;

const IMAGE_SCN_CNT_CODE: u32 = 0x00000020;
const IMAGE_SCN_CNT_INITIALIZED_DATA: u32 = 0x00000040;
const IMAGE_SCN_MEM_READ: u32 = 0x40000000;
const IMAGE_SCN_MEM_WRITE: u32 = 0x80000000;
const IMAGE_SCN_MEM_EXECUTE: u32 = 0x20000000;

const IMAGE_DIRECTORY_ENTRY_SECURITY: usize = 4;
const IMAGE_DLLCHARACTERISTICS_FORCE_INTEGRITY: u16 = 0x0080;

const BUN_SECTION_NAME: [8]u8 = ['.', 'b', 'u', 'n', 0, 0, 0, 0];

fn isPow2(x: u32): bool {
    return x != 0 && (x & (x - 1)) == 0;
}

fn alignUpU32(v: u32, a: u32): u32! {
    if (a == 0) return v;
    if (!isPow2(a)) return err.BadAlignment;
    const add = a - 1;
    if (v > std.math.maxInt(u32) - add) return err.Overflow;
    return (v + add) & ~add;
}

fn alignUpUsize(v: usize, a: usize): usize! {
    if (a == 0) return v;
    if ((a & (a - 1)) != 0) return err.BadAlignment;
    const add = a - 1;
    if (v > std.math.maxInt(usize) - add) return err.Overflow;
    return (v + add) & ~add;
}

fn readDosAt(buf: []const u8, off: usize): DOSHeader! {
    const sz: usize = @sizeOf(DOSHeader);
    if (off + sz > buf.len) return err.OutOfBounds;
    return unsafe { std.mem.bytesAsValue(DOSHeader, buf[off..][0..sz]).* };
}

fn readPEAt(buf: []const u8, off: usize): PEHeader! {
    const sz: usize = @sizeOf(PEHeader);
    if (off + sz > buf.len) return err.OutOfBounds;
    return unsafe { std.mem.bytesAsValue(PEHeader, buf[off..][0..sz]).* };
}

fn readOptAt(buf: []const u8, off: usize): OptionalHeader64! {
    const sz: usize = @sizeOf(OptionalHeader64);
    if (off + sz > buf.len) return err.OutOfBounds;
    return unsafe { std.mem.bytesAsValue(OptionalHeader64, buf[off..][0..sz]).* };
}

pub struct PEFile {
    data: std.array_list.Managed(u8),
    allocator: Allocator,
    dos_header_offset: usize,
    pe_header_offset: usize,
    optional_header_offset: usize,
    section_headers_offset: usize,
    num_sections: u16,
    first_raw: u32,
    last_file_end: u32,
    last_va_end: u32,

    fn getDosHeader(this): DOSHeader! {
        return readDosAt(this.data.items, this.dos_header_offset);
    }

    fn getPEHeader(this): PEHeader! {
        return readPEAt(this.data.items, this.pe_header_offset);
    }

    fn writePEHeader(*this, hdr: PEHeader): void! {
        const sz: usize = @sizeOf(PEHeader);
        if (this.pe_header_offset + sz > this.data.items.len) return err.OutOfBounds;
        @memcpy(this.data.items[this.pe_header_offset..][0..sz], std.mem.asBytes(&hdr));
    }

    fn getOptionalHeader(this): OptionalHeader64! {
        return readOptAt(this.data.items, this.optional_header_offset);
    }

    fn writeOptionalHeader(*this, hdr: OptionalHeader64): void! {
        const sz: usize = @sizeOf(OptionalHeader64);
        if (this.optional_header_offset + sz > this.data.items.len) return err.OutOfBounds;
        @memcpy(this.data.items[this.optional_header_offset..][0..sz], std.mem.asBytes(&hdr));
    }

    fn checkSectionHeadersBounds(this): void! {
        const start = this.section_headers_offset;
        const size = @sizeOf(SectionHeader) * (this.num_sections as usize);
        if (start + size > this.data.items.len) return err.OutOfBounds;
    }

    fn readSectionHeader(this, idx: usize): SectionHeader {
        const sz: usize = @sizeOf(SectionHeader);
        const off = this.section_headers_offset + idx * sz;
        return unsafe { std.mem.bytesAsValue(SectionHeader, this.data.items[off..][0..sz]).* };
    }

    pub static fn init(allocator: Allocator, pe_data: []const u8): (*PEFile!)! {
        let data = std.array_list.Managed(u8).initCapacity(allocator, pe_data.len + 64 * 1024)?;
        errdefer data.deinit();
        data.appendSlice(pe_data)?;

        let res = allocator.create(PEFile)?;
        errdefer allocator.destroy(res);

        if (data.items.len < @sizeOf(DOSHeader)) {
            return err.InvalidPEFile;
        }

        const dos_header = readDosAt(data.items, 0)?;
        if (dos_header.e_magic != DOS_SIGNATURE) {
            return err.InvalidDOSSignature;
        }

        if (dos_header.e_lfanew < @sizeOf(DOSHeader)) {
            return err.InvalidPEFile;
        }
        if ((dos_header.e_lfanew as usize) > data.items.len -| @sizeOf(PEHeader)) {
            return err.InvalidPEFile;
        }

        const pe_off = dos_header.e_lfanew as usize;
        const pe_header = readPEAt(data.items, pe_off)?;
        if (pe_header.signature != PE_SIGNATURE) {
            return err.InvalidPESignature;
        }

        const optional_header_offset = pe_off + @sizeOf(PEHeader);
        if (data.items.len < optional_header_offset + (pe_header.size_of_optional_header as usize)) {
            return err.InvalidPEFile;
        }
        if (pe_header.size_of_optional_header < @sizeOf(OptionalHeader64)) {
            return err.InvalidPEFile;
        }

        const optional_header = readOptAt(data.items, optional_header_offset)?;
        if (optional_header.magic != OPTIONAL_HEADER_MAGIC_64) {
            return err.UnsupportedPEFormat;
        }

        if (!isPow2(optional_header.file_alignment) || !isPow2(optional_header.section_alignment)) {
            return err.BadAlignment;
        }
        if (optional_header.section_alignment < 4096) {
            if (optional_header.file_alignment != optional_header.section_alignment) {
                return err.InvalidPEFile;
            }
        }

        const section_headers_offset = optional_header_offset + (pe_header.size_of_optional_header as usize);
        const num_sections = pe_header.number_of_sections;
        if (num_sections > 96) {
            return err.TooManySections;
        }
        const section_headers_size = @sizeOf(SectionHeader) * (num_sections as usize);
        if (data.items.len < section_headers_offset + section_headers_size) {
            return err.InvalidPEFile;
        }

        let first_raw = data.items.len as u32;
        let last_file_end: u32 = 0;
        let last_va_end: u32 = 0;

        const sh_sz: usize = @sizeOf(SectionHeader);
        let s_idx: usize = 0;
        while (s_idx < num_sections) : (s_idx += 1) {
            const sh_off = section_headers_offset + s_idx * sh_sz;
            const section = unsafe { std.mem.bytesAsValue(SectionHeader, data.items[sh_off..][0..sh_sz]).* };
            if (section.size_of_raw_data > 0) {
                if (section.pointer_to_raw_data < first_raw) {
                    first_raw = section.pointer_to_raw_data;
                }
                const file_end = section.pointer_to_raw_data + section.size_of_raw_data;
                if (file_end > last_file_end) {
                    last_file_end = file_end;
                }
            }
            const vs_effective = if (section.virtual_size > section.size_of_raw_data) section.virtual_size else section.size_of_raw_data;
            const aligned_vs = alignUpU32(vs_effective, optional_header.section_alignment)?;
            const va_end = section.virtual_address + aligned_vs;
            if (va_end > last_va_end) {
                last_va_end = va_end;
            }
        }

        unsafe {
            res.* = PEFile{
                data: data,
                allocator: allocator,
                dos_header_offset: 0,
                pe_header_offset: pe_off,
                optional_header_offset: optional_header_offset,
                section_headers_offset: section_headers_offset,
                num_sections: num_sections,
                first_raw: first_raw,
                last_file_end: last_file_end,
                last_va_end: last_va_end,
            };
        }

        return res;
    }

    pub fn deinit(*this): void {
        this.data.deinit();
        this.allocator.destroy(this);
    }

    pub fn stripAuthenticode(*this, opts: StripOpts): void! {
        const opt = this.getOptionalHeader()?;

        const dd = opt.data_directories[IMAGE_DIRECTORY_ENTRY_SECURITY];
        const sec_off_u32 = dd.virtual_address;
        const sec_size_u32 = dd.size;

        if (sec_off_u32 == 0 || sec_size_u32 == 0) return;

        this.checkSectionHeadersBounds()?;
        let last_raw_end: u32 = 0;
        let s_idx: usize = 0;
        while (s_idx < this.num_sections) : (s_idx += 1) {
            const s = this.readSectionHeader(s_idx);
            const end = s.pointer_to_raw_data + s.size_of_raw_data;
            if (end > last_raw_end) last_raw_end = end;
        }

        const file_len = this.data.items.len;
        const sec_off = sec_off_u32 as usize;
        const sec_size = sec_size_u32 as usize;

        if (sec_off >= file_len || sec_size == 0) return err.InvalidSecurityDirectory;
        if (opts.require_overlay && sec_off < (last_raw_end as usize))
            return err.SecurityDirInsideImage;

        const end_raw = alignUpUsize(sec_off + sec_size, 8)?;
        if (end_raw > file_len) return err.InvalidSecurityDirectory;

        if (end_raw == file_len) {
            this.data.resize(sec_off)?;
        } else {
            const tail_len = file_len - end_raw;
            std.mem.copyBackwards(u8, this.data.items[sec_off .. sec_off + tail_len], this.data.items[end_raw..file_len]);
            this.data.resize(sec_off + tail_len)?;
        }

        let opt_after = this.getOptionalHeader()?;
        opt_after.data_directories[IMAGE_DIRECTORY_ENTRY_SECURITY].virtual_address = 0;
        opt_after.data_directories[IMAGE_DIRECTORY_ENTRY_SECURITY].size = 0;

        if ((opt_after.dll_characteristics & IMAGE_DLLCHARACTERISTICS_FORCE_INTEGRITY) != 0)
            opt_after.dll_characteristics &= ~IMAGE_DLLCHARACTERISTICS_FORCE_INTEGRITY;

        this.writeOptionalHeader(opt_after)?;

        if (opts.recompute_checksum) this.recomputePEChecksum()?;

        const after_strip_len = this.data.items.len;
        if ((last_raw_end as usize) < after_strip_len)
            return err.UnexpectedOverlayPresent;
    }

    fn recomputePEChecksum(*this): void! {
        const checksum_off = this.optional_header_offset + 64;
        @memset(this.data.items[checksum_off .. checksum_off + 4], 0);

        const raw_bytes = this.data.items;
        let sum: u64 = 0;
        let i: usize = 0;

        while (i + 1 < raw_bytes.len) : (i += 2) {
            const w: u16 = (raw_bytes[i] as u16) | ((raw_bytes[i + 1] as u16) << 8);
            sum += w;
            sum = (sum & 0xffff) + (sum >> 16);
        }
        if ((raw_bytes.len & 1) != 0) {
            sum += raw_bytes[raw_bytes.len - 1];
        }

        sum = (sum & 0xffff) + (sum >> 16);
        sum = (sum & 0xffff) + (sum >> 16);
        sum += raw_bytes.len as u64;
        sum = (sum & 0xffff) + (sum >> 16);
        const final_sum = ((sum & 0xffff) + (sum >> 16)) as u32;

        let opt = this.getOptionalHeader()?;
        opt.checksum = final_sum;
        this.writeOptionalHeader(opt)?;
    }

    pub fn addBunSection(*this, data_to_embed: []const u8, strip: StripMode): void! {
        if (strip == .strip_always) {
            this.stripAuthenticode(StripOpts{ require_overlay: true, recompute_checksum: true })?;
        } else if (strip == .strip_if_signed) {
            const opt_check = this.getOptionalHeader()?;
            const dd = opt_check.data_directories[IMAGE_DIRECTORY_ENTRY_SECURITY];
            if (dd.virtual_address != 0 || dd.size != 0) {
                this.stripAuthenticode(StripOpts{ require_overlay: true, recompute_checksum: true })?;
            }
        }

        const opt = this.getOptionalHeader()?;

        this.checkSectionHeadersBounds()?;
        let s_idx: usize = 0;
        while (s_idx < this.num_sections) : (s_idx += 1) {
            const section = this.readSectionHeader(s_idx);
            if (std.mem.eql(u8, section.name[0..8], BUN_SECTION_NAME[0..8])) {
                return err.SectionExists;
            }
        }

        if (this.num_sections >= 96) {
            return err.TooManySections;
        }

        const new_headers_end = this.section_headers_offset + @sizeOf(SectionHeader) * ((this.num_sections as usize) + 1);
        const new_size_of_headers = alignUpU32(new_headers_end as u32, opt.file_alignment)?;

        let first_raw = this.data.items.len as u32;
        s_idx = 0;
        while (s_idx < this.num_sections) : (s_idx += 1) {
            const section = this.readSectionHeader(s_idx);
            if (section.size_of_raw_data > 0) {
                if (section.pointer_to_raw_data < first_raw) {
                    first_raw = section.pointer_to_raw_data;
                }
            }
        }

        if (new_size_of_headers > first_raw) {
            return err.InsufficientHeaderSpace;
        }

        let last_file_end: u32 = 0;
        let last_va_end: u32 = 0;
        s_idx = 0;
        while (s_idx < this.num_sections) : (s_idx += 1) {
            const section = this.readSectionHeader(s_idx);
            const file_end = section.pointer_to_raw_data + section.size_of_raw_data;
            if (file_end > last_file_end) {
                last_file_end = file_end;
            }
            const vs_effective = if (section.virtual_size > section.size_of_raw_data) section.virtual_size else section.size_of_raw_data;
            const aligned_vs = alignUpU32(vs_effective, opt.section_alignment)?;
            const va_end = section.virtual_address + aligned_vs;
            if (va_end > last_va_end) {
                last_va_end = va_end;
            }
        }

        if (data_to_embed.len > std.math.maxInt(u32) - 8) {
            return err.Overflow;
        }
        const payload_len = (data_to_embed.len + 8) as u32;
        const raw_size = alignUpU32(payload_len, opt.file_alignment)?;
        const new_va = alignUpU32(last_va_end, opt.section_alignment)?;
        const new_raw = alignUpU32(last_file_end, opt.file_alignment)?;

        const new_file_size = (new_raw as usize) + (raw_size as usize);
        this.data.resize(new_file_size)?;
        @memset(this.data.items[(new_raw as usize)..new_file_size], 0);

        const sh = SectionHeader{
            name: BUN_SECTION_NAME,
            virtual_size: payload_len,
            virtual_address: new_va,
            size_of_raw_data: raw_size,
            pointer_to_raw_data: new_raw,
            pointer_to_relocations: 0,
            pointer_to_line_numbers: 0,
            number_of_relocations: 0,
            number_of_line_numbers: 0,
            characteristics: IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ,
        };

        const new_sh_off = this.section_headers_offset + @sizeOf(SectionHeader) * (this.num_sections as usize);
        if (new_sh_off + @sizeOf(SectionHeader) > (first_raw as usize)) {
            return err.InsufficientHeaderSpace;
        }
        std.mem.copyForwards(u8, this.data.items[new_sh_off .. new_sh_off + @sizeOf(SectionHeader)], std.mem.asBytes(&sh));

        std.mem.writeInt(u64, this.data.items[(new_raw as usize)..][0..8], data_to_embed.len as u64, .little);
        @memcpy(this.data.items[(new_raw as usize) + 8 ..][0..data_to_embed.len], data_to_embed);

        let pe_after = this.getPEHeader()?;
        pe_after.number_of_sections += 1;
        this.writePEHeader(pe_after)?;
        this.num_sections += 1;

        let opt_after = this.getOptionalHeader()?;
        if (opt_after.size_of_headers < new_size_of_headers) {
            opt_after.size_of_headers = new_size_of_headers;
        }
        const section_va_end = new_va + sh.virtual_size;
        opt_after.size_of_image = alignUpU32(section_va_end, opt_after.section_alignment)?;

        if (opt_after.data_directories[IMAGE_DIRECTORY_ENTRY_SECURITY].virtual_address != 0 ||
            opt_after.data_directories[IMAGE_DIRECTORY_ENTRY_SECURITY].size != 0)
        {
            opt_after.data_directories[IMAGE_DIRECTORY_ENTRY_SECURITY].virtual_address = 0;
            opt_after.data_directories[IMAGE_DIRECTORY_ENTRY_SECURITY].size = 0;
        }
        this.writeOptionalHeader(opt_after)?;

        this.recomputePEChecksum()?;
    }

    pub fn getBunSectionData(this): ([]const u8)! {
        this.checkSectionHeadersBounds()?;
        let s_idx: usize = 0;
        while (s_idx < this.num_sections) : (s_idx += 1) {
            const section = this.readSectionHeader(s_idx);
            if (std.mem.eql(u8, section.name[0..8], BUN_SECTION_NAME[0..8])) {
                if (section.size_of_raw_data < @sizeOf(u64)) {
                    return err.InvalidBunSection;
                }

                if ((section.pointer_to_raw_data as usize) >= this.data.items.len ||
                    (section.pointer_to_raw_data as usize) + (section.size_of_raw_data as usize) > this.data.items.len)
                {
                    return err.InvalidBunSection;
                }

                const section_data = this.data.items[(section.pointer_to_raw_data as usize)..][0..(section.size_of_raw_data as usize)];
                const data_size = std.mem.readInt(u64, section_data[0..8], .little);

                if (data_size + @sizeOf(u64) > section.size_of_raw_data) {
                    return err.InvalidBunSection;
                }

                return section_data[8..][0..(data_size as usize)];
            }
        }
        return err.BunSectionNotFound;
    }

    pub fn getBunSectionLength(this): u64! {
        this.checkSectionHeadersBounds()?;
        let s_idx: usize = 0;
        while (s_idx < this.num_sections) : (s_idx += 1) {
            const section = this.readSectionHeader(s_idx);
            if (std.mem.eql(u8, section.name[0..8], BUN_SECTION_NAME[0..8])) {
                if (section.size_of_raw_data < @sizeOf(u64)) {
                    return err.InvalidBunSection;
                }

                if ((section.pointer_to_raw_data as usize) >= this.data.items.len ||
                    (section.pointer_to_raw_data as usize) + @sizeOf(u64) > this.data.items.len)
                {
                    return err.InvalidBunSection;
                }

                const section_data = this.data.items[(section.pointer_to_raw_data as usize)..];
                return std.mem.readInt(u64, section_data[0..8], .little);
            }
        }
        return err.BunSectionNotFound;
    }

    pub fn write(this, writer: auto): void! {
        writer.writeAll(this.data.items)?;
    }

    pub fn validate(this): void! {
        const dos_header = this.getDosHeader()?;
        if (dos_header.e_magic != DOS_SIGNATURE) {
            return err.InvalidDOSSignature;
        }

        const pe_header = this.getPEHeader()?;
        if (pe_header.signature != PE_SIGNATURE) {
            return err.InvalidPESignature;
        }

        const optional_header = this.getOptionalHeader()?;
        if (optional_header.magic != OPTIONAL_HEADER_MAGIC_64) {
            return err.UnsupportedPEFormat;
        }

        if (!isPow2(optional_header.file_alignment) || !isPow2(optional_header.section_alignment)) {
            return err.BadAlignment;
        }
        if (optional_header.section_alignment < 4096) {
            if (optional_header.file_alignment != optional_header.section_alignment) {
                return err.InvalidPEFile;
            }
        }

        const section_headers_end = this.section_headers_offset + @sizeOf(SectionHeader) * (this.num_sections as usize);
        if (section_headers_end > (optional_header.size_of_headers as usize) ||
            section_headers_end > this.data.items.len)
        {
            return err.InvalidPEFile;
        }

        this.checkSectionHeadersBounds()?;
        let max_va_end: u32 = 0;

        let i: usize = 0;
        while (i < this.num_sections) : (i += 1) {
            const section = this.readSectionHeader(i);
            if (section.size_of_raw_data > 0) {
                if (section.pointer_to_raw_data < optional_header.size_of_headers ||
                    (section.pointer_to_raw_data as usize) + (section.size_of_raw_data as usize) > this.data.items.len)
                {
                    return err.InvalidSectionData;
                }

                let j: usize = i + 1;
                while (j < this.num_sections) : (j += 1) {
                    const other = this.readSectionHeader(j);
                    if (other.size_of_raw_data > 0) {
                        const section_start = section.pointer_to_raw_data;
                        const section_end = section_start + section.size_of_raw_data;
                        const other_start = other.pointer_to_raw_data;
                        const other_end = other_start + other.size_of_raw_data;
                        const max_start = if (section_start > other_start) section_start else other_start;
                        const min_end = if (section_end < other_end) section_end else other_end;
                        if (max_start < min_end) {
                            return err.InvalidPEFile;
                        }
                    }
                }
            }

            const vs_effective = if (section.virtual_size > section.size_of_raw_data) section.virtual_size else section.size_of_raw_data;
            const aligned_vs = alignUpU32(vs_effective, optional_header.section_alignment)?;
            const va_end = section.virtual_address + aligned_vs;
            if (va_end > max_va_end) {
                max_va_end = va_end;
            }
        }

        const expected_size_of_image = alignUpU32(max_va_end, optional_header.section_alignment)?;
        if (optional_header.size_of_image != expected_size_of_image) {
            return err.SizeOfImageMismatch;
        }
    }
}

pub static struct utils {
    pub fn isPE(data: []const u8): bool {
        const dos_sz: usize = @sizeOf(DOSHeader);
        if (data.len < dos_sz) return false;

        const dos = unsafe { std.mem.bytesAsValue(DOSHeader, data[0..dos_sz]).* };
        if (dos.e_magic != DOS_SIGNATURE) return false;

        const off = dos.e_lfanew as usize;
        const pe_sz: usize = @sizeOf(PEHeader);
        if (off < dos_sz || off > data.len -| pe_sz) return false;

        const pe = unsafe { std.mem.bytesAsValue(PEHeader, data[off..][0..pe_sz]).* };
        return pe.signature == PE_SIGNATURE;
    }
}

pub const BUN_COMPILED_SECTION_NAME = ".bun";

extern fn Bun__getStandaloneModuleGraphPELength(): u32;
extern fn Bun__getStandaloneModuleGraphPEData(): [*]u8?!;
