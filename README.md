# Agentic Alpha C++ Quant Core Study Files

These are **real C++ source files**. Python does not translate them into C++.
They are compiled by a C++ compiler such as `g++` or `clang++`.

## Recommended learning order

1. `00_hello_cpp.cpp`
2. `01_cpp_quant_basics.cpp`
3. `02_contract_and_market.cpp`
4. `03_signal_order_risk.cpp`
5. `04_execution_simulator.cpp`
6. `05_position_and_pnl.cpp`
7. `06_event_driven_backtester.cpp`

## Local IDE

On macOS with Apple Clang:

```bash
clang++ -std=c++20 01_cpp_quant_basics.cpp -o lesson01
./lesson01
```

On Linux / Google Colab:

```bash
g++ -std=c++20 01_cpp_quant_basics.cpp -o lesson01
./lesson01
```

## Google Colab

Colab's managed runtime is primarily Python. You do **not** need a C++ kernel to run real C++.
Use shell cells to compile the `.cpp` source with `g++`:

```python
!g++ -std=c++20 01_cpp_quant_basics.cpp -o lesson01
!./lesson01
```

If the file is on your local computer, upload it first:

```python
from google.colab import files
uploaded = files.upload()
```

Then compile it with the two commands above.

## Important distinction

This:

```python
!g++ -std=c++20 program.cpp -o program
!./program
```

means:

1. the notebook asks the Linux shell to run `g++`;
2. `g++` compiles your **real C++ source** into a native executable;
3. the next command runs that executable.

Python is only the notebook's default kernel. It is not translating or executing the C++ semantics.

## About C++ kernels in Colab

Google Colab does not officially provide a managed C++ Jupyter kernel. Third-party `xeus-cling`/`cling` setups can be made to work in some Jupyter environments, but they are brittle in hosted Colab and are not needed for this course.

For interactive pure-C++ notebooks, use a local Jupyter installation with a C++ kernel. For Colab, compiling `.cpp` files with `g++` is simpler and closer to how production C++ is actually built.
