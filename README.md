Joyeer is a general purpose programming language, which inspired by Swift.

*The project is still in developing, not release yet!* 


### Requirements

- macOS > 12.0
- cmake > 3.22.0
- clang > 13.0.0
- Python > 3.10
- Ninja > 1.11.0

### Getting Started

#### Building

```shell
    cmake -B ./build -G Ninja
    cmake --build ./build
```

#### Testing

```shell
    cd build
    ctest
```

#### Examples

- [Quick Sort in joyeer][].
- [More language examples][].
  
[quick sort in joyeer]: /tests/leetcode/quick_sort.joyeer
[more language examples]: /tests/
