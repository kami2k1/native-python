"""Event logging — pure Python, compiled to native code by kamipy.

Replaces the hand-written C++ logger that used to live in runtime/src/netio.cpp.
The public surface follows CPython closely enough for ordinary use:

    logging.basicConfig(level=logging.INFO,
                        format="%(asctime)s | %(levelname)s | %(message)s")
    logging.info("connected to %s:%d", host, port)
    log = logging.getLogger(__name__)
    log.warning("retrying")

Records go to stderr, exactly like CPython's default `StreamHandler`.
"""
import sys
import time

CRITICAL = 50
FATAL = 50
ERROR = 40
WARNING = 30
WARN = 30
INFO = 20
DEBUG = 10
NOTSET = 0

BASIC_FORMAT = "%(levelname)s:%(name)s:%(message)s"

_LEVEL_NAMES = {
    50: "CRITICAL",
    40: "ERROR",
    30: "WARNING",
    20: "INFO",
    10: "DEBUG",
    0: "NOTSET",
}

_NAME_LEVELS = {
    "CRITICAL": 50,
    "FATAL": 50,
    "ERROR": 40,
    "WARNING": 30,
    "WARN": 30,
    "INFO": 20,
    "DEBUG": 10,
    "NOTSET": 0,
}

# Module state. `_config` holds what basicConfig() set up; keeping it in a dict
# means the handler functions can mutate it without `global` declarations.
_config = {
    "level": WARNING,
    "format": BASIC_FORMAT,
    "datefmt": "%Y-%m-%d %H:%M:%S",
    "stream": None,
    "filename": None,
    "file": None,
    "configured": False,
}


def getLevelName(level):
    name = _LEVEL_NAMES.get(level)
    if name is None:
        return "Level " + str(level)
    return name


def _coerce_level(level):
    if isinstance(level, str):
        got = _NAME_LEVELS.get(level.upper())
        if got is None:
            raise ValueError("ValueError: unknown level '" + level + "'")
        return got
    return int(level)


def basicConfig(level=None, format=None, datefmt=None, stream=None, filename=None,
                filemode="a", force=False):
    if level is not None:
        _config["level"] = _coerce_level(level)
    if format is not None:
        _config["format"] = format
    if datefmt is not None:
        _config["datefmt"] = datefmt
    if stream is not None:
        _config["stream"] = stream
    if filename is not None:
        _config["filename"] = filename
        _config["file"] = open(filename, filemode)
    _config["configured"] = True


def _target():
    f = _config["file"]
    if f is not None:
        return f
    s = _config["stream"]
    if s is not None:
        return s
    return sys.stderr


def _asctime():
    now = time.time()
    stamp = time.strftime(_config["datefmt"], time.localtime(now))
    millis = int((now - int(now)) * 1000)
    # CPython's default asctime carries milliseconds after a comma.
    if _config["datefmt"] == "%Y-%m-%d %H:%M:%S":
        return stamp + "," + _pad3(millis)
    return stamp


def _pad3(n):
    s = str(n)
    while len(s) < 3:
        s = "0" + s
    return s


def _render(name, level, message):
    fmt = _config["format"]
    out = ""
    i = 0
    n = len(fmt)
    while i < n:
        ch = fmt[i]
        if ch != "%" or i + 1 >= n or fmt[i + 1] != "(":
            out = out + ch
            i = i + 1
            continue
        close = fmt.find(")", i)
        if close < 0:
            out = out + ch
            i = i + 1
            continue
        field = fmt[i + 2:close]
        # skip the conversion tail, e.g. "%(levelname)-8s"
        j = close + 1
        while j < n and fmt[j] not in "sdif":
            j = j + 1
        spec = fmt[close + 1:j]
        if j < n:
            j = j + 1
        if field == "message":
            value = message
        elif field == "levelname":
            value = getLevelName(level)
        elif field == "levelno":
            value = str(level)
        elif field == "name":
            value = name
        elif field == "asctime":
            value = _asctime()
        elif field == "msecs":
            value = _pad3(int((time.time() % 1.0) * 1000))
        elif field == "process":
            value = str(_pid())
        else:
            value = "?"
        if spec != "" and spec[0] == "-":
            width = 0
            digits = spec[1:]
            if digits.isdigit():
                width = int(digits)
            while len(value) < width:
                value = value + " "
        out = out + value
        i = j
    return out


def _pid():
    import os
    return os.getpid()


def _emit(name, level, msg, args):
    if level < _config["level"]:
        return
    text = str(msg)
    if len(args) > 0:
        text = text % args
    line = _render(name, level, text) + "\n"
    out = _target()
    sys.stdout.flush()
    out.write(line)
    out.flush()


class Logger:
    def __init__(self, name):
        self.name = name
        self.level = NOTSET

    def setLevel(self, level):
        self.level = _coerce_level(level)
        # CPython keeps per-logger levels; with a single root handler the
        # effective threshold is what basicConfig() recorded, so mirror it.
        _config["level"] = self.level

    def getEffectiveLevel(self):
        if self.level != NOTSET:
            return self.level
        return _config["level"]

    def isEnabledFor(self, level):
        return level >= self.getEffectiveLevel()

    def log(self, level, msg, *args):
        _emit(self.name, _coerce_level(level), msg, args)

    def debug(self, msg, *args):
        _emit(self.name, DEBUG, msg, args)

    def info(self, msg, *args):
        _emit(self.name, INFO, msg, args)

    def warning(self, msg, *args):
        _emit(self.name, WARNING, msg, args)

    def warn(self, msg, *args):
        _emit(self.name, WARNING, msg, args)

    def error(self, msg, *args):
        _emit(self.name, ERROR, msg, args)

    def critical(self, msg, *args):
        _emit(self.name, CRITICAL, msg, args)

    def fatal(self, msg, *args):
        _emit(self.name, CRITICAL, msg, args)

    def exception(self, msg, *args):
        _emit(self.name, ERROR, msg, args)

    def addHandler(self, handler):
        return None

    def removeHandler(self, handler):
        return None


root = Logger("root")
_loggers = {"root": root}


def getLogger(name=None):
    if name is None or name == "":
        return root
    hit = _loggers.get(name)
    if hit is not None:
        return hit
    made = Logger(name)
    _loggers[name] = made
    return made


def log(level, msg, *args):
    _emit("root", _coerce_level(level), msg, args)


def debug(msg, *args):
    _emit("root", DEBUG, msg, args)


def info(msg, *args):
    _emit("root", INFO, msg, args)


def warning(msg, *args):
    _emit("root", WARNING, msg, args)


def warn(msg, *args):
    _emit("root", WARNING, msg, args)


def error(msg, *args):
    _emit("root", ERROR, msg, args)


def critical(msg, *args):
    _emit("root", CRITICAL, msg, args)


def fatal(msg, *args):
    _emit("root", CRITICAL, msg, args)


def exception(msg, *args):
    _emit("root", ERROR, msg, args)


def disable(level=CRITICAL):
    _config["level"] = level + 1


def shutdown():
    f = _config["file"]
    if f is not None:
        f.flush()
