"""Common string constants — compiled from this source, like every stdlib module."""

ascii_lowercase = "abcdefghijklmnopqrstuvwxyz"
ascii_uppercase = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
ascii_letters = ascii_lowercase + ascii_uppercase
digits = "0123456789"
hexdigits = "0123456789abcdefABCDEF"
octdigits = "01234567"
punctuation = "!\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~"
whitespace = " \t\n\r\x0b\x0c"
printable = digits + ascii_letters + punctuation + whitespace


def capwords(s):
    out = []
    for word in s.split():
        out.append(word.capitalize())
    return " ".join(out)
