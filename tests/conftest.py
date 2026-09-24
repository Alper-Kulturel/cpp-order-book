"""Make the compiled extension importable when pytest runs from the repo root.

The CMake build drops order_book_cpp*.so and bench_driver*.so into python/, so
adding that directory to sys.path is all the test suite needs.
"""

import os
import sys

_PYTHON_DIR = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "python")
if _PYTHON_DIR not in sys.path:
    sys.path.insert(0, _PYTHON_DIR)
