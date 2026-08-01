# `logging` is now a pure-Python module (runtime/pylib/logging.py).
# Records go to stderr, so stdout stays clean for the golden file.
import logging

logging.basicConfig(level=logging.INFO, format="%(levelname)s|%(name)s|%(message)s")
logging.debug("not shown")
logging.info("plain")
logging.info("with %s and %d", "args", 7)
logging.warning("careful")
logging.error("broken: %s", "reason")

log = logging.getLogger("mod.sub")
log.info("named logger")
print("levels:", logging.DEBUG, logging.INFO, logging.WARNING, logging.ERROR, logging.CRITICAL)
print("name of 30:", logging.getLevelName(30))
print("root is root:", logging.getLogger() is logging.root)
print("same instance:", logging.getLogger("x") is logging.getLogger("x"))

logging.basicConfig(level=logging.ERROR)
logging.warning("suppressed now")
print("done")
