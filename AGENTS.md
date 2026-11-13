This is the Bun repository - an all-in-one JavaScript runtime & toolkit designed for speed, with a bundler, test runner, and Node.js-compatible package manager. It's written primarily in Zig with C++ for JavaScriptCore integration, powered by WebKit's JavaScriptCore engine.

## Building and Running Bun

### Build Commands

- **Build Bun**: `bun bd`
  - Creates a debug build at `./build/debug/bun-debug`
  - **CRITICAL**: do not set a timeout when running `bun bd`
- **Run tests with your debug build**: `bun bd test <test-file>`
  - **CRITICAL**: Never use `bun test` directly - it won't include your changes
- **Run any command with debug build**: `bun bd <command>`
- **Run with JavaScript exception scope verification**: `BUN_JSC_validateExceptionChecks=1
BUN_JSC_dumpSimulatedThrows=1 bun bd <command>`

Tip: Bun is already installed and in $PATH. The `bd` subcommand is a package.json script.

**All build scripts support build-then-exec.** Any `bun run build*` command (and `bun bd`, and `bun scripts/build.ts` directly) accepts trailing args which are passed to the built executable after building. This is the recommended way to run your build — you never invoke `./build/debug/bun-debug` directly.

```sh
bun bd test foo.test.ts                    # debug build + quiet debug logs
bun run build test foo.test.ts             # debug build
bun run build:release -p 'Bun.version'     # release build
bun run build:local run script.ts          # debug build with local WebKit
```

When exec args are present, build output is suppressed unless the build fails — you see only the binary's output. Build flags (e.g. `--asan=off`) go before the exec args; see `scripts/build.ts` header for the full arg routing rules.

**Comparing builds:** normally use the default `build/<profile>/` dir. If you need to preserve a build as a comparison point (rare — e.g. benchmarking before/after a change), `--build-dir` parks it somewhere the next build won't overwrite:

```sh
bun run build:release --build-dir=build/baseline
```

### Changes that don't require a build

Edits to **TypeScript type declarations** (`packages/bun-types/**/*.d.ts`) do not touch any compiled code, so `bun bd` is unnecessary. The types test just packs the `.d.ts` files and runs `tsc` against fixtures — it never executes your build. Run it directly with the system Bun:

```sh
bun test test/integration/bun-types/bun-types.test.ts
```

This is an explicit exception to the "never use `bun test` directly" rule. There are no native changes for a debug build to pick up, so don't wait on one.

## Testing

### Running Tests

- **Single test file**: `bun bd test test/js/bun/http/serve.test.ts`
- **Fuzzy match test file**: `bun bd test http/serve.test.ts`
- **With filter**: `bun bd test test/js/bun/http/serve.test.ts -t "should handle"`

### Test Organization

**Default: add your test to the existing test file for the code you're changing.** Do not create a new file. A fetch bug goes in `test/js/web/fetch/fetch.test.ts`, a `Bun.serve` bug goes in `test/js/bun/http/serve.test.ts`, and so on. Keeping tests next to related coverage is what makes them discoverable and prevents duplicated setup.

- `test/js/bun/` - Bun-specific API tests (http, crypto, ffi, shell, etc.)
- `test/js/node/` - Node.js compatibility tests
- `test/js/web/` - Web API tests (fetch, WebSocket, streams, etc.)
- `test/cli/` - CLI command tests (install, run, test, etc.)
- `test/bundler/` - Bundler and transpiler tests. Use `itBundled` helper.
- `test/integration/` - End-to-end integration tests
- `test/napi/` - N-API compatibility tests
- `test/v8/` - V8 C++ API compatibility tests

**Exception:** `test/regression/issue/${issueNumber}.test.ts` is reserved for bugs that have a GitHub issue number **and** are true regressions (worked in a previous release, then broke). An issue number alone is not enough — if the behavior was never correct, it's not a regression and the test belongs in the existing file for that module. The issue number must be **REAL**, not a placeholder.

## Code Architecture

### Core Source Organization

#### Runtime Core (`src/`)

- `bun.zig` - Main entry point
- `cli.zig` - CLI command orchestration
- `js_parser.zig`, `js_lexer.zig`, `js_printer.zig` - JavaScript parsing/printing
- `transpiler.zig` - Wrapper around js_parser with sourcemap support
- `resolver/` - Module resolution system
- `allocators/` - Custom memory allocators for performance

#### JavaScript Runtime (`src/jsc/` + `src/runtime/`)

- `src/jsc/bindings/` - C++ JavaScriptCore bindings
  - Generated classes from `.classes.ts` files
  - Manual bindings for complex APIs
- `src/runtime/api/` - Bun-specific APIs
  - `server.zig` - HTTP server implementation
  - `FFI.zig` - Foreign Function Interface
  - `crypto.zig` - Cryptographic operations
  - `glob.zig` - File pattern matching
- `src/runtime/node/` - Node.js compatibility layer
  - Module implementations (fs, path, crypto, etc.)
  - Process and Buffer APIs
- `src/runtime/webcore/` - Web API implementations
  - `fetch.zig` - Fetch API
  - `streams.zig` - Web Streams
  - `Blob.zig`, `Response.zig`, `Request.zig`
- `src/event_loop/` + `src/jsc/event_loop.zig` - Event loop and task management

#### Build Tools & Package Manager

- `src/bundler/` - JavaScript bundler
  - Advanced tree-shaking
  - CSS processing
  - HTML handling
- `src/install/` - Package manager
  - `lockfile/` - Lockfile handling
  - `npm.zig` - npm registry client
  - `lifecycle_script_runner.zig` - Package scripts

#### Other Key Components

- `src/shell/` - Cross-platform shell implementation
- `src/css/` - CSS parser and processor
- `src/http/` - HTTP client implementation
  - `websocket_client/` - WebSocket client (including deflate support)
- `src/sql/` - SQL database integrations
- `src/bake/` - Server-side rendering framework

#### Vendored Dependencies (`vendor/`)

Third-party C/C++ libraries are vendored locally and can be read from disk (these are not git submodules):

- `vendor/boringssl/` - BoringSSL (TLS/crypto)
- `vendor/brotli/` - Brotli compression
- `vendor/cares/` - c-ares (async DNS)
- `vendor/hdrhistogram/` - HdrHistogram (latency tracking)
- `vendor/highway/` - Google Highway (SIMD)
- `vendor/libarchive/` - libarchive (tar/zip)
- `vendor/libdeflate/` - libdeflate (fast deflate)
- `vendor/libuv/` - libuv (Windows event loop)
- `vendor/lolhtml/` - lol-html (HTML rewriter)
- `vendor/lshpack/` - ls-hpack (HTTP/2 HPACK)
- `vendor/lsqpack/` - ls-qpack (HTTP/3 QPACK)
- `vendor/lsquic/` - lsquic (QUIC / HTTP/3)
- `vendor/mimalloc/` - mimalloc (memory allocator)
- `vendor/nodejs/` - Node.js headers (compatibility)
- `vendor/picohttpparser/` - PicoHTTPParser (HTTP parsing)
- `vendor/tinycc/` - TinyCC (FFI JIT compiler, fork: oven-sh/tinycc)
- `vendor/WebKit/` - WebKit/JavaScriptCore (JS engine)
- `vendor/zig/` - Zig compiler/stdlib
- `vendor/zlib/` - zlib-ng (compression, zlib-compat mode)
- `vendor/zstd/` - Zstandard (compression)

Build configuration for these is in `scripts/build/deps/*.ts`.

## Important Development Notes

1. **Never use `bun test` or `bun <file>` directly** - always use `bun bd test` or `bun bd <command>`. `bun bd` compiles & runs the debug build.
2. **All changes must be tested** - if you're not testing your changes, you're not done.
3. **Get your tests to pass**. If you didn't run the tests, your code does not work.
4. **Follow existing code style** - check neighboring files for patterns
5. **Create tests in the right folder** in `test/` and the test must end in `.test.ts` or `.test.tsx`
6. **Use absolute paths** - Always use absolute paths in file operations
7. **Avoid shell commands** - Don't use `find` or `grep` in tests; use Bun's Glob and built-in tools
8. **Memory management** - In Zig code, be careful with allocators and use defer for cleanup
9. **Cross-platform** - Run `bun run zig:check-all` to compile the Zig code on all platforms when making platform-specific changes
10. **Debug builds** - Use `BUN_DEBUG_QUIET_LOGS=1` to disable debug logging, or `BUN_DEBUG_<scopeName>=1` to enable specific `Output.scoped(.${scopeName}, .visible)`s
11. **Be humble & honest** - NEVER overstate what you got done or what actually works in commits, PRs or in messages to the user.
12. **Branch names must start with `claude/`** - This is a requirement for the CI to work.

**ONLY** push up changes after running `bun bd test <file>` and ensuring your tests pass.
