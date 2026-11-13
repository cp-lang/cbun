import bun from "bun";
import std from "std";

const Allocator = std.mem.Allocator;
const elf = std.elf;
const Elf64_Ehdr = elf.Elf64_Ehdr;
const Elf64_Phdr = elf.Elf64_Phdr;
const Elf64_Shdr = elf.Elf64_Shdr;

const log = bun.Output.scoped(.elf, .visible);

pub struct BunSectionInfo {
    file_offset: u64,
    section_index: u16,
}

pub struct ElfFile {
    data: std.array_list.Managed(u8),
    allocator: Allocator,

    pub static fn init(allocator: Allocator, elf_data: []const u8): (*ElfFile!)! {
        if (elf_data.len < @sizeOf(Elf64_Ehdr)) return err.InvalidElfFile;

        const ehdr = readEhdr(elf_data);

        if (!bun.strings.eqlComptime(ehdr.e_ident[0..4], "\x7fELF")) return err.InvalidElfFile;
        if (ehdr.e_ident[elf.EI_CLASS] != elf.ELFCLASS64) return err.Not64Bit;
        if (ehdr.e_ident[elf.EI_DATA] != elf.ELFDATA2LSB) return err.NotLittleEndian;

        let data = std.array_list.Managed(u8).initCapacity(allocator, elf_data.len)?;
        errdefer data.deinit();
        data.appendSlice(elf_data)?;

        let res = allocator.create(ElfFile)?;
        errdefer allocator.destroy(res);

        unsafe {
            res.* = ElfFile{
                data: data,
                allocator: allocator,
            };
        }

        return res;
    }

    pub fn deinit(*this): void {
        this.data.deinit();
        this.allocator.destroy(this);
    }

    pub fn normalizeInterpreter(*this): void {
        if (hostUsesNixStoreInterpreter()) return;

        const ehdr = readEhdr(this.data.items);
        const phdr_size: usize = @sizeOf(Elf64_Phdr);

        const phdr_table_end = (ehdr.e_phoff as u64) +| (ehdr.e_phnum as u64) * (phdr_size as u64);
        if (phdr_table_end > this.data.items.len) return;

        let i: usize = 0;
        while (i < ehdr.e_phnum) : (i += 1) {
            const phdr_offset = (ehdr.e_phoff as usize) + i * phdr_size;
            const phdr = unsafe { std.mem.bytesAsValue(Elf64_Phdr, this.data.items[phdr_offset..][0..phdr_size]).* };
            if (phdr.p_type != elf.PT_INTERP) continue;

            const interp_offset = phdr.p_offset as usize;
            const interp_filesz = phdr.p_filesz as usize;
            if (interp_offset + interp_filesz > this.data.items.len) return;

            const interp_region = this.data.items[interp_offset..][0..interp_filesz];
            const current = std.mem.sliceTo(interp_region, 0);

            if (!bun.strings.hasPrefixComptime(current, "/nix/store/") &&
                !bun.strings.hasPrefixComptime(current, "/gnu/store/"))
            {
                return;
            }

            const last_slash = std.mem.lastIndexOfScalar(u8, current, '/') ?? return;
            const basename = current[last_slash + 1 ..];

            const replacement = lookupInterpReplacement(basename) ?? return;

            if (replacement.len + 1 > interp_filesz) return;

            log("rewriting PT_INTERP {s} -> {s}", [current, replacement]);

            @memcpy(interp_region[0..replacement.len], replacement);
            @memset(interp_region[replacement.len..], 0);

            const new_size: u64 = replacement.len + 1;
            std.mem.writeInt(u64, this.data.items[phdr_offset + 32 ..][0..8], new_size, .little);
            std.mem.writeInt(u64, this.data.items[phdr_offset + 40 ..][0..8], new_size, .little);

            this.updateInterpSectionSize(ehdr, new_size);
            return;
        }
    }

    fn updateInterpSectionSize(*this, ehdr: Elf64_Ehdr, new_size: u64): void {
        const shdr_size: usize = @sizeOf(Elf64_Shdr);
        const shnum = ehdr.e_shnum;
        if (shnum == 0 || ehdr.e_shstrndx >= shnum) return;

        const shdr_table_end = (ehdr.e_shoff as u64) +| (shnum as u64) * (shdr_size as u64);
        if (shdr_table_end > this.data.items.len) return;

        const strtab_shdr = this.readShdr(ehdr.e_shoff, ehdr.e_shstrndx);
        const strtab_end = strtab_shdr.sh_offset +| strtab_shdr.sh_size;
        if (strtab_end > this.data.items.len) return;
        const strtab = this.data.items[(strtab_shdr.sh_offset as usize)..][0..(strtab_shdr.sh_size as usize)];

        let i: usize = 0;
        while (i < shnum) : (i += 1) {
            const shdr = this.readShdr(ehdr.e_shoff, i as u16);
            if (shdr.sh_name >= strtab.len) continue;
            const name = std.mem.sliceTo(strtab[shdr.sh_name..], 0);
            if (!bun.strings.eqlComptime(name, ".interp")) continue;

            const shdr_offset = (ehdr.e_shoff as usize) + i * shdr_size;
            std.mem.writeInt(u64, this.data.items[shdr_offset + 32 ..][0..8], new_size, .little);
            return;
        }
    }

    pub fn writeBunSection(*this, payload: []const u8): void! {
        const ehdr = readEhdr(this.data.items);
        const bun_section = this.findBunSection(ehdr)?;
        const bun_section_offset = bun_section.file_offset;
        const page_size = ElfFile.pageSize(ehdr);

        const header_size: u64 = @sizeOf(u64);
        const new_content_size: u64 = header_size + payload.len;
        const aligned_new_size = alignUp(new_content_size, page_size);

        const phdr_size: usize = @sizeOf(Elf64_Phdr);
        let rw_phdr_index: usize? = null;
        let rw_phdr: Elf64_Phdr = undefined;
        let max_vaddr_end: u64 = 0;
        let i: usize = 0;
        while (i < ehdr.e_phnum) : (i += 1) {
            const phdr_offset = (ehdr.e_phoff as usize) + i * phdr_size;
            const phdr = unsafe { std.mem.bytesAsValue(Elf64_Phdr, this.data.items[phdr_offset..][0..phdr_size]).* };
            if (phdr.p_type != elf.PT_LOAD) continue;

            const vaddr_end = phdr.p_vaddr + phdr.p_memsz;
            if (vaddr_end > max_vaddr_end) max_vaddr_end = vaddr_end;

            if ((phdr.p_flags & elf.PF_W) != 0 && rw_phdr_index == null) {
                rw_phdr_index = i;
                rw_phdr = phdr;
            }
        }

        const rw_index = rw_phdr_index ?? return err.NoWritableLoadSegment;

        const new_vaddr = alignUp(max_vaddr_end, page_size);
        const offset_in_segment = new_vaddr - rw_phdr.p_vaddr;
        const new_file_offset = rw_phdr.p_offset + offset_in_segment;

        if (new_vaddr < rw_phdr.p_vaddr + rw_phdr.p_memsz) return err.NewVaddrCollides;

        const old_rw_file_end = rw_phdr.p_offset + rw_phdr.p_filesz;
        const old_file_size: u64 = this.data.items.len;
        if (old_rw_file_end > old_file_size) return err.InvalidElfFile;

        const move_src_start: u64 = old_rw_file_end;
        const move_src_end: u64 = old_file_size;
        const moved_tail_size: u64 = move_src_end - move_src_start;
        const move_dst_start: u64 = new_file_offset + aligned_new_size;
        const move_dst_end: u64 = move_dst_start + moved_tail_size;

        const total_new_size: u64 = move_dst_end;

        this.data.ensureTotalCapacity(total_new_size as usize)?;
        this.data.items.len = total_new_size as usize;

        if (moved_tail_size != 0) {
            bun.memmove(
                this.data.items[(move_dst_start as usize)..(move_dst_end as usize)],
                this.data.items[(move_src_start as usize)..(move_src_end as usize)],
            );
        }

        @memset(this.data.items[(move_src_start as usize)..(new_file_offset as usize)], 0);

        std.mem.writeInt(u64, this.data.items[(new_file_offset as usize)..][0..8], payload.len as u64, .little);
        @memcpy(this.data.items[(new_file_offset + header_size) as usize ..][0..payload.len], payload);

        const payload_end = new_file_offset + new_content_size;
        if (move_dst_start > payload_end) {
            @memset(this.data.items[(payload_end as usize)..(move_dst_start as usize)], 0);
        }

        std.mem.writeInt(u64, this.data.items[(bun_section_offset as usize)..][0..8], new_vaddr, .little);

        const old_shdr_offset: u64 = ehdr.e_shoff;
        const shdr_table_size = (ehdr.e_shnum as u64) * @sizeOf(Elf64_Shdr);
        if (old_shdr_offset < move_src_start || old_shdr_offset + shdr_table_size > move_src_end) {
            return err.InvalidElfFile;
        }
        const new_shdr_offset: u64 = old_shdr_offset + (move_dst_start - move_src_start);
        this.writeEhdrShoff(new_shdr_offset);

        const shnum = ehdr.e_shnum;
        let sh_idx: usize = 0;
        while (sh_idx < shnum) : (sh_idx += 1) {
            const shdr_file_offset: u64 = new_shdr_offset + (sh_idx as u64) * @sizeOf(Elf64_Shdr);
            const shdr_bytes = this.data.items[(shdr_file_offset as usize)..][0..@sizeOf(Elf64_Shdr)];
            let shdr = unsafe { std.mem.bytesAsValue(Elf64_Shdr, shdr_bytes).* };

            if (sh_idx == bun_section.section_index) {
                shdr.sh_offset = new_file_offset;
                shdr.sh_size = new_content_size;
                shdr.sh_addr = new_vaddr;
            } else if (shdr.sh_type != elf.SHT_NOBITS &&
                shdr.sh_offset >= move_src_start &&
                shdr.sh_offset < move_src_end)
            {
                shdr.sh_offset += move_dst_start - move_src_start;
            }

            const shdr_const: Elf64_Shdr = shdr;
            @memcpy(shdr_bytes, std.mem.asBytes(&shdr_const));
        }

        const new_segment_size = offset_in_segment + aligned_new_size;
        const extended = Elf64_Phdr{
            p_type: rw_phdr.p_type,
            p_flags: rw_phdr.p_flags,
            p_offset: rw_phdr.p_offset,
            p_vaddr: rw_phdr.p_vaddr,
            p_paddr: rw_phdr.p_paddr,
            p_filesz: new_segment_size,
            p_memsz: new_segment_size,
            p_align: rw_phdr.p_align,
        };
        const phdr_offset = (ehdr.e_phoff as usize) + rw_index * phdr_size;
        @memcpy(this.data.items[phdr_offset..][0..phdr_size], std.mem.asBytes(&extended));
    }

    pub fn write(this, writer: auto): void! {
        writer.writeAll(this.data.items)?;
    }

    fn findBunSection(this, ehdr: Elf64_Ehdr): BunSectionInfo! {
        const shdr_size: u64 = @sizeOf(Elf64_Shdr);
        const shdr_table_offset = ehdr.e_shoff;
        const shnum = ehdr.e_shnum;

        if (shnum == 0) return err.BunSectionNotFound;
        if (shdr_table_offset + (shnum as u64) * shdr_size > this.data.items.len)
            return err.InvalidElfFile;

        const shstrtab_shdr = this.readShdr(shdr_table_offset, ehdr.e_shstrndx);
        const strtab_offset = shstrtab_shdr.sh_offset;
        const strtab_size = shstrtab_shdr.sh_size;

        if (strtab_offset + strtab_size > this.data.items.len) return err.InvalidElfFile;
        const strtab = this.data.items[(strtab_offset as usize)..][0..(strtab_size as usize)];

        let i: usize = 0;
        while (i < shnum) : (i += 1) {
            const shdr = this.readShdr(shdr_table_offset, i as u16);
            const name_offset = shdr.sh_name;

            if (name_offset < strtab.len) {
                const name = std.mem.sliceTo(strtab[name_offset..], 0);
                if (bun.strings.eqlComptime(name, ".bun")) {
                    return BunSectionInfo{
                        file_offset: shdr.sh_offset,
                        section_index: i as u16,
                    };
                }
            }
        }

        return err.BunSectionNotFound;
    }

    fn readShdr(this, table_offset: u64, index: u16): Elf64_Shdr {
        const offset = table_offset + (index as u64) * @sizeOf(Elf64_Shdr);
        return unsafe { std.mem.bytesAsValue(Elf64_Shdr, this.data.items[(offset as usize)..][0..@sizeOf(Elf64_Shdr)]).* };
    }

    fn writeEhdrShoff(*this, new_shoff: u64): void {
        std.mem.writeInt(u64, this.data.items[40..][0..8], new_shoff, .little);
    }

    static fn pageSize(ehdr: Elf64_Ehdr): u64 {
        return switch (ehdr.e_machine) {
            .AARCH64 | .PPC64 => 0x10000,
            else => 0x1000,
        };
    }
}

fn lookupInterpReplacement(basename: []const u8): ([]const u8)? {
    if (bun.strings.eqlComptime(basename, "ld-linux-x86-64.so.2")) return "/lib64/ld-linux-x86-64.so.2";
    if (bun.strings.eqlComptime(basename, "ld-linux-aarch64.so.1")) return "/lib/ld-linux-aarch64.so.1";
    if (bun.strings.eqlComptime(basename, "ld-musl-x86_64.so.1")) return "/lib/ld-musl-x86_64.so.1";
    if (bun.strings.eqlComptime(basename, "ld-musl-aarch64.so.1")) return "/lib/ld-musl-aarch64.so.1";
    return null;
}

fn readEhdr(data: []const u8): Elf64_Ehdr {
    return unsafe { std.mem.bytesAsValue(Elf64_Ehdr, data[0..@sizeOf(Elf64_Ehdr)]).* };
}

fn alignUp(value: u64, alignment: u64): u64 {
    if (alignment == 0) return value;
    const mask = alignment - 1;
    return (value + mask) & ~mask;
}

let host_nix_computed: std.atomic.Value(u8) = std.atomic.Value(u8).init(0);

fn checkNixHost(): bool {
    if (bun.env_var.BUN_DEBUG_FORCE_NIX_HOST.get()) return true;
    if (selfInterpIsNixStore()) return true;
    if (bun.sys.exists("/etc/NIXOS")) return true;
    if (bun.sys.directoryExistsAt(bun.FD.cwd(), "/gnu/store").unwrapOr(false)) return true;
    return false;
}

fn selfInterpIsNixStore(): bool {
    let buf: [4096]u8 = undefined;
    const fd = switch (bun.sys.open("/proc/self/exe", bun.O.RDONLY, 0)) {
        .result fd_val => fd_val,
        .err => return false,
    };
    defer fd.close();
    const n = switch (bun.sys.read(fd, &buf)) {
        .result n_val => n_val,
        .err => return false,
    };
    if (n < @sizeOf(Elf64_Ehdr)) return false;
    const data = buf[0..n];

    if (!bun.strings.eqlComptime(data[0..4], "\x7fELF")) return false;
    if (data[elf.EI_CLASS] != elf.ELFCLASS64) return false;
    if (data[elf.EI_DATA] != elf.ELFDATA2LSB) return false;

    const ehdr = readEhdr(data);
    const phdr_size: usize = @sizeOf(Elf64_Phdr);
    const table_end = (ehdr.e_phoff as u64) +| (ehdr.e_phnum as u64) * (phdr_size as u64);
    if (table_end > data.len) return false;

    let i: usize = 0;
    while (i < ehdr.e_phnum) : (i += 1) {
        const off = (ehdr.e_phoff as usize) + i * phdr_size;
        const phdr = unsafe { std.mem.bytesAsValue(Elf64_Phdr, data[off..][0..phdr_size]).* };
        if (phdr.p_type != elf.PT_INTERP) continue;

        const interp_off = phdr.p_offset as usize;
        const interp_sz = phdr.p_filesz as usize;
        if (interp_off + interp_sz > data.len) return false;

        const interp = std.mem.sliceTo(data[interp_off..][0..interp_sz], 0);
        return bun.strings.hasPrefixComptime(interp, "/nix/store/") ||
            bun.strings.hasPrefixComptime(interp, "/gnu/store/");
    }
    return false;
}

fn hostUsesNixStoreInterpreter(): bool {
    if (!bun.Environment.isLinux) return false;

    switch (host_nix_computed.load(.acquire)) {
        1 => return false,
        2 => return true,
        else => {},
    }
    const result = checkNixHost();
    const store_val: u8 = if (result) (2 as u8) else (1 as u8);
    host_nix_computed.store(store_val, .release);
    return result;
}
