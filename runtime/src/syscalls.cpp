// C-ABI primitive layer.
//
// Two jobs:
//
//  1. The unbox/box helpers the compiler emits around a direct C call. A
//     CCall in the IR looks like
//
//         %a = call double @kami_c_arg_f64(ptr %slot)
//         %r = call double @sqrt(double %a)
//         call void @kami_make_float(ptr %ret, double %r)
//
//     so all the type checking lives here, once, instead of being inlined into
//     every call site.
//
//  2. The raw operations CPython implements in C extension modules and that
//     have no natural expression as Python: descriptor I/O (posix.open/read/
//     write/close/lseek), gethostname, and struct.pack/unpack. These are the
//     landing pads the compiler maps `_os.*`, `_socket.*` and `_struct.*` onto.
#include "rt_internal.h"

#include "kami_builtins.h"

#include <cctype>
#include <cerrno>
#include <cstring>
#include <cstdio>
#include <string>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace kami {

namespace {

int64_t as_int(const KamiValue* v, const char* what) {
    switch (v->tag) {
    case KT_INT:
    case KT_BOOL: return v->i;
    case KT_FLOAT: return (int64_t)v->f;
    default:
        panic(std::string(what) + ": expected an int, got " + type_name(v->tag));
    }
}

std::string as_text(const KamiValue* v, const char* what) {
    if (v->tag != KT_STR) panic(std::string(what) + ": expected a str, got " + type_name(v->tag));
    KamiStr* s = (KamiStr*)v->p;
    return std::string(s->data, (size_t)s->len);
}

[[noreturn]] void os_error(const char* op) {
    // strerror_s / strerror_r have incompatible shapes across platforms, and
    // MSVC deprecates plain strerror. errno is captured immediately, before
    // building the message can disturb it.
    int saved = errno;
    char buf[128];
#ifdef _WIN32
    if (strerror_s(buf, sizeof buf, saved) != 0) snprintf(buf, sizeof buf, "errno %d", saved);
#else
    snprintf(buf, sizeof buf, "%s", strerror(saved));
#endif
    panic(std::string(op) + " failed: " + buf);
}

// ---- struct pack/unpack ----------------------------------------------------
// Supported format characters, all in native byte order (a leading '<', '>',
// '=' or '!' selects little/big endian explicitly):
//   b/B 1 byte   h/H 2 bytes   i/I l/L 4 bytes   q/Q 8 bytes   f 4   d 8
struct Field {
    char code;
    int size;
};

bool parse_format(const std::string& fmt, std::vector<Field>& out, bool& big_endian) {
    big_endian = false;
    size_t i = 0;
    if (!fmt.empty() && (fmt[0] == '<' || fmt[0] == '>' || fmt[0] == '!' || fmt[0] == '=' ||
                         fmt[0] == '@')) {
        big_endian = fmt[0] == '>' || fmt[0] == '!';
        i = 1;
    }
    for (; i < fmt.size(); i++) {
        char c = fmt[i];
        int repeat = 1;
        if (isdigit((unsigned char)c)) {
            repeat = 0;
            while (i < fmt.size() && isdigit((unsigned char)fmt[i])) repeat = repeat * 10 + (fmt[i++] - '0');
            if (i >= fmt.size()) return false;
            c = fmt[i];
        }
        int size;
        switch (c) {
        case 'b': case 'B': case 'x': size = 1; break;
        case 'h': case 'H': size = 2; break;
        case 'i': case 'I': case 'l': case 'L': case 'f': size = 4; break;
        case 'q': case 'Q': case 'd': size = 8; break;
        default: return false;
        }
        for (int k = 0; k < repeat; k++) out.push_back({c, size});
    }
    return true;
}

void put_bytes(std::string& buf, const void* src, int size, bool big_endian) {
    const unsigned char* p = (const unsigned char*)src;
    if (big_endian)
        for (int i = size - 1; i >= 0; i--) buf += (char)p[i];
    else
        for (int i = 0; i < size; i++) buf += (char)p[i];
}

void get_bytes(const char* src, void* dst, int size, bool big_endian) {
    unsigned char* p = (unsigned char*)dst;
    if (big_endian)
        for (int i = 0; i < size; i++) p[size - 1 - i] = (unsigned char)src[i];
    else
        memcpy(dst, src, (size_t)size);
}

} // namespace

// Called from builtins.cpp's dispatch when no earlier table claims the id.
bool dispatch_syscalls(std::unique_lock<std::recursive_mutex>&, int64_t id, KamiValue* out,
                       KamiValue** argv, int64_t nargs) {
    auto set_int = [&](int64_t v) {
        out->tag = KT_INT;
        out->i = v;
    };
    auto set_str = [&](const std::string& s) {
        out->tag = KT_STR;
        out->p = str_new(s.data(), (int64_t)s.size());
    };
    switch (id) {
    case KB_SYS_OPEN: {
        if (nargs < 2 || nargs > 3) panic("open(): expected (path, flags[, mode])");
        std::string path = as_text(argv[0], "os.open");
        int64_t pyflags = as_int(argv[1], "os.open");
        int64_t mode = nargs == 3 ? as_int(argv[2], "os.open") : 0666;
        // The compiler hands us CPython's Linux flag values; translate them so
        // the same Python source works on Windows too.
        int flags = 0;
        if ((pyflags & 3) == 1) flags |= O_WRONLY;
        else if ((pyflags & 3) == 2) flags |= O_RDWR;
        else flags |= O_RDONLY;
        if (pyflags & 0100) flags |= O_CREAT;
        if (pyflags & 01000) flags |= O_TRUNC;
        if (pyflags & 02000) flags |= O_APPEND;
#ifdef _WIN32
        flags |= _O_BINARY;
        int fd = _open(path.c_str(), flags, (int)mode);
#else
        int fd = ::open(path.c_str(), flags, (mode_t)mode);
#endif
        if (fd < 0) os_error("os.open");
        set_int(fd);
        return true;
    }
    case KB_SYS_READ: {
        if (nargs != 2) panic("read(): expected (fd, n)");
        int64_t fd = as_int(argv[0], "os.read");
        int64_t n = as_int(argv[1], "os.read");
        if (n < 0) panic("os.read(): negative size");
        std::string buf((size_t)n, '\0');
#ifdef _WIN32
        int got = _read((int)fd, buf.data(), (unsigned)n);
#else
        ssize_t got = ::read((int)fd, buf.data(), (size_t)n);
#endif
        if (got < 0) os_error("os.read");
        buf.resize((size_t)got);
        set_str(buf);
        return true;
    }
    case KB_SYS_WRITE: {
        if (nargs != 2) panic("write(): expected (fd, data)");
        int64_t fd = as_int(argv[0], "os.write");
        std::string data = as_text(argv[1], "os.write");
#ifdef _WIN32
        int put = _write((int)fd, data.data(), (unsigned)data.size());
#else
        ssize_t put = ::write((int)fd, data.data(), data.size());
#endif
        if (put < 0) os_error("os.write");
        set_int((int64_t)put);
        return true;
    }
    case KB_SYS_CLOSE: {
        if (nargs != 1) panic("close(): expected (fd)");
        int64_t fd = as_int(argv[0], "os.close");
#ifdef _WIN32
        if (_close((int)fd) < 0) os_error("os.close");
#else
        if (::close((int)fd) < 0) os_error("os.close");
#endif
        out->tag = KT_NONE;
        out->i = 0;
        return true;
    }
    case KB_SYS_LSEEK: {
        if (nargs != 3) panic("lseek(): expected (fd, pos, whence)");
        int64_t fd = as_int(argv[0], "os.lseek");
        int64_t pos = as_int(argv[1], "os.lseek");
        int64_t whence = as_int(argv[2], "os.lseek");
#ifdef _WIN32
        long r = _lseek((int)fd, (long)pos, (int)whence);
#else
        off_t r = ::lseek((int)fd, (off_t)pos, (int)whence);
#endif
        if (r < 0) os_error("os.lseek");
        set_int((int64_t)r);
        return true;
    }
    case KB_SYS_GETHOSTNAME: {
        char buf[256] = {0};
#ifdef _WIN32
        // Winsock must be up before gethostname(); netio.cpp does that lazily,
        // so fall back to the Win32 API which never needs initialisation.
        DWORD n = sizeof buf;
        if (!GetComputerNameA(buf, &n)) panic("gethostname() failed");
#else
        if (::gethostname(buf, sizeof buf - 1) != 0) os_error("gethostname");
#endif
        set_str(buf);
        return true;
    }
    case KB_STRUCT_CALCSIZE: {
        if (nargs != 1) panic("calcsize(): expected (format)");
        std::vector<Field> fields;
        bool be = false;
        std::string fmt = as_text(argv[0], "struct.calcsize");
        if (!parse_format(fmt, fields, be))
            panic("struct.calcsize(): bad char in struct format '" + fmt + "'");
        int64_t total = 0;
        for (const Field& f : fields) total += f.size;
        set_int(total);
        return true;
    }
    case KB_STRUCT_PACK: {
        if (nargs < 1) panic("pack(): expected (format, *values)");
        std::string fmt = as_text(argv[0], "struct.pack");
        std::vector<Field> fields;
        bool be = false;
        if (!parse_format(fmt, fields, be))
            panic("struct.pack(): bad char in struct format '" + fmt + "'");
        std::string buf;
        int64_t vi = 1;
        for (const Field& f : fields) {
            if (f.code == 'x') {
                buf += '\0';
                continue;
            }
            if (vi >= nargs) panic("struct.pack(): not enough arguments for format '" + fmt + "'");
            KamiValue* v = argv[vi++];
            if (f.code == 'f') {
                float x = (float)(v->tag == KT_FLOAT ? v->f : (double)as_int(v, "struct.pack"));
                put_bytes(buf, &x, 4, be);
            } else if (f.code == 'd') {
                double x = v->tag == KT_FLOAT ? v->f : (double)as_int(v, "struct.pack");
                put_bytes(buf, &x, 8, be);
            } else {
                // The low f.size bytes of x are the value; put_bytes emits them
                // low-first or high-first as the format asked. (Assumes a
                // little-endian host, which every target we compile for is.)
                int64_t x = as_int(v, "struct.pack");
                put_bytes(buf, &x, f.size, be);
            }
        }
        if (vi != nargs) panic("struct.pack(): too many arguments for format '" + fmt + "'");
        set_str(buf);
        return true;
    }
    case KB_STRUCT_UNPACK: {
        if (nargs != 2) panic("unpack(): expected (format, data)");
        std::string fmt = as_text(argv[0], "struct.unpack");
        std::string data = as_text(argv[1], "struct.unpack");
        std::vector<Field> fields;
        bool be = false;
        if (!parse_format(fmt, fields, be))
            panic("struct.unpack(): bad char in struct format '" + fmt + "'");
        size_t need = 0;
        for (const Field& f : fields) need += (size_t)f.size;
        if (data.size() != need)
            panic("struct.unpack(): buffer of size " + std::to_string(data.size()) +
                  " does not match format '" + fmt + "' (needs " + std::to_string(need) + ")");
        KamiList* l = list_new((int64_t)fields.size());
        out->tag = KT_LIST;
        out->p = l;
        const char* p = data.data();
        for (const Field& f : fields) {
            KamiValue item{KT_NONE, {0}};
            if (f.code == 'x') {
                p += 1;
                continue;
            }
            if (f.code == 'f') {
                float x;
                get_bytes(p, &x, 4, be);
                item.tag = KT_FLOAT;
                item.f = x;
            } else if (f.code == 'd') {
                double x;
                get_bytes(p, &x, 8, be);
                item.tag = KT_FLOAT;
                item.f = x;
            } else {
                unsigned char raw[8] = {0};
                get_bytes(p, raw, f.size, be);
                uint64_t u = 0;
                for (int i = f.size - 1; i >= 0; i--) u = (u << 8) | raw[i];
                bool signed_code = f.code == 'b' || f.code == 'h' || f.code == 'i' ||
                                   f.code == 'l' || f.code == 'q';
                int64_t x = (int64_t)u;
                if (signed_code && f.size < 8) {
                    int64_t sign_bit = (int64_t)1 << (f.size * 8 - 1);
                    if (x & sign_bit) x -= sign_bit << 1;
                }
                item.tag = KT_INT;
                item.i = x;
            }
            list_push(l, &item);
            p += f.size;
        }
        return true;
    }
    default: return false;
    }
}

} // namespace kami

using namespace kami;

// ---- unbox/box helpers for direct C calls ---------------------------------

extern "C" int64_t kami_c_arg_i64(const KamiValue* v) {
    Lock lk(g_lock);
    return as_int(v, "C call argument");
}

extern "C" double kami_c_arg_f64(const KamiValue* v) {
    Lock lk(g_lock);
    switch (v->tag) {
    case KT_FLOAT: return v->f;
    case KT_INT:
    case KT_BOOL: return (double)v->i;
    default:
        panic(std::string("C call argument: expected a number, got ") + type_name(v->tag));
    }
}

// KamiStr keeps a NUL terminator right after the payload, so a Python str can
// be handed to a C function as a const char* with no copy. The value lives in a
// GC-rooted frame slot for the whole call, so the pointer stays valid.
extern "C" const char* kami_c_arg_cstr(const KamiValue* v) {
    Lock lk(g_lock);
    if (v->tag == KT_NONE) return nullptr; // NULL pointer
    if (v->tag != KT_STR)
        panic(std::string("C call argument: expected a str, got ") + type_name(v->tag));
    return ((KamiStr*)v->p)->data;
}

extern "C" void kami_c_ret_cstr(KamiValue* out, const char* s) {
    Lock lk(g_lock);
    if (!s) {
        out->tag = KT_NONE;
        out->i = 0;
        return;
    }
    out->tag = KT_STR;
    out->p = str_new(s, (int64_t)strlen(s));
}
