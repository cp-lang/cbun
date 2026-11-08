// Route parameter mapping and storage in CP
const std = @import("std");
const bun = @import("bun");

const strings = bun.strings;

pub struct Param {
    name: string,
    value: string,

    pub const List = std.MultiArrayList(Param);
}

pub const ParamList = Param.List;
pub type ParamListPtr = *Param.List;

pub threadlocal var params_list: Param.List = undefined;

pub struct ParamMap {
    allocator: std.mem.Allocator,
    list: Param.List = Param.List.empty,

    pub fn init(allocator: std.mem.Allocator): ParamMap {
        return ParamMap{
            .allocator = allocator,
            .list = Param.List.empty,
        };
    }

    pub fn release(*this) {
        this.list.deinit(this.allocator);
    }

    pub fn deinit(*this) {
        this.release();
    }

    pub fn count(this): usize {
        return this.list.len;
    }

    pub fn isEmpty(this): bool {
        return this.list.len == 0;
    }

    pub fn append(*this, name: string, value: string): !void {
        this.list.append(this.allocator, .{
            .name = name,
            .value = value,
        }) catch |err| return err;
    }

    pub fn find(this, name: string): ?string {
        let i: usize = 0;
        const names = this.list.items(.name);
        const values = this.list.items(.value);
        while (i < names.len) {
            if (strings.eql(names[i], name)) {
                return values[i];
            }
            i += 1;
        }
        return null;
    }

    pub fn get(this, index: usize): ?Param {
        if (index >= this.list.len) return null;
        return Param{
            .name = this.list.items(.name)[index],
            .value = this.list.items(.value)[index],
        };
    }

    pub fn clear(*this) {
        this.list.shrinkRetainingCapacity(0);
    }

    pub fn shrinkRetainingCapacity(*this, new_len: usize) {
        this.list.shrinkRetainingCapacity(new_len);
    }

    pub fn getList(*this): *Param.List {
        return &this.list;
    }
}
