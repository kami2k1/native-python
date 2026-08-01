# Standard library `warnings` fallback for KamiPython AOT compiler.

def warn(message, category=None, stacklevel=1, source=None):
    pass

def filterwarnings(action, message="", category=None, module="", lineno=0, append=False):
    pass

def simplefilter(action, category=None, lineno=0, append=False):
    pass

def resetwarnings():
    pass

class Warning(Exception):
    pass

class UserWarning(Warning):
    pass

class DeprecationWarning(Warning):
    pass

class SyntaxWarning(Warning):
    pass

class RuntimeWarning(Warning):
    pass

class FutureWarning(Warning):
    pass

class ImportWarning(Warning):
    pass

class UnicodeWarning(Warning):
    pass

class BytesWarning(Warning):
    pass

class ResourceWarning(Warning):
    pass
