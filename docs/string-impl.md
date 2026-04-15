# Joyeer v0.1 — String Implementation Plan

## Design Principle

Follow the Swift approach:
- Memory layout and core operations are built into the compiler/runtime (Rust)
- Public API is exposed through a unified method table
- Users can add methods via `extension String { }` or `func String.method()`
- Builtin methods and user extension methods share the same method table and lookup rules

---

## Layer 1: Runtime (Rust)

### Memory Layout

```rust
/// String object stored on the GC heap
#[repr(C)]
struct StringData {
    head: ObjectHead,    // 8 bytes — type tag + refcount
    length: i64,         // 8 bytes — byte length
    // followed by `length` bytes of UTF-8 data (flexible array)
}
```

### Core Runtime Functions

```rust
impl StringClass {
    /// Allocate a String from a string constant pool entry
    fn allocate(&self, vm: &mut IsolateVM, const_slot: usize) -> Address {
        let raw = &vm.strings[const_slot];
        let obj = vm.gc.allocate(self, size_of::<StringData>() + raw.len());
        let data = obj.cast::<StringData>();
        data.head.type_slot = ValueType::String as i64;
        data.length = raw.len() as i64;
        unsafe { ptr::copy_nonoverlapping(raw.as_ptr(), data.data_ptr(), raw.len()) };
        obj
    }

    /// Allocate a String from a raw byte slice
    fn allocate_from_bytes(&self, vm: &mut IsolateVM, bytes: &[u8]) -> Address {
        let obj = vm.gc.allocate(self, size_of::<StringData>() + bytes.len());
        let data = obj.cast::<StringData>();
        data.head.type_slot = ValueType::String as i64;
        data.length = bytes.len() as i64;
        unsafe { ptr::copy_nonoverlapping(bytes.as_ptr(), data.data_ptr(), bytes.len()) };
        obj
    }

    /// Get the byte length of a String
    fn get_length(&self, addr: Address) -> i64 {
        let data = addr.cast::<StringData>();
        data.length
    }

    /// Get a pointer to the raw bytes
    fn as_bytes(&self, addr: Address) -> *const u8 {
        let data = addr.cast::<StringData>();
        data.data_ptr()
    }

    /// Convert to Rust String (for debug/print)
    fn to_rust_string(&self, addr: Address) -> String {
        let data = addr.cast::<StringData>();
        let slice = unsafe { std::slice::from_raw_parts(data.data_ptr(), data.length as usize) };
        String::from_utf8_lossy(slice).into_owned()
    }
}
```

### Builtin Native Functions (exposed to Joyeer)

```rust
/// String.len() -> Int
fn string_len(executor: &mut Executor, args: &Arguments) -> Value {
    let self_addr = args.get(0); // self
    let len = executor.vm.string_class.get_length(self_addr);
    len as Value
}

/// String.charAt(index: Int) -> Char
fn string_char_at(executor: &mut Executor, args: &Arguments) -> Value {
    let index = args.get(0) as usize;  // index parameter
    let self_addr = args.get(1);        // self
    let ptr = executor.vm.string_class.as_bytes(self_addr);
    let ch = unsafe { *ptr.add(index) };
    ch as Value
}

/// Int.toString() -> String
fn int_to_string(executor: &mut Executor, args: &Arguments) -> Value {
    let value = args.get(0) as i64; // self
    let s = value.to_string();
    let addr = executor.vm.string_class.allocate_from_bytes(&mut executor.vm, s.as_bytes());
    addr as Value
}

/// Char.toString() -> String
fn char_to_string(executor: &mut Executor, args: &Arguments) -> Value {
    let ch = args.get(0) as u8; // self
    let addr = executor.vm.string_class.allocate_from_bytes(&mut executor.vm, &[ch]);
    addr as Value
}
```

---

## Layer 2: Compiler — Registering Builtin Methods

### Builtin Enum

```rust
enum BuiltIn {
    // ... existing entries ...

    // String methods
    StringFuncLen,
    StringFuncCharAt,

    // Int methods
    IntFuncToString,

    // Char methods
    CharFuncToString,
}
```

### Registration

```rust
impl IsolateVM {
    fn register_builtin_methods(&mut self) {
        // String
        self.register_method("String", "len",     string_len,     0); // 0 params (self only)
        self.register_method("String", "charAt",  string_char_at, 1); // 1 param + self

        // Int
        self.register_method("Int", "toString",   int_to_string,  0);

        // Char
        self.register_method("Char", "toString",  char_to_string, 0);
    }
}
```

---

## Layer 3: User-Facing API

### Builtin Methods (available without import)

```
var s = "hello world"

s.len()                   // -> 11
s.charAt(index: 0)        // -> 'h'
s.charAt(index: 4)        // -> 'o'
```

### User Extensions (via `extension` block)

```
extension String {
    func contains(ch: Char): Bool {
        var i = 0
        while i < self.len() {
            if self.charAt(index: i) == ch {
                return true
            }
            i = i + 1
        }
        return false
    }

    func substring(from: Int, to: Int): String {
        var result = ""
        var i = from
        while i < to {
            result = result + self.charAt(index: i).toString()
            i = i + 1
        }
        return result
    }

    func startsWith(prefix: String): Bool {
        if prefix.len() > self.len() {
            return false
        }
        var i = 0
        while i < prefix.len() {
            if self.charAt(index: i) != prefix.charAt(index: i) {
                return false
            }
            i = i + 1
        }
        return true
    }

    func trim(): String {
        var start = 0
        var end = self.len() - 1
        while start <= end && (self.charAt(index: start) == ' '
                || self.charAt(index: start) == '\n'
                || self.charAt(index: start) == '\t') {
            start = start + 1
        }
        while end >= start && (self.charAt(index: end) == ' '
                || self.charAt(index: end) == '\n'
                || self.charAt(index: end) == '\t') {
            end = end - 1
        }
        return self.substring(from: start, to: end + 1)
    }
}
```

### Type-Prefix Function Syntax

```
func String.reversed(): String {
    var result = ""
    var i = self.len() - 1
    while i >= 0 {
        result = result + self.charAt(index: i).toString()
        i = i - 1
    }
    return result
}
```

---

## Method Lookup Flow

```
s.len()
  |
  +-- 1. Type of s? -> String
  +-- 2. Does String have a field named "len"? -> No
  +-- 3. Does the method table have "len"? -> Yes (builtin, NativeFunc)
  +-- 4. Emit: INVOKE string_len

s.contains(ch: 'e')
  |
  +-- 1. Type of s? -> String
  +-- 2. Does the method table have "contains"? -> Yes (extension, VMFunc)
  +-- 3. Emit: INVOKE String_contains
```

Builtin methods (NativeFunc) and user extension methods (VMFunc) share the same method table. The lookup logic is identical.

---

## String Operator Support

### Existing

| Operator | Implementation | Notes |
|---|---|---|
| `+` concatenation | StringBuilder builtin | `"a" + "b"` goes through StringBuilder |
| `==` equality | TBD | Content-based comparison |

### To Add

| Operator | Notes |
|---|---|
| `!=` | String inequality |

---

## Char Type (New)

### Memory Representation

`Char` is a value type, stored directly as an integer. No heap allocation needed.

v0.1 simplified: **Char = 1 byte ASCII** (sufficient for JSON parser).

Future: consider 4-byte Unicode scalar (like Rust's `char`).

### Literals

```
'a'    // plain character
'\n'   // escape: newline
'\t'   // escape: tab
'\\'   // escape: backslash
'\''   // escape: single quote
'\"'   // escape: double quote
```

### Operations

```
ch == 'a'         // comparison
ch != 'b'         // inequality
ch >= '0'         // range check
ch <= '9'
ch - '0'          // -> Int (char to digit)
ch.toString()     // -> String (single character string)
```

---

## Implementation Checklist

| Component | Changes |
|---|---|
| Runtime: types | Add `StringData` struct, `StringClass` with `allocate_from_bytes`, `get_length`, `as_bytes` |
| Runtime: builtins | Add `string_len`, `string_char_at`, `int_to_string`, `char_to_string` native functions |
| Runtime: VM | Register builtin methods in method table during VM initialization |
| Compiler: lexer | Add `'x'` char literal scanning |
| Compiler: tokens | Add `CharLit` token variant |
| Compiler: parser | Parse `extension` blocks and `func T.method()` declarations |
| Compiler: type checker | Resolve `x.method()` calls via method table lookup |
| Compiler: codegen | Emit method call bytecode with implicit `self` parameter |
