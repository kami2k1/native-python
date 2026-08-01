# A pip-installed module simple enough to COMPILE: the bundler must pick it
# up from site-packages and compile it natively (no bridge involved).
GREETING = "hello from purelib"

def double(x):
    return x * 2
