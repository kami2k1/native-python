"""Logging: levels, %-style formatting and basicConfig, in Python.

Records go to stderr through the raw file-descriptor binding (`_kami.fd_write`),
so nothing about logging lives in the runtime.
"""
import _kami
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

_UNSET = "\x00kami-unset"

_level = WARNING
_format = BASIC_FORMAT
_datefmt = "%Y-%m-%d %H:%M:%S"
_stream_fd = 2  # stderr


def getLevelName(level):
    if level >= CRITICAL:
        return "CRITICAL"
    if level >= ERROR:
        return "ERROR"
    if level >= WARNING:
        return "WARNING"
    if level >= INFO:
        return "INFO"
    if level >= DEBUG:
        return "DEBUG"
    return "NOTSET"


# ---- %-style formatting ----------------------------------------------------
# The numeric work is delegated to the format() builtin: a printf conversion is
# translated into a format-spec ("%05.2f" → "05.2f").

def _spec(flags, width, prec, conv):
    out = ""
    if flags.find("-") >= 0:
        out = out + "<"
    elif flags.find("0") >= 0:
        out = out + "0"
    if flags.find("+") >= 0:
        out = out + "+"
    out = out + width
    if prec != "":
        out = out + "." + prec
    if conv != "s" and conv != "r":
        out = out + conv
    return out


def _apply(value, flags, width, prec, conv):
    if conv == "s" or conv == "r":
        value = str(value)
    elif conv == "i":
        conv = "d"
    spec = _spec(flags, width, prec, conv)
    if spec == "":
        return str(value)
    return format(value, spec)


def percent_format(fmt, args):
    out = ""
    i = 0
    n = len(fmt)
    argi = 0
    while i < n:
        c = fmt[i]
        if c != "%":
            out = out + c
            i += 1
            continue
        i += 1
        if i >= n:
            raise ValueError("incomplete format")
        if fmt[i] == "%":
            out = out + "%"
            i += 1
            continue
        flags = ""
        while i < n and (fmt[i] == "-" or fmt[i] == "+" or fmt[i] == " " or fmt[i] == "0"):
            flags = flags + fmt[i]
            i += 1
        width = ""
        while i < n and fmt[i] >= "0" and fmt[i] <= "9":
            width = width + fmt[i]
            i += 1
        prec = ""
        if i < n and fmt[i] == ".":
            i += 1
            while i < n and fmt[i] >= "0" and fmt[i] <= "9":
                prec = prec + fmt[i]
                i += 1
        if i >= n:
            raise ValueError("incomplete format")
        conv = fmt[i]
        i += 1
        if argi >= len(args):
            raise TypeError("not enough arguments for format string")
        out = out + _apply(args[argi], flags, width, prec, conv)
        argi += 1
    return out


def _asctime():
    parts = _kami.localtime(time.time())
    out = ""
    i = 0
    fmt = _datefmt
    n = len(fmt)
    while i < n:
        if fmt[i] == "%" and i + 1 < n:
            code = fmt[i + 1]
            i += 2
            if code == "Y":
                out = out + _pad2(parts[0], 4)
            elif code == "m":
                out = out + _pad2(parts[1], 2)
            elif code == "d":
                out = out + _pad2(parts[2], 2)
            elif code == "H":
                out = out + _pad2(parts[3], 2)
            elif code == "M":
                out = out + _pad2(parts[4], 2)
            elif code == "S":
                out = out + _pad2(parts[5], 2)
            elif code == "%":
                out = out + "%"
            continue
        out = out + fmt[i]
        i += 1
    return out


def _pad2(value, width):
    text = str(value)
    while len(text) < width:
        text = "0" + text
    return text


def _render(level, name, message):
    out = ""
    i = 0
    fmt = _format
    n = len(fmt)
    while i < n:
        if fmt[i] == "%" and i + 1 < n and fmt[i + 1] == "(":
            close = -1
            j = i + 2
            while j < n:
                if fmt[j] == ")":
                    close = j
                    break
                j += 1
            if close > 0 and close + 1 < n and fmt[close + 1] == "s":
                field = fmt[i + 2:close]
                if field == "message":
                    out = out + message
                elif field == "levelname":
                    out = out + getLevelName(level)
                elif field == "levelno":
                    out = out + str(level)
                elif field == "name":
                    out = out + name
                elif field == "asctime":
                    out = out + _asctime()
                else:
                    out = out + "?"
                i = close + 2
                continue
        out = out + fmt[i]
        i += 1
    return out


def basicConfig(level=None, format=None, datefmt=None, filename=None, filemode="a",
                stream=None, force=False):
    global _level, _format, _datefmt, _stream_fd
    if level is not None:
        _level = level
    if format is not None:
        _format = format
    if datefmt is not None:
        _datefmt = datefmt
    if filename is not None:
        fd = _kami.fd_open(filename, "a" if filemode == "a" else "w")
        if fd < 0:
            raise OSError("logging: cannot open '" + filename + "': " + _kami.errmsg())
        _stream_fd = fd


def getLevel():
    return _level


def _emit(level, name, msg, args):
    if level < _level:
        return
    text = str(msg)
    if len(args) > 0:
        text = percent_format(text, args)
    _kami.fd_write(_stream_fd, _render(level, name, text) + "\n")


def _pack(a, b, c, d):
    args = []
    for value in [a, b, c, d]:
        if value == _UNSET:
            break
        args.append(value)
    return args


class Logger:
    def __init__(self, name):
        self.name = name
        self.level = NOTSET

    def isEnabledFor(self, level):
        floor = self.level if self.level != NOTSET else _level
        return level >= floor

    def setLevel(self, level):
        self.level = level

    def log(self, level, msg, a=_UNSET, b=_UNSET, c=_UNSET, d=_UNSET):
        if self.level != NOTSET and level < self.level:
            return
        _emit(level, self.name, msg, _pack(a, b, c, d))

    def debug(self, msg, a=_UNSET, b=_UNSET, c=_UNSET, d=_UNSET):
        self.log(DEBUG, msg, a, b, c, d)

    def info(self, msg, a=_UNSET, b=_UNSET, c=_UNSET, d=_UNSET):
        self.log(INFO, msg, a, b, c, d)

    def warning(self, msg, a=_UNSET, b=_UNSET, c=_UNSET, d=_UNSET):
        self.log(WARNING, msg, a, b, c, d)

    def warn(self, msg, a=_UNSET, b=_UNSET, c=_UNSET, d=_UNSET):
        self.log(WARNING, msg, a, b, c, d)

    def error(self, msg, a=_UNSET, b=_UNSET, c=_UNSET, d=_UNSET):
        self.log(ERROR, msg, a, b, c, d)

    def exception(self, msg, a=_UNSET, b=_UNSET, c=_UNSET, d=_UNSET):
        self.log(ERROR, msg, a, b, c, d)

    def critical(self, msg, a=_UNSET, b=_UNSET, c=_UNSET, d=_UNSET):
        self.log(CRITICAL, msg, a, b, c, d)


_loggers = {}
_root = Logger("root")


def getLogger(name="root"):
    if name == "root":
        return _root
    existing = _loggers.get(name)
    if existing is not None:
        return existing
    created = Logger(name)
    _loggers[name] = created
    return created


def log(level, msg, a=_UNSET, b=_UNSET, c=_UNSET, d=_UNSET):
    _emit(level, "root", msg, _pack(a, b, c, d))


def debug(msg, a=_UNSET, b=_UNSET, c=_UNSET, d=_UNSET):
    _emit(DEBUG, "root", msg, _pack(a, b, c, d))


def info(msg, a=_UNSET, b=_UNSET, c=_UNSET, d=_UNSET):
    _emit(INFO, "root", msg, _pack(a, b, c, d))


def warning(msg, a=_UNSET, b=_UNSET, c=_UNSET, d=_UNSET):
    _emit(WARNING, "root", msg, _pack(a, b, c, d))


def warn(msg, a=_UNSET, b=_UNSET, c=_UNSET, d=_UNSET):
    _emit(WARNING, "root", msg, _pack(a, b, c, d))


def error(msg, a=_UNSET, b=_UNSET, c=_UNSET, d=_UNSET):
    _emit(ERROR, "root", msg, _pack(a, b, c, d))


def exception(msg, a=_UNSET, b=_UNSET, c=_UNSET, d=_UNSET):
    _emit(ERROR, "root", msg, _pack(a, b, c, d))


def critical(msg, a=_UNSET, b=_UNSET, c=_UNSET, d=_UNSET):
    _emit(CRITICAL, "root", msg, _pack(a, b, c, d))
