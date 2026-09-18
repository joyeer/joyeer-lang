## §16 Worked Examples

### 16.1 Quicksort (canonical demo)

```joyeer
import std.io

func medianOfThree(a: Int, b: Int, c: Int): Int {
  let r: Int
  if (a <= b && b <= c) || (c <= b && b <= a) {
    r = b
  } else if (b <= a && a <= c) || (c <= a && a <= b) {
    r = a
  } else {
    r = c
  }
  assert(condition: r == a || r == b || r == c)
  return r
}

func swap(a: inout Int, b: inout Int) {
  let tmp = a
  &a = b
  &b = tmp
}

func partition(array: inout [Int], lo: Int, hi: Int): Int {
  precondition(condition: 0 <= lo && lo <= hi && hi < array.count)
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
      &array.swapAt(i: i, j: j)   // one inout projection of `array`; cannot pass &array[i] and &array[j] (§4.4.2)
      i += 1
      j -= 1
    }
  }
  assert(condition: lo <= j && j <= hi)
  return j
}

func quickSortRange(array: inout [Int], lo: Int, hi: Int) {
  precondition(condition: hi < array.count)
  if lo < hi {
    let p = partition(array: &array, lo: lo, hi: hi)
    quickSortRange(array: &array, lo: lo,    hi: p)
    quickSortRange(array: &array, lo: p + 1, hi: hi)
  }
}

public func quickSort(input: consuming [Int]): [Int] {
  var arr = input
  if arr.count > 1 {
    quickSortRange(array: &arr, lo: 0, hi: arr.count - 1)
  }
  assert(condition: arr.count == input.count)
  return arr
}

public func main() {
  let unsorted = [8, 6, 1, 2, 1, 12, 3, 4, 34]
  let sorted = quickSort(input: consume unsorted)
  for x in sorted { print(value: x) }
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
  Unexpected(UInt8, at: Int),
  InvalidNumber(String),
}

struct Parser {
  var input: String
  var pos: Int
}

func peek(p: Parser): UInt8? {
  if p.pos >= p.input.count { return nil }
  return p.input[p.pos]
}

func advance(p: inout Parser): UInt8? {
  let oldPos = p.pos
  if p.pos >= p.input.count {
    assert(condition: p.pos == oldPos)
    return nil
  }
  let c = p.input[p.pos]
  &p.pos += 1
  assert(condition: p.pos == oldPos + 1)
  return c
}

func parseValue(p: inout Parser): Result<JsonValue, JsonError> {
  skipWhitespace(p: &p)
  let c = peek(p: p) ?? return .Err(.UnexpectedEof)
  return match c {
    b'"'        => parseString(p: &p),
    b'{'        => parseObject(p: &p),
    b'['        => parseArray(p: &p),
    b't', b'f'  => parseBool(p: &p),
    b'n'        => parseNull(p: &p),
    _           => parseNumber(p: &p),
  }
}

public func parse(source: String): Result<JsonValue, JsonError> {
  var p = Parser(input: source, pos: 0)
  return parseValue(p: &p)
}
```

### 16.3 Linked list (indirect enum + deinit demo)

```joyeer
public enum IntList {
  Empty,
  indirect Cons(Int, IntList),
}

extension IntList {
  public func count(): Int {
    match self {
      .Empty       => 0,
      .Cons(_, t)  => 1 + t.count(),
    }
  }

  public func prepended(x: consuming Int): IntList {
    .Cons(x, self)
  }
}
```

### 16.4 String builder (subscript + mutating/consuming demo)

```joyeer
public struct StringBuilder {
  var buffer: [UInt8]

  public init() { buffer = [] }

  public mutating func append(s: String) {
    for b in s.utf8() { &buffer.append(element: b) }
  }

  public subscript(i: Int): UInt8 {
    borrowing { yield  buffer[i] }
    inout     { yield &buffer[i] }
  }

  public consuming func build(): String {
    String(utf8: buffer)
  }
}

public func main() {
  var sb = StringBuilder()
  &sb.append(s: "hello, ")
  &sb.append(s: "world")
  let s = consume sb.build()    // build() is consuming; consume marks the receiver
  print(value: s)
}
```

---

