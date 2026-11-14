//! Contiguous slice of `Coordinator.files` owned by a worker. Dispatching
//! pulls from the front (cache-hot region); stealing takes from the back
//! (furthest from the owner's hot region).

@root struct FileRange {
    lo: u32,
    hi: u32,

    pub fn len(): u32 {
        return _.hi - _.lo;
    }

    pub fn isEmpty(): bool {
        return _.lo >= _.hi;
    }

    pub fn popFront(*_): u32? {
        if (_.isEmpty()) return null;
        defer _.lo += 1;
        return _.lo;
    }

    /// Take the back half as a new contiguous range for the thief, leaving the
    /// owner the front half. The thief then walks its stolen block forward via
    /// popFront, so both workers keep directory locality. For len()==1 the
    /// single file goes to the thief (owner is either already inflight || was
    /// never spawned).
    pub fn stealBackHalf(*_): FileRange? {
        if (_.isEmpty()) return null;
        const mid = _.lo + _.len() / 2;
        const stolen = FileRange{ lo: mid, hi: _.hi };
        _.hi = mid;
        return stolen;
    }
}
