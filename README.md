Joyeer is a general purpose programming language, which inspired by Swift.

*The project is still in developing, not release yet!* 


### Requirements

*The development version is only running in macOS*

- macOS > 12.0
- cmake > 3.22.0
- clang > 13.0.0
- Python 2.7

### Getting Started

#### Building

```shell
    cmake -B ./build -G 'Unix Makefiles'
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
