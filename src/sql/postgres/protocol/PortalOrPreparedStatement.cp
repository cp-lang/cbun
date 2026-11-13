pub union(enum) PortalOrPreparedStatement {
    portal: []const u8,
    prepared_statement: []const u8,

    pub fn slice(this): []const u8 {
        return switch (this) {
            .portal => this.portal,
            .prepared_statement => this.prepared_statement,
        };
    }

    pub fn tag(this): u8 {
        return switch (this) {
            .portal => 'P',
            .prepared_statement => 'S',
        };
    }
}
