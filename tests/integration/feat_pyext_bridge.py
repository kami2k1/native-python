import _hashlib

h = _hashlib.openssl_sha256(b"hello world")
print(h.hexdigest())

import _sqlite3
conn = _sqlite3.connect(":memory:")
cur = conn.cursor()
cur.execute("CREATE TABLE t (id INTEGER, name TEXT)")
cur.execute("INSERT INTO t VALUES (1, 'alpha')")
cur.execute("SELECT id, name FROM t")
print(cur.fetchall())
conn.close()

import zlib
data = "hello hello hello hello" * 10
print(zlib.decompress(zlib.compress(data)) == data)

from _hashlib import openssl_sha1
print(openssl_sha1(b"abc").hexdigest())
