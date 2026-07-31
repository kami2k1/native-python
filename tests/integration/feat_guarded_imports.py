# The exact real-world pattern from the bug report: guarded imports of
# unavailable modules, docstrings, __name__ guard, doctest.
"""Module docstring: a small app skeleton."""
import sys
import time

try:
    import cv2
    import numpy as np
    from PIL import Image
except ImportError:
    pass

try:
    import pyautogui
except ImportError:
    pyautogui = None

def try_click():
    # Method call with kwargs on a dynamic receiver: must compile, and the
    # failure must surface as a catchable runtime exception, not a build error.
    try:
        loc = pyautogui.locateCenterOnScreen("img.png", confidence=0.8)
        return loc
    except Exception:
        return "guarded"

def find_button(name, retries=3):
    """Pretends to look for a UI button."""
    for attempt in range(retries):
        if name == "ok" and attempt >= 1:
            return attempt
    return -1

def main():
    started = time.time()
    print("button at attempt:", find_button("ok"))
    print("missing:", find_button("cancel"))
    print("argv is a list:", isinstance(sys.argv, list))
    print("kwargs method call guarded:", try_click())
    print("elapsed >= 0:", time.time() - started >= 0.0)
    return 0

if __name__ == "__main__":
    import doctest
    doctest.testmod()
    main()
