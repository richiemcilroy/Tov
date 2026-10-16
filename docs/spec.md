# Tov language spec (draft 0)

Tov is TypeScript-shaped. **If code looks like TypeScript it behaves like TypeScript, or it is a compile error with a code and a fix.** This spec lists only what differs from, or is removed from, TypeScript; everything else means what it means in TS.

Status tags: **[M0]** checked today · **[Mn]** planned for milestone n.

## 1. Files, modules, entry point
- Source files end in `.tov` (`.ts` works too). One file = one module.
- `import { a, b } from "./other"` (relative, no extension) and `import * as fs from "std/fs"`. No default imports/exports, no `import x as y` renaming, no re-exports (`export { x } from`), no side-effect imports. **[M0]**
- `export` goes on declarations: `export function`, `export type`, `export interface`, `export const`, `export class` [M2].
- A constant can be used by a closure in its own initializer when its type is known without the closure (it's annotated, or it's a call whose signature fixes its type): `const server = Bun.serve({ websocket: { message(ws, m) { server.publish("room", m) } } })`.
- **Entry point.** The program's entry file is either a **script** — top-level statements run in source order, with its module `const`/`let` initialized where they're declared — or has `function main()` (optionally `main(): int` for an exit code). Imported modules hold only declarations, `const`/`let` bindings and `test(...)` calls: importing a module has no side effects.
- **Module state.** Module-level `let` can be reassigned in its module; a module `const` can't be rebound, but its contents can change (`todos.push(t)`), as in TS. Imported bindings are read-only.

## 2. Lexical
- Comments `//`, `/* */`. Identifiers as in TS (ASCII + `_` + `$`).
- Integer literals: `42`, `1_000`, `0xff`, `0b1010`, `0o17`. Float literals contain `.` or an exponent: `1.0`, `.5`, `2e10`.
- Strings: `"..."` or `'...'` with JS escapes. Template literals `` `a ${expr} b` ``.
- Semicolons are optional. A line break ends a statement unless the next line starts with `.`, `?.`, or a binary operator, or the break is inside `()`/`[]`. A line starting with `(` or `[` never continues the previous line.

## 3. Types **[M0]**
| Type | Meaning |
|---|---|
| `int` | 64-bit signed integer (overflow traps) |
| `f64` (alias `number`), `f32` | IEEE floats |
| `i8 i16 i32 u8 u16 u32 u64` | sized integers |
| `bool` (alias `boolean`), `string` | UTF-8 immutable string |
| `bigint` | arbitrary-precision integer, as in TS (`123n`) |
| `decimal` | exact base-10 number, 34 significant digits (Tov's own: `19.99m`, or `19.99` where a `decimal` is expected) |
| `undefined`, `void`, `never`, `unknown` | as TS |
| `T[]` / `Array<T>` | growable array (value) |
| `Map<K, V>`, `Set<T>` | hash map/set (value) |
| `{ a: T, b?: U }` | exact record (value) |
| `A \| B` | union; `"lit"` string-literal types |
| `(a: T) => U` | function type (`(a: T) => U throws E` if it can throw, §7) |
| `Record<string, V>` | string-keyed map built with an object literal: `{ "Content-Type": "text/plain" }`; read with `.get(k)` (it is a `Map<string, V>`) |
| `Promise<T>`, `Promise<T, E>` | a promise of a `T` that never rejects, or can reject with `E` (an `Error` class); see §7b |

- Removed: `any`, `null` (use `undefined`), `object`, `symbol`, tuples [later], intersection `&`, conditional/mapped/indexed-access types, `keyof`, `typeof` in types, enums (use literal unions), namespaces, decorators.
- `number` is exactly `f64`; `boolean` is exactly `bool`.

### Numbers
- An integer literal is `int` unless context expects a float type. An unannotated `let x = 0` becomes `f64` if any later use in the function requires it (`x += 0.5`, `return x` from an `f64` function).
- `int` widens implicitly to `f64` (in arithmetic and assignment). `f64` → `int` is never implicit: use `Math.trunc/floor/ceil/round(x)`, which return `int` (trap on NaN/overflow).
- Float arithmetic is IEEE 754 with every operation rounded separately, exactly as in JavaScript: `a * b + c` is never fused into one multiply-add.
- `/` always produces `f64` (as in TS). `%`, `+ - *` on two ints produce `int`. Sized integers don't mix implicitly; convert with `u8(x)`, `i32(x)` etc. (trap if out of range).

### bigint
- `bigint` is JavaScript's BigInt: `123n`, `0xffn`, `+ - * / % **` (`/` truncates), `& | ^ ~ << >>` (two's complement), `===`, `<` and friends, `++`/`--`, `typeof x === "bigint"`. `BigInt(x)` converts an integer (exactly), a string (`try`: `SyntaxError` unless it's one) or a number (`try`: `RangeError` unless it's an integer); `Number(x)` is the nearest number; `int(x)`, `u64(x)`, `i32(x)`... trap if it doesn't fit. `x.toString(radix?)`, `BigInt.asIntN(bits, x)`, `BigInt.asUintN(bits, x)`.
- A `bigint` never mixes with numbers: `x + 1` is an error with the fix `x + 1n` (JavaScript throws a TypeError); comparing with `<` against an integer works, as in JS. `>>>` is an error (a bigint has no width). Dividing by `0n` and a negative `**` exponent trap. `JSON.stringify` rejects bigints, as JavaScript does.
- Values below 2^62 in magnitude live in one word (no allocation); larger ones are reference-counted, and `x += y`, `x *= y`, `x = (x + a * b) * c` update `x` in place when nothing else holds it.

### decimal
- `decimal` holds money and other base-10 quantities exactly: `0.1m + 0.2m === 0.3m`. A literal is a decimal with an `m` suffix, or wherever a `decimal` is expected (`const price: decimal = 19.99`, `total * 0.0825`); an `int` converts to one implicitly (exactly), a float never does (`decimal(x)` converts a number through its shortest form, so `decimal(0.1)` is `0.1`; NaN and the infinities trap). `decimal("12.50")` parses (`try`: `SyntaxError`, or `RangeError` past 34 digits before the point); `Number(d)` is the nearest number, `int(d)` traps unless it's a whole number that fits.
- `+ - *` are exact and keep every digit: the scale of a sum is the larger of the two (`1.10 + 2.205` is `3.305`), of a product their sum (`1.10 * 3` is `3.30`). Past 34 digits a result rounds half to even; 34 digits before the point traps. `/` gives 34 significant digits, half to even (`1m / 3m` is `0.3333333333333333333333333333333333`); an exact quotient keeps the dividend's scale less the divisor's (`10.00m / 4` is `2.50`). `%` is exact. `===` and `<` compare values (`1.10m === 1.1m`); `toString()` keeps the scale (`"1.10"`).
- `d.round(places?, mode?)` (modes as the TC39 Decimal proposal names them: `"halfEven"`, the default, `"halfExpand"`, `"halfTrunc"`, `"halfCeil"`, `"halfFloor"`, `"ceil"`, `"floor"`, `"trunc"`, `"expand"`), `floor()`, `ceil()`, `trunc()`, `abs()`, `toFixed(digits)` (half away from zero, as Number's). `JSON.stringify` writes a decimal as a number with its digits (`3.10`), and `JSON.parse` reads a number into a decimal exactly.
- No `**`, bitwise operators or `Math` functions: decimals are for exact arithmetic.

### Records
- `type P = { x: f64, y: f64 }` declares an exact record. Two record types with the same fields are the same type.
- An object literal must provide every required field and no others. Optional fields (`b?: U`) may be omitted and have type `U | undefined`.
- Records, arrays, maps, sets, and strings are **values**: assignment copies (cheaply, copy-on-write). See §6.

### Unions and narrowing
- A **discriminated union** is a union of records sharing a string-literal field (e.g. `kind`). It compiles to a tagged union.
- Narrowing works on: `switch (x.kind)`, `if (x.kind === "a")`, `x === undefined` / `x !== undefined`, `typeof x === "string"` (for unions of primitives), and early exits (`if (x === undefined) return` narrows the rest of the block).
- Field paths of locals narrow too: `if (t.left !== undefined) use(t.left)`. So does a module `const` (`const port = process.env.PORT; if (port !== undefined) ...`), though not its fields (they can change elsewhere). `Array.isArray(x)` narrows a union to its array members. Assigning to the path (or any prefix of it), or passing it as `inout`, forgets the narrowing. Inside a closure, narrowings of the enclosing function's `let` variables don't apply.
- A `switch` over a union discriminant or literal union must be exhaustive or have `default`.

### Interfaces **[M2]**
- `interface` is structural and implicit: any record or class with the members (and matching types) satisfies it. As a generic bound it is statically dispatched; as a value type (`function f(x: Named)`, `Shape[]`) it holds any satisfying value. A record behind an interface is a copy (records stay values); a class instance is shared.
- Methods in interfaces (`area(): f64`) are function-typed members: a class satisfies them with methods, a record with fields holding functions.

### Generics
- `function f<T>(x: T): T`, `function g<T extends I>(...)`, `type Box<T> = { value: T }`, defaults (`<T = undefined>`: the type when nothing fixes it, and what `Box` means without arguments), generic classes, and generic methods (`static of<T>(x: T)`, `json<T>(): Promise<T, SyntaxError>`; a generic method can't be overridden yet). Type arguments are inferred from arguments and from the expected type (`const t: Todo = try await req.json()`); closures are checked after everything else, so `connect({ data: { n: 1 }, open: (s) => s.data.n })` infers `T` from `data` and then types `s`. Bodies are checked once, at definition: a `T` can only be used through its bound.

## 4. Declarations and statements **[M0]**
- `function`, `const`, `let`, `type`, `interface`, arrow functions `(x: T) => expr` / `=> { ... }`, and methods in object literals (`{ fetch(req) { ... } }`, a field holding an arrow function).
- Exported functions must annotate every parameter and the return type (unannotated return means `void`). Local and unexported functions may omit the return type when it can be inferred.
- `if/else`, `while`, `do/while`, `for (init; cond; step)`, `for (const x of xs)`, `switch`, `break`, `continue`, `return`, blocks.
- Conditions must be `bool`, or an optional of a non-primitive type (tests "is defined"). `if (n)` on a number or `if (s)` on a string is an error with the explicit rewrite (`n !== 0`, `s !== ""`).
- A function with a non-`void` return type must return on every path.
- Removed: `var`, `with`, labels, `for…in` (use `for (const k of map.keys())`), `delete`, `arguments`, `eval`, comma operator.

## 5. Expressions **[M0]**
- Operators and precedence as in TS. Equality is `===` / `!==`; `==` / `!=` are errors (fix: use `===`). Equality on records/arrays compares by value.
- `+` concatenates when either side is a `string` (numbers/bools format as in JS).
- `a?.b`, `a ?? b`, `c ? a : b`, `x as T` (checked cast from `unknown` or a union; traps on failure), `x!` (non-undefined assertion; traps).
- Indexing `xs[i]` requires an integer `i` and has type `T | undefined`; `xs[i]!` asserts presence (traps when out of bounds). Index assignment `xs[i] = v` traps when out of bounds.
- Strings have no `.length` (ambiguous under UTF-8). Use `s.byteLength` or `s.chars().length`.
- `console.log(...args)` prints (Node's formatting); `Math.*` as in JS (with the int-returning rounders above).
- `map.keys()`, `map.values()` and `set.values()` return arrays (not iterators).
- `parseInt(s)`, `parseFloat(s)` and `Number(s)` return `T | undefined` instead of `NaN`, so failed parses must be handled.

## 6. Values, mutation, `inout` **[M0]**
- `let` bindings can be reassigned; `const` bindings cannot. Both can be mutated in place (`xs.push(1)`, `p.x = 2`) — same as TS.
- Parameters are read-only views of the caller's value. Mutating one is an error. To mutate the caller's value declare `inout` and pass a `let` variable (or a field/element of one) with `&` — the callee may replace the whole value:
  ```ts
  function addOne(inout xs: int[]) { xs.push(1) }
  addOne(&items)
  ```
- Copy-then-mutate where the original is read again (`const b = a; b.push(1); use(a)`, or `const b = a; addOne(&a); use(b)`) is an error [M1], because TS would share and Tov copies.
- `class` instances are shared references (reference-counted): assignment shares the object, as in TS, and changes through one reference are seen through all. Their fields can be changed through any reference (`p.x = 1` works on a `const p` or a parameter). See §6a.
- A value is freed as soon as nothing can use it again: a local's value is released after the last statement of its block that uses it (directly or through a local that may borrow from it), not at the end of the block.

## 6a. Classes **[M2]**
```ts
abstract class Shape {
  constructor(readonly name: string) {}     // parameter property: a readonly field
  abstract area(): f64
  describe(): string { return `${this.name}: ${this.area()}` }
}
class Circle extends Shape {
  private hits = 0
  static readonly UNIT = 1
  constructor(private r: f64) { super("circle") }
  area(): f64 { return Math.PI * this.r * this.r }
  get diameter(): f64 { return 2 * this.r }
}
```
- Single inheritance (`extends`, `super(...)`, `super.method(...)`), `abstract` classes and methods, `implements` (checked structurally), `static` methods and `static readonly` fields (no mutable statics), getters (setters later), `public`/`protected`/`private` and `#private` members, `readonly` fields (set only by their initializer or the constructor), parameter properties. No generic methods yet (make the class generic, or use a top-level generic function).
- Every field needs a value: an initializer, an assignment at the top level of the constructor, or `?` (optional). In a constructor, `this` is only used to set fields until all are set. A derived class's constructor starts with `super(...)`.
- An override takes the same parameters (or wider types) and returns the same type (or a narrower one). Methods nobody overrides are called directly.
- `x instanceof C` narrows unions and base-class types; `x as C` is a checked downcast (traps if wrong). `===` on instances is identity. `console.log` prints `ClassName { field: value }`, and `${x}` uses a `toString(): string` method if the class has one.
- `C | undefined` fields and variables are nullable pointers (no tag word).
- **Memory.** Instances are reference-counted and freed as soon as the last reference goes. Reference counting can't free *cycles*, so the checker rejects class graphs where instances could form a cycle through strong fields, and offers three fixes: `weak parent: Node | undefined` (a back-reference that doesn't keep its target alive and reads as `undefined` once the target is freed); `readonly` references set in the constructor (objects then only point to older objects, so no cycle can form); or `cyclic class Node`, which also enrolls its instances (and those of classes on the same cycles, and subclasses) in a cycle collector. Only cyclic classes pay for it. Closures stored in fields that capture `this` are not traced: avoid storing such closures on the object they capture.

## 7. Errors **[M3]**
```ts
class NotFound extends Error {
  constructor(readonly key: string) { super(`no such key: ${key}`); this.name = "NotFound" }
}
function lookup(m: Map<string, int>, k: string): int throws NotFound {
  const v = m.get(k)
  if (v === undefined) throw new NotFound(k)
  return v
}
function total(m: Map<string, int>): int {   // throws NotFound (inferred)
  return try lookup(m, "a") + try lookup(m, "b")
}
try {
  console.log(total(m))
} catch (e) {                                  // e: NotFound
  console.log(e.message)
} finally {
  cleanup()
}
```
- Only `Error` objects (the built-in `Error`, `TypeError`, `RangeError`, `SyntaxError`, or classes extending them) are thrown. `e.message`, `e.name`, and `String(e)` / `${e}` (`"Name: message"`) work as in JavaScript.
- A call that can throw is either inside a `try { }` block, or marked `try f(x)`: the error then passes on to the caller. Unmarked calls are errors with the fix. `throws E` declares what a function can throw; unexported functions and methods infer it, exported ones must declare it.
- `catch (e)` gives `e` the most specific class covering everything the block can throw (else `Error`); narrow with `instanceof`. `finally` runs on every path out of the block (no `return`/`break` inside a `try` that has `finally` yet).
- A closure can pass errors on only when its function type says so: `type Handler = (req: Request) => Response throws Error`. An arrow checked against that type may `throw` and use `try f()`; calls through a value of that type are calls that can throw (a function used as a value keeps its `throws`). Otherwise catch inside the closure. An error that escapes a script or `main` prints `uncaught Name: message` to stderr and exits with status 1; in a test it fails the test.
- A function handed to JavaScript (an npm package's callback or handler) takes `Js` parameters, and may pass errors on: they're thrown into JavaScript, a `JsError` as the value JavaScript threw, any other error as a JavaScript `Error` with its name and message. An async function hands JavaScript a promise, rejected with the error.
- A JavaScript property read (`ctx.params`) can throw, from a getter or a Proxy. Marked `try`, it passes a `JsError` on like a call. Unmarked, in a function JavaScript called, the exception goes back to that JavaScript caller, as in JavaScript (so does one from an npm module that fails as it loads there): the Tov code it passes through stops there and releases its values (a `finally` there doesn't run). An async arrow passed to a JavaScript function rejects its promise with it instead, for a read in the arrow or in a function it calls directly. Anywhere else, including an async function's code after an `await` when it isn't that arrow, it's a trap.
- Errors are return values underneath (no unwinding): a call that can throw costs one branch.
- Bugs (index out of range with `!`, integer overflow, failed `as`) are traps, not errors: they can't be caught, in a function JavaScript called too. In a test, a trap or a failed `expect` fails the test even there.
- Effects (`uses fs | net`) are planned.

## 7a. Standard library **[M3]**
Node's names, so existing habits work:
- `import { readFileSync, readFileBytesSync, writeFileSync, appendFileSync, existsSync, readdirSync, mkdirSync, rmSync, unlinkSync, statSync, accessSync } from "node:fs"` (also `"fs"`, `"std/fs"`): the synchronous API; failures throw `Error` with Node's messages (`ENOENT: no such file or directory, open 'x'`). `readdirSync` returns sorted names. `readFileBytesSync` returns the file's `u8[]` (what Node's `readFileSync` gives as a Buffer), and `writeFileSync`/`appendFileSync` take `string | u8[]`. `statSync` returns a `Stats` (`size`, `mtimeMs`, `isFile()`, `isDirectory()`); `accessSync(path, mode?)` takes Node's mode flags (0 exists, 4 read, 2 write, 1 execute).
- `import * as path from "node:path"`: `join`, `resolve`, `normalize`, `basename`, `dirname`, `extname`, `isAbsolute`, `sep` (POSIX).
- `process.argv` (`[binary, binary, ...args]`, so `process.argv.slice(2)` are the arguments), `process.env.NAME` (`string | undefined`), `process.exit(code?)`, `process.cwd()`, `process.platform`, `process.arch`, `process.pid`, `process.stdout.write(s)`, `process.stderr.write(s)`; `console.error` / `console.warn` write to stderr.
- `Date.now()` and `performance.now()` (milliseconds).
- `JSON.stringify(value, undefined, indent?)` as in JavaScript (maps with string keys are written as objects, where JavaScript writes `{}`). `JSON.parse(text)` takes its type from context — `const c: Config = try JSON.parse(text)` or `try JSON.parse(text) as Config` — and **checks** the input against it (exact integers, string literals, discriminants, required fields; extra keys ignored, `null` and missing keys read as `undefined`), throwing `SyntaxError` otherwise.
- Rest parameters: `function f(first: T, ...rest: U[])` (not in methods yet).
- Web servers: see §7c.

## 7b. Async
`async` functions, arrows and methods (class, static and object-literal), and `await`, work as in JavaScript, including the order things run in: microtasks run in order after each timer or request, and an `await` takes the same number of ticks as in Node and Bun (`tests/async` compares every program's output with Node's). A script's top level can `await`, and `main` can be `async`. Not yet: `for await`, `Promise.allSettled`/`any`, `.then`/`.catch`/`.finally`.
- **Promises.** An async function returns `Promise<T>`; its errors reject the promise instead of being thrown at the call: `async function load(id: string): Promise<User> throws NotFound` has type `(id: string) => Promise<User, NotFound>`. `await p` gives the `T`; if `p` can reject, `await` throws, so it's marked `try` (or caught) like a call: `const u = try await load(id)`. Awaiting a value that isn't a promise gives the value. `new Promise<T>((resolve, reject) => ...)` (it can reject only if the executor takes `reject`), `Promise.resolve(v)` and `Promise.reject(e)` make promises, `Promise.all(ps)` (every value, in order; the first rejection) and `Promise.race(ps)` (the first to settle) combine an array of them (their types merge: `Promise<A, E1> | Promise<B, E2>` is `Promise<A | B, E1 | E2>`); a rejection nobody awaits is reported like an uncaught error.
- **Timers.** `setTimeout(f, ms)` and `setInterval(f, ms)` return an id for `clearTimeout`/`clearInterval`; `queueMicrotask(f)`. Timers keep time as Node's do (whole milliseconds on the loop's clock) and fire within about 0.1 ms of it.
- **Running.** An async function runs until its first `await` that has to wait. Awaiting a call to an async function costs no allocation (its state lives inside the caller's); a call that isn't awaited runs as a task of its own. The program exits when nothing is left to run: no timers, tasks waiting on them, or servers.

## 7c. Web servers (Bun's API)
`Bun.serve`, `Request`, `Response`, `Headers`, `URL` and `URLSearchParams` are globals, as in Bun (also `import { serve, ... } from "std/http"`). A Bun server ports with Tov's usual edits only — `try` on calls that can throw, `.byteLength` for `.length` on strings; `tests/parity` runs a Bun server and its port side by side and compares every response.
```ts
const server = Bun.serve({
  port: Number(process.env.PORT ?? 3000),
  routes: {
    "/": new Response("Welcome"),
    "/users/:id": (req) => Response.json({ id: req.params.id }),   // req.params: { id: string }
    "/todos": {
      GET: () => Response.json(todos),
      POST: async (req) => {
        const t = try (await req.json()) as Todo   // a bad body → SyntaxError → 500
        todos.push(t)
        return Response.json(t, { status: 201 })
      },
    },
  },
  fetch(req) {
    const url = try new URL(req.url)
    return new Response(`Not Found: ${url.pathname}`, { status: 404 })
  },
})
console.log(`Listening on ${server.url}`)
```
- **Serving.** `Bun.serve` binds right away (an unusable port prints the error and exits, as an uncaught error does in Bun) and returns a `Server` (`port`, `hostname`, `url`, `stop(closeActive?)`); requests are served once the script's top level (or `main`) has finished, until every server is stopped. `workers: n` (Tov only) forks `n` processes sharing the sockets, supervised: a worker that dies is replaced, and SIGTERM/SIGINT stop them all.
- **Routing** as in Bun: exact paths first, then `:param` and `/*` routes; a route is a `Response`, a handler, or an object of per-method handlers (`GET`, `POST`, ...; others fall through to `fetch`). `req.params` is typed from the route. Unmatched requests go to `fetch`, else `404`.
- **Handlers** return a `Response`, or (`async`) a promise of one: their type is `(req, server) => Response | Promise<Response, Error> throws Error`. A synchronous response is written at once; an async one when its promise settles, and responses on a connection still go out in request order (pipelining works either way).
- **Errors.** A handler that throws (or whose promise rejects) is logged to stderr and answered by `error(err)` if given, else `500 Something went wrong!`, like Bun in production.
- **Types.** `Request`: `method`, `url`, `headers`, `text()` (a `Promise<string>`), `json<T>()` (a `Promise<T, SyntaxError>`, parsed against the expected type). `Response(body?, { status?, statusText?, headers? })`, `Response.json(value, init?)`, `Response.redirect(url, status?)`, `status`, `ok`, `headers`, `text()`, `json<T>()` (promises, as on `Request`). `Headers(init?)`: `get`/`has`/`set`/`append`/`delete`/`forEach`/`toJSON`, case-insensitive; CR/LF in names and values are dropped. `URL(input, base?)` (throws `TypeError`): `href`, `protocol`, `host`, `hostname`, `port`, `pathname`, `search`, `hash`, `origin`, `searchParams`; `URL.parse`, `URL.canParse`. `URLSearchParams`: `get`/`getAll`/`has`/`set`/`append`/`delete`/`size`/`forEach`/`keys`/`values`/`toString`. `Request` also has `formData()`, `blob()`, `bytes()` and `arrayBuffer()`; a `Response` body can be a `Blob` or a `FormData`.
- **Protocol.** HTTP/1.1 with keep-alive, pipelining, `Content-Length` and chunked request bodies, `Expect: 100-continue` and `HEAD`; `content-length` and `date` are added, and `content-type: text/plain;charset=utf-8` when a body has none. Malformed or oversized requests get `400`/`413`/`431` and are closed.
- Not yet: TLS for `Bun.serve`, HTTP/2, streaming bodies, `Bun.file`.
- **WebSockets** as in Bun: `Bun.serve<T>({ fetch(req, server) { if (server.upgrade(req, { data, headers })) return; ... }, websocket: { open, message, close, drain, ping, pong, maxPayloadLength } })`. A handler that upgrades returns nothing. `ServerWebSocket<T>`: `data` (`T | undefined`), `send(text | bytes)`, `sendText`, `sendBinary`, `ping`, `pong`, `close(code?, reason?)`, `terminate()`, `subscribe`, `unsubscribe`, `isSubscribed`, `subscriptions`, `publish(topic, data)` (to the other subscribers; `publishToSelf` includes it), `readyState`, `remoteAddress`; `server.publish(topic, data)` and `server.subscriberCount(topic)`. `message` gets a `string` for text and `u8[]` for binary. Frames are checked as RFC 6455 says (Autobahn's test suite passes); text must be UTF-8; a message over `maxPayloadLength` (16 MB) closes with 1009. Compression (`perMessageDeflate`) and idle timeouts aren't there yet.
- The **`WebSocket` client**: `new WebSocket(url, protocols? | { protocols?, headers?, tls? })` (`ws:`, `wss:`; `try`: a bad URL throws a `SyntaxError`), `onopen`/`onmessage`/`onclose`/`onerror` and `addEventListener`, `send` (messages sent before it opens go out once it does), `close(code?, reason?)`, `readyState`, `protocol`. Events are `WebSocketEvent`s: `data` (`string | u8[]`), `code`, `reason`, `wasClean`.

## 7d. fetch (Bun's client)
`fetch(input, init?)` is a global, as in Bun: `input` is a URL string, a `URL` or a `Request`; `init` is `{ method?, headers?, body?, redirect?, signal?, decompress?, tls?, unix?, proxy?, keepalive? }`. It returns `Promise<Response, Error>`: it resolves once the response's head has arrived (an HTTP error status still resolves, `ok` is false) and the body follows, and rejects with a `FetchError` (a `TypeError` with Bun's `message` and `code`: `ConnectionRefused`, `ENOTFOUND`, `ECONNRESET`, `TooManyRedirects`, `UnexpectedRedirect`, `Malformed_HTTP_Response`, `ZlibError`, `BrotliDecompressionError`, `ZstdDecompressionError`, `ERR_INVALID_URL`, ...) or, when its `signal` aborts, with the signal's reason.
```ts
type User = { id: number, name: string }
const res = try await fetch("http://api.local/users/1", { headers: { Accept: "application/json" }, signal: AbortSignal.timeout(5000) })
if (!res.ok) throw new Error(`HTTP ${res.status}`)
const user = try (await res.json()) as User
```
- **Requests.** Methods are sent upper-cased; every method but `GET`/`HEAD` sends `Content-Length` (a `GET`/`HEAD` with a body is an error). Bodies: `string`, `URLSearchParams` (sent as `application/x-www-form-urlencoded;charset=UTF-8`), bytes (`u8[]`), a `Blob` or `File` (its `type` is the `Content-Type`), or a `FormData` (sent as `multipart/form-data` with a random boundary). The defaults follow the caller's headers, as Bun sends them: `Connection: keep-alive`, `User-Agent`, `Accept: */*`, `Host`, `Accept-Encoding: gzip, deflate, br, zstd`; credentials in the URL become `Authorization: Basic`.
- **Responses.** `status`, `statusText`, `ok`, `headers`, `url` (after redirects, without the fragment), `redirected`, `type`, `bodyUsed`; `text()` (UTF-8 decoded: a BOM is dropped, invalid bytes become U+FFFD), `json<T>()` (checked against the expected type), `bytes()`/`arrayBuffer()` (`u8[]`), `blob()` (typed by the `Content-Type` as it is), `formData()` (a `multipart/form-data` or `application/x-www-form-urlencoded` body; any other rejects with `ERR_FORMDATA_PARSE_ERROR`), `clone()`. These wait for the whole body and reject if it fails (`ECONNRESET` for a connection that drops mid-body, the signal's reason when it aborts). A body is read once: another read rejects with `ERR_BODY_ALREADY_USED`. A response that's dropped unread frees its request (and closes the connection if the body is still arriving).
- **Streaming.** `res.body` is a `ReadableStream` of `u8[]` chunks as they arrive: `getReader()` → `read()` (`{ done, value }`), `cancel()`, `releaseLock()`, `locked`. `for await (const chunk of stream) { ... }` reads it chunk by chunk (it throws, like `try`, if the body fails; leaving the loop early cancels the stream). A reader that falls behind pauses the connection at 4 MB. A compressed body read this way is decoded as it arrives (gzip and deflate with zlib, brotli, zstd), at most 4 MB ahead of the reader, so a small body that decodes to a huge one is decoded only as it's read; one read whole (`text()` and the like) is decoded at once when it's in (gzip and deflate with libdeflate, twice as fast). A stream that ends before its encoding does fails (`ZlibError`, ...). `TextDecoder` (`decode(bytes, { stream: true })` completes characters cut between chunks; `fatal`, `ignoreBOM`) and `TextEncoder` (`encode(text)`) convert between text and `u8[]`.
- **HTTPS.** TLS 1.3 and 1.2 (BoringSSL, vendored: AES-GCM with the CPU's AES instructions, else ChaCha20-Poly1305; X25519 and post-quantum ML-KEM key shares; ALPN `http/1.1`; SNI), with sessions resumed per origin. Certificates are checked against the system's CA bundle (`SSL_CERT_FILE` overrides it, `NODE_EXTRA_CA_CERTS` adds to it) and the host name; failures reject with Node's codes (`CERT_HAS_EXPIRED`, `DEPTH_ZERO_SELF_SIGNED_CERT`, `SELF_SIGNED_CERT_IN_CHAIN`, `UNABLE_TO_VERIFY_LEAF_SIGNATURE`, `ERR_TLS_CERT_ALTNAME_INVALID`, ...). `tls: { ca?, rejectUnauthorized? }` adds trusted certificates or turns the checks off, as does `NODE_TLS_REJECT_UNAUTHORIZED=0`.
- **Protocol.** HTTP/1.1 on pooled keep-alive connections per origin (a pooled connection the server has closed is replaced and the request retried once); `Content-Length`, chunked (extensions and trailers) and close-delimited bodies, interim `1xx` responses, HTTP/1.0 servers; gzip, deflate, brotli and zstd bodies are decoded unless `decompress: false` (`Accept-Encoding: gzip, deflate, br, zstd`, as Bun sends). DNS lookups run on a small thread pool and are cached for 30 s.
- **Redirects** (`redirect: "follow"`, the default): up to 20, as the Fetch standard says; 303 (and 301/302 after a `POST`) continue as `GET` without the body; a redirect to another origin drops `Authorization`, `Proxy-Authorization` and `Cookie`. `"manual"` returns the 3xx response; `"error"` rejects.
- **Cancelling.** `AbortController` (`signal`, `abort(reason?)`), `AbortSignal` (`aborted`, `reason`, `throwIfAborted()`, `addEventListener("abort", f)`, `onabort`, `AbortSignal.abort(reason?)`, `AbortSignal.timeout(ms)`, `AbortSignal.any(signals)`) and `DOMException` (`name`, `code`). An aborted request's connection is closed. `AbortSignal.timeout`'s timer doesn't keep the program running by itself.
- **Headers** (client and server): `get` joins repeated names with `", "`; `getSetCookie()` lists each cookie; `forEach`, `keys()`, `values()` and `toJSON()` see names lower-cased and sorted, repeated names combined.
- **Unix sockets** (Bun's `unix: "/path/to.sock"`): the request goes over that socket; the URL still gives the path and `Host` (`http://localhost/v1/info` for the Docker API). A socket that can't be opened rejects with `FailedToOpenSocket`.
- **Proxies** (Bun's `proxy: "http://user:pass@host:port"`, else `HTTP_PROXY`/`HTTPS_PROXY` (or lower case) unless `NO_PROXY` names the host): plain HTTP goes to the proxy in absolute form; HTTPS through a `CONNECT` tunnel, with TLS and certificate checks end to end. Credentials in the proxy URL become `Proxy-Authorization`. A proxy that refuses the tunnel (`407`) answers the request, as in Bun.
- **Blob, File, FormData.** `new Blob(parts?, { type? })` joins strings (as UTF-8), `u8[]` and Blobs: `size`, `type` (lower-cased; `""` unless printable ASCII), `slice(start?, end?, type?)` (negative offsets count from the end), `text()`, `json<T>()`, `bytes()`, `arrayBuffer()`, `stream()`. `new File(parts, name, { type?, lastModified? })` adds `name` and `lastModified`. `FormData`: `append`/`set` (a string, or a Blob added as a `File`: named by the third argument, else its own name, `"blob"` for a plain Blob), `get`, `getAll`, `has`, `delete`, `forEach`, `keys()`, `values()`; values are `string | File`. It's written as browsers write it: newlines in names and text values become CRLF, and `"`, CR and LF in names and file names are percent-escaped. Unlike Bun, a file is sent with its own `type` (Bun sends most as `application/octet-stream`), and a plain Blob is named `"blob"` (as the standard and Node do).
- Not yet: request bodies as streams, client certificates, `https:` proxies.

## 7e. TCP and TLS sockets (Bun's API)
`Bun.connect(options)` and `Bun.listen(options)`, as in Bun: `hostname`, `port` (or `unix`), `tls` (`true`, or `{ ca?, rejectUnauthorized?, serverName? }` to connect; `{ cert, key }`, PEM text, to listen), `data` (the sockets' starting `socket.data`), `allowHalfOpen`, and `socket`, the handlers: `open(socket)`, `data(socket, bytes)` (a `u8[]`), `drain(socket)`, `end(socket)` (the peer finished sending), `close(socket, error?)`, `error(socket, error)`, `connectError(socket, error)`, `handshake(socket, success, authorizationError?)`.
```ts
type Session = { name: string }
const server = Bun.listen<Session>({
  hostname: "0.0.0.0",
  port: 7000,
  socket: {
    open(socket) { socket.data = { name: "guest" } },
    data(socket, data) { socket.write(data) },
  },
})
const client = try await Bun.connect({ hostname: "localhost", port: 7000, socket: { data(socket, data) { console.log(data.length) } } })
client.write("hello")
```
- `Bun.connect` resolves once the socket is open (with TLS, once the handshake is done and the certificate checked) and rejects with a `SocketError` (`code`: `ECONNREFUSED`, `ENOTFOUND`, `CERT_HAS_EXPIRED`, ...) when it can't connect. `Bun.listen` returns a `TCPSocketListener` (`port`, `hostname`, `stop(closeActive?)`, `ref`, `unref`); a port that can't be bound prints the error and exits, as an uncaught error does in Bun.
- `Socket`: `write(text | bytes)`, `end(data?)`, `flush()`, `terminate()`, `shutdown()`, `setNoDelay(on?)`, `ref()`, `unref()`, `data`, `remoteAddress`, `remotePort`, `localAddress`, `localPort`, `bufferedAmount` (Tov). Unlike Bun's, `write` takes everything: what the kernel can't take yet is buffered (Bun returns a short count and drops the rest), and `drain` follows once it's out. Writes made in a handler go out together when it returns: one system call for many small writes.
- `end()` closes the socket once its data is out (`close` runs); with `allowHalfOpen` it shuts only the sending side, and the peer can keep sending. A handler that throws hands the error to `error`, else it's logged and the socket closes. A connect's `socket.data` is `T | undefined` (`T` from `data`, or the type argument: `Bun.listen<Session>(...)`).

## 8. Tests **[M0]**
```ts
test("adds", () => {
  expect(add(1, 2)).toBe(3)
})
```
`test` calls are top-level only. Matchers: `toBe`, `toEqual`, `toBeCloseTo`, `toBeUndefined`, `toBeDefined`.

## 9. Diagnostics
Every diagnostic has a stable code, a location, the observed problem, **the valid alternatives**, and — where possible — a fix tagged `safe` (apply automatically), `maybe` (review), or `placeholder` (needs input). `tov explain <CODE>` prints details.

Code prefixes: `L` lexing · `P` parsing · `N` names/imports · `T` types · `F` control flow · `V` values/mutation · `X` removed TypeScript feature.
