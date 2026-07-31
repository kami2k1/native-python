# Local module used by feat_bundle_app.py (bundled into its executable).
# It is also compiled standalone by the test runner, which checks that the
# __main__ guard DOES run when this file is the program itself.
import math

GREETING = "hello"


def helper(name, excited=False):
    msg = GREETING + " " + name
    if excited:
        msg = msg + "!"
    return msg


def circle_area(r):
    return math.pi * r * r


print("util loaded")

if __name__ == "__main__":
    print("util main")
