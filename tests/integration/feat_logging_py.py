# logging is stdlib/logging.py: %-formatting, levels and handlers in Python,
# writing through the raw file-descriptor binding. Records are sent to a file so
# the test can read them back on stdout. Valid CPython too.
import logging
import os

LOG = "/tmp/kami_logging_test.log"
if os.path.exists(LOG):
    os.remove(LOG)

logging.basicConfig(filename=LOG, filemode="w", level=logging.INFO,
                    format="%(levelname)s|%(name)s|%(message)s")

logging.debug("invisible: below the threshold")
logging.info("plain message")
logging.info("%s has %d items (%.2f%% done)", "queue", 3, 42.5)
logging.warning("careful: %s", "disk almost full")
logging.error("failed with code %d", 7)
logging.critical("meltdown")

app = logging.getLogger("app")
app.info("hello from %s", "app")
app.setLevel(logging.ERROR)
app.info("suppressed by the logger level")
app.error("still visible")

for line in open(LOG):
    print(line.strip())

print(logging.getLevelName(logging.WARNING), logging.getLevelName(logging.DEBUG))
os.remove(LOG)
print(os.path.exists(LOG))
