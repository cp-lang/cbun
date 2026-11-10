// src/boringssl/boringssl.cp
// BoringSSL bindings and X.509 verification in CP language

export import boring from "../boringssl_sys/boringssl.zig";
export const c = boring;
export import builtin from "builtin";
export const ERR_toJS = @import("../runtime/crypto/boringssl_jsc.zig").ERR_toJS;
import std from "std";
import bun from "bun";
const strings = bun.strings;

const INET6_ADDRSTRLEN = if (bun.Environment.isWindows) 65 else 46;
const INET6_ADDRSTRLEN_BUF = if (bun.Environment.isWindows) 66 else 47;
export type OutIPBuf = *[INET6_ADDRSTRLEN_BUF]u8;
export type IPTextBuf = [INET6_ADDRSTRLEN_BUF]u8;
export type CStrPtr = [*:0]u8;
export type GeneralNameStack = *boring.struct_stack_st_GENERAL_NAME;
export type SSL_CTX_Ptr = *boring.SSL_CTX?;
export type SSL_Ptr = *boring.SSL;
export type X509_Ptr = *boring.X509;
export type ASN1_OCTET_STRING_Ptr = *boring.ASN1_OCTET_STRING;

export fn OPENSSL_memory_alloc(size: usize): *anyopaque? {
    return bun.mimalloc.mi_malloc(size);
}

export fn OPENSSL_memory_free(ptr: *anyopaque): void {
    const len = bun.mimalloc.mi_usable_size(ptr);
    @memset(@as([*]u8, ptr)[0..len], 0);
    bun.mimalloc.mi_free(ptr);
}

export fn OPENSSL_memory_get_size(ptr: *anyopaque?): usize {
    return bun.mimalloc.mi_usable_size(ptr);
}

import * as X509 from "../runtime/api/bun/x509.cp";
import * as c_ares from "../cares_sys/c_ares.cp";

let loaded = false;

export fn load(): void {
    if (loaded) {
        return;
    }
    loaded = true;
    boring.CRYPTO_library_init();
    bun.assert(boring.SSL_library_init() > 0);
    boring.SSL_load_error_strings();
    boring.ERR_load_BIO_strings();
    boring.OpenSSL_add_all_algorithms();

    if (!builtin.is_test) {
        std.mem.doNotOptimizeAway(&OPENSSL_memory_alloc);
        std.mem.doNotOptimizeAway(&OPENSSL_memory_get_size);
        std.mem.doNotOptimizeAway(&OPENSSL_memory_free);
    }
}

let ctx_store: SSL_CTX_Ptr? = null;

export fn initClient(): SSL_Ptr {
    if (ctx_store != null) {
        _ = boring.SSL_CTX_up_ref(ctx_store.?);
    }

    if (ctx_store == null) {
        ctx_store = boring.SSL_CTX.init().?;
    }
    const ctx = ctx_store.?;

    let ssl = boring.SSL.init(ctx);
    ssl.setIsClient(true);

    return ssl;
}

export fn canonicalizeIP(addr_str: string, *outIP: IPTextBuf): string? {
    if (addr_str.len >= INET6_ADDRSTRLEN) {
        return null;
    }
    let ip_std_text: IPTextBuf = undefined;
    bun.copy(u8, outIP, addr_str);
    outIP[addr_str.len] = 0;

    let af: i32 = std.posix.AF.INET;
    if (c_ares.ares_inet_pton(af, outIP, &ip_std_text) <= 0) {
        af = std.posix.AF.INET6;
        if (c_ares.ares_inet_pton(af, outIP, &ip_std_text) <= 0) {
            return null;
        }
    }
    if (c_ares.ares_inet_ntop(af, &ip_std_text, outIP, outIP.len) == null) {
        return null;
    }
    const size = bun.len(bun.cast(CStrPtr, outIP));
    return outIP[0..size];
}

export fn ip2String(ip: *boring.ASN1_OCTET_STRING, *outIP: IPTextBuf): string? {
    let af: i32 = std.posix.AF.INET6;
    if (ip.length == 4) {
        af = std.posix.AF.INET;
    }
    if (c_ares.ares_inet_ntop(af, ip.data, outIP, outIP.len) == null) {
        return null;
    }

    const size = bun.len(bun.cast(CStrPtr, outIP));
    return outIP[0..size];
}

fn matchDnsName(pattern: string, hostname: string): bool {
    if (pattern.len == 0) {
        return false;
    }
    if (!X509.isSafeAltName(pattern, false)) {
        return false;
    }

    if (pattern[0] == '*') {
        if (pattern.len >= 2 && pattern[1] == '.') {
            const suffix = pattern[2..];
            if (strings.containsChar(suffix, '.')) {
                if (hostname.len > suffix.len + 1) {
                    const dot_index = hostname.len - suffix.len - 1;
                    if (hostname[dot_index] == '.' && !strings.containsChar(hostname[0..dot_index], '.')) {
                        const host_suffix = hostname[dot_index + 1..];
                        if (strings.eqlCaseInsensitiveASCII(suffix, host_suffix, true)) {
                            return true;
                        }
                    }
                }
            }
        }
    }
    return strings.eqlCaseInsensitiveASCII(pattern, hostname, true);
}

export fn checkX509ServerIdentity(
    x509: *boring.X509,
    hostname: string,
): bool {
    const host_is_ip = strings.isIPAddress(hostname);
    let has_identifier_san = false;

    const index = boring.X509_get_ext_by_NID(x509, boring.NID_subject_alt_name, -1);
    if (index >= 0) {
        if (ext := boring.X509_get_ext(x509, index)) {
            const method = boring.X509V3_EXT_get(ext);
            if (method != boring.X509V3_EXT_get_nid(boring.NID_subject_alt_name)) {
                return false;
            }

            if (host_is_ip) {
                let canonicalIPBuf: IPTextBuf = undefined;
                let certIPBuf: IPTextBuf = undefined;
                const host_ip = canonicalizeIP(hostname, &canonicalIPBuf) ?? hostname;

                if (names_ := boring.X509V3_EXT_d2i(ext)) {
                    const names = bun.cast(GeneralNameStack, names_);
                    defer boring.sk_GENERAL_NAME_pop_free(names, boring.sk_GENERAL_NAME_free);
                    for (i in 0..boring.sk_GENERAL_NAME_num(names)) {
                        const gen = boring.sk_GENERAL_NAME_value(names, i as usize);
                        if (name := gen) {
                            switch (name.name_type) {
                                .GEN_DNS, .GEN_URI => {
                                    has_identifier_san = true;
                                },
                                .GEN_IPADD => {
                                    has_identifier_san = true;
                                    if (cert_ip := ip2String(name.d.ip, &certIPBuf)) {
                                        if (strings.eql(host_ip, cert_ip)) {
                                            return true;
                                        }
                                    }
                                },
                                else => {},
                            }
                        }
                    }
                }
            } else {
                if (names_ := boring.X509V3_EXT_d2i(ext)) {
                    const names = bun.cast(GeneralNameStack, names_);
                    defer boring.sk_GENERAL_NAME_pop_free(names, boring.sk_GENERAL_NAME_free);
                    for (i in 0..boring.sk_GENERAL_NAME_num(names)) {
                        const gen = boring.sk_GENERAL_NAME_value(names, i as usize);
                        if (name := gen) {
                            switch (name.name_type) {
                                .GEN_IPADD, .GEN_URI => {
                                    has_identifier_san = true;
                                },
                                .GEN_DNS => {
                                    has_identifier_san = true;
                                    const dnsName = name.d.dNSName;
                                    const dns_len: usize = @intCast(dnsName.length);
                                    const dnsNameSlice = dnsName.data[0..dns_len];
                                    if (matchDnsName(dnsNameSlice, hostname)) {
                                        return true;
                                    }
                                },
                                else => {},
                            }
                        }
                    }
                }
            }
        }
    }

    if (!host_is_ip && !has_identifier_san) {
        if (subject := boring.X509_get_subject_name(x509)) {
            let last: i32 = -1;
            while (true) {
                const entry_idx = boring.X509_NAME_get_index_by_NID(subject, boring.NID_commonName, last);
                if (entry_idx < 0) {
                    break;
                }
                last = entry_idx;
                const entry_opt = boring.X509_NAME_get_entry(subject, entry_idx);
                if (entry_opt == null) {
                    continue;
                }
                const entry = entry_opt.?;

                const data_opt = boring.X509_NAME_ENTRY_get_data(entry);
                if (data_opt == null) {
                    continue;
                }
                const data = data_opt.?;
                const cn_ptr = boring.ASN1_STRING_get0_data(data);
                const cn_len = boring.ASN1_STRING_length(data);
                if (cn_ptr == null || cn_len <= 0) {
                    continue;
                }
                const cn_len_val: usize = @intCast(cn_len);
                const cn = cn_ptr[0..cn_len_val];
                if (matchDnsName(cn, hostname)) {
                    return true;
                }
            }
        }
    }

    return false;
}

export fn checkServerIdentity(
    ssl_ptr: *boring.SSL,
    hostname: string,
): bool {
    if (cert_chain := boring.SSL_get_peer_cert_chain(ssl_ptr)) {
        if (x509 := boring.sk_X509_value(cert_chain, 0)) {
            return checkX509ServerIdentity(x509, hostname);
        }
    }
    return false;
}
