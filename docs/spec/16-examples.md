## §16 Worked Examples

### 16.1 Quicksort (canonical demo)

```joyeer
import std.io

func medianOfThree(a: Int, b: Int, c: Int): Int
  ensures result == a || result == b || result == c
{
  if (a <= b && b <= c) || (c <= b && b <= a) { return b }
  if (b <= a && a <= c) || (c <= a && a <= b) { return a }
  return c
}

func swap<T>(_ a: inout T, _ b: inout T) {
  let tmp = a
  a = b
  b = tmp
}

func partition(_ array: inout [Int], lo: Int, hi: Int): Int
  requires 0 <= lo && lo <= hi && hi < array.count
  ensures lo <= result && result <= hi
{
  let pivot = medianOfThree(
    a: array[lo],
    b: array[(lo + hi) / 2],
    c: array[hi],
  )
  var i = lo
  var j = hi
  while i <= j {
    while array[i] < pivot { i += 1 }
    while array[j] > pivot { j -= 1 }
    if i <= j {
      swap(&array[i], &array[j])
      i += 1
      j -= 1
    }
  }
  return j
}

func quickSortRange(_ array: inout [Int], lo: Int, hi: Int)
  requires hi < array.count
{
  if lo < hi {
    let p = partition(&array, lo: lo, hi: hi)
    quickSortRange(&array, lo: lo,    hi: p)
    quickSortRange(&array, lo: p + 1, hi: hi)
  }
}

public func quickSort(_ input: consuming [Int]): [Int]
  ensures result.count == input.count
{
  var arr = input
  if arr.count > 1 {
    quickSortRange(&arr, lo: 0, hi: arr.count - 1)
  }
  return arr
}

public func main() {
  let unsorted = [8, 6, 1, 2, 1, 12, 3, 4, 34]
  let sorted = quickSort(consume unsorted)
  for x in sorted { print(x) }
}
```

### 16.2 JSON parser (sketch matching v0.1-plan.md)

```joyeer
public enum JsonValue {
  Null,
  Bool(Bool),
  Number(Double),
  Str(String),
  Array([JsonValue]),
  Object([String: JsonValue]),
}

public enum JsonError {
  UnexpectedEof,
  Unexpected(Char, at: Int),
  InvalidNumber(String),
}

struct Parser {
  var input: String
  var pos: Int
}

func peek(_ p: Parser): Char? {
  if p.pos >= p.input.count { return nil }
  return p.input[p.pos]
}

func advance(_ p: inout Parser): Char?
  ensures p.pos == old(p.pos) + 1 || (result == nil && p.pos == old(p.pos))
{
  if p.pos >= p.input.count { return nil }
  let c = p.input[p.pos]
  p.pos += 1
  return c
}

func parseValue(_ p: inout Parser): Result<JsonValue, JsonError> {
  skipWhitespace(&p)
  let c = peek(p) ?? return .Err(.UnexpectedEof)
  return match c {
    '"' => parseString(&p),
    '{' => parseObject(&p),
    '[' => parseArray(&p),
    't', 'f' => parseBool(&p),
    'n' => parseNull(&p),
    _   => parseNumber(&p),
  }
}

public func parse(_ source: String): Result<JsonValue, JsonError> {
  var p = Parser(input: source, pos: 0)
  return parseValue(&p)
}
```

### 16.3 Linked list (indirect enum + deinit demo)

```joyeer
public enum List<T> {
  Empty,
  indirect Cons(T, List<T>),
}

extension List<T> {
  public func count(): Int {
    match self {
      .Empty       => 0,
      .Cons(_, t)  => 1 + t.count(),
    }
  }

  public func prepended(_ x: consuming T): List<T> {
    .Cons(x, self)
  }
}
```

### 16.4 String builder (subscript + mutating/consuming demo)

```joyeer
public struct StringBuilder {
  var buffer: [UInt8]

  public init() { buffer = [] }

  public mutating func append(_ s: String) {
    for b in s.utf8() { &buffer.append(b) }
  }

  public subscript(_ i: Int): UInt8 {
    borrowing { yield  buffer[i] }
    inout     { yield &buffer[i] }
  }

  public consuming func build(): String {
    String(utf8: buffer)
  }
}

public func main() {
  var sb = StringBuilder()
  &sb.append("hello, ")
  &sb.append("world")
  let s = consume sb.build()    // build() is consuming; consume marks the receiver
  print(s)
}
```

---

