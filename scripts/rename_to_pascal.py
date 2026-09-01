#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
#
# rename_to_pascal.py — batch-rename C functions from snake_case to
# PascalCase (Microsoft style).  Used once per tree (kernel/, user/) to
# unify naming; safe to re-run (already-PascalCase names are skipped).
#
# Safety: only tokens that look like function names (followed by '(',
# or address-of / function-pointer assignments) are replaced; C
# keywords, libc names, asm symbols and syscall-handler names are
# reserved.  Always rebuild + smoke-test after running.
"""Batch rename snake_case -> PascalCase for a source tree."""
import os, re, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

RESERVED = {
    "main", "_start", "perm_port", "name", "_exit", "_fini", "_init", "__kernel_phys_start", "__kernel_virt_base",
    "__kernel_stack", "__kernel_stack_top", "__kernel_end",
    "__kernel_image_end", "memset", "memcpy", "memmove", "memcmp",
    "strlen", "strcmp", "strncmp", "strcpy", "strncpy", "strcat",
    "strncat", "strchr", "strrchr", "strstr", "strtok", "strtol",
    "strtoul", "strtoll", "strtoull", "atoi", "atol", "malloc", "free",
    "abort", "exit", "printf", "sprintf", "snprintf", "vsnprintf",
    "memchr", "abs", "labs", "rand", "srand", "putchar", "puts",
    "getchar", "gets", "toupper", "tolower", "isalpha", "isdigit",
    "isalnum", "isspace", "isupper", "islower", "isxdigit", "isprint",
    "iscntrl", "ispunct", "isgraph", "strtod", "strtof", "strtold",
    "strcoll", "strxfrm", "strerror", "strcspn", "strspn", "strpbrk",
    "strcasecmp", "strncasecmp", "strdup", "strndup", "strcasestr",
    "strnlen", "strtok_r", "itoa", "ltoa", "utoa", "ultoa", "qsort",
    "bsearch", "div", "ldiv", "lldiv", "calloc", "realloc", "vprintf",
    "vfprintf", "vsprintf", "fprintf", "fputs", "fputc", "fgetc",
    "fgets", "perror", "sscanf", "fscanf", "scanf", "setjmp",
    "longjmp", "fabs", "sqrt", "floor", "ceil", "pow", "fmod", "sin",
    "cos", "tan", "exp", "log", "log10", "atan", "atan2", "rand_r",
    "time", "clock", "difftime", "mktime", "localtime", "gmtime",
    "asctime", "ctime", "strftime", "gmtime_r", "localtime_r",
    "memalign", "posix_memalign", "atof", "atoll", "strtoumax",
    "strtoimax", "wcstol", "wcstoul", "wcstoll", "wcstoull", "wcstod",
    "wcslen", "wcscpy", "wcscat", "wcscmp", "wcsncmp", "wcsncpy",
    "wcsncat", "wcsstr", "wcsrchr", "wcschr", "wcspbrk", "wcsspn",
    "wcscspn", "wcstok", "wmemchr", "wmemcmp", "wmemcpy", "wmemmove",
    "wmemset", "wcscoll", "wcsxfrm", "mbsinit", "mbrlen", "mbrtowc",
    "wcrtomb", "mbsrtowcs", "wcsrtombs", "wcwidth", "mbrtoc16",
    "c16rtomb", "mbrtoc32", "c32rtomb", "iswalpha", "iswdigit",
    "iswspace", "towlower", "towupper", "iswprint", "iswupper",
    "iswlower", "iswcntrl", "iswpunct", "iswgraph", "iswalnum",
    "iswxdigit", "getenv", "llabs", "atoll", "strcasecmp",
    # C11 threads.h / stdlib
    "thrd_create", "thrd_exit", "thrd_join", "thrd_detach", "thrd_sleep",
    "thrd_yield", "thrd_current", "thrd_equal", "thrd_start",
    "mtx_init", "mtx_lock", "mtx_trylock", "mtx_timedlock", "mtx_unlock",
    "cnd_init", "cnd_signal", "cnd_broadcast", "cnd_wait", "cnd_timedwait",
    "cnd_destroy", "mtx_destroy", "thrd_destroy", "call_once",
    "tss_create", "tss_get", "tss_set", "tss_delete", "quick_exit",
    "at_quick_exit", "aligned_alloc", "timespec_get", "timespec_getres",

    # C keywords (misread after "__asm__" / in fn-pointer types)
    "volatile", "const", "inline", "register", "static", "signed",
    "unsigned", "goto", "sizeof", "typeof", "asm", "restrict", "void",
    "char", "int", "long", "short", "double", "float", "struct", "enum",
    "union", "return", "if", "else", "while", "for", "switch", "case",
    "break", "continue", "typedef", "extern",
    # asm-defined symbols (desync risk with .S / boot.asm)
    "context_switch", "context_switch_to_user", "enter_user_mode",
    "syscall_entry_stub", "syscall_entry_fast", "syscall_entry_iretq",
    "syscall_common", "boot_stack_base", "boot_stack_top", "kernel_stack",
    "kernel_stack_top", "gdt64", "gdt64_end", "mb_header_start",
    "mb_header_end", "mboot_info", "saved_eax", "pml4", "pdp", "pd",
    "start64", "start64_higher_half", "isr_handler", "syscall_dispatch",
    "signal_check_syscall", "isr_common_stub",
}

def pascal(name):
    return "".join(p[:1].upper() + p[1:] for p in name.split("_") if p)

def is_reserved(name):
    if name in RESERVED or name.startswith("__") or name[0].isupper():
        return True
    if name.startswith("sys_") or name.startswith("sc_sys_"):
        return True  # syscall handlers / sc_##fn adapters
    return False

DEF_RE = re.compile(
    r"^\s*(?:static\s+|inline\s+|const\s+|volatile\s+)*"
    r"(?:[A-Za-z_][A-Za-z0-9_]*\s+)+?"
    r"(?P<name>[a-z_][a-z0-9_]*)\s*\(")
TYPE_TAIL = re.compile(
    r"\b(static|inline|const|volatile|unsigned|signed|error_t|bool|void|"
    r"u8|u16|u32|u64|i8|i16|i32|i64|size_t|ssize_t|uintptr_t|intptr_t|"
    r"port_t|subject_id_t|irq_t|cap_rights_t|pid_t|tid_t|vaddr_t|paddr_t|"
    r"u128|i128|char|int|long|short|double|float|struct|enum|union)\s*$")

def scan(dirpath):
    files = []
    for dp, _, fns in os.walk(dirpath):
        if "build" in dp or dp.endswith("__pycache__"):
            continue
        for fn in fns:
            if fn.endswith((".c", ".h")):
                files.append(os.path.join(dp, fn))
    return sorted(files)

def main():
    dry = "--dry-run" in sys.argv
    target = sys.argv[1] if len(sys.argv) > 1 and not sys.argv[1].startswith("-") else "kernel"
    files = scan(os.path.join(ROOT, target))
    funcs = set()
    for f in files:
        with open(f, encoding="utf-8") as fh:
            lines = fh.readlines()
        prev_type = False
        for line in lines:
            m = DEF_RE.match(line)
            if m and not is_reserved(m.group("name")):
                first = line.split()[0] if line.split() else ""
                if first in ("return", "if", "while", "for", "switch",
                             "case", "default", "break", "continue",
                             "goto", "else"):
                    continue
                funcs.add(m.group("name"))
            sm = re.match(r"^([a-z_][a-z0-9_]*)\s*\(", line)
            if sm and prev_type and not is_reserved(sm.group(1)):
                funcs.add(sm.group(1))
            s = line.strip()
            if s and not s.startswith(("//", "*", "/*")):
                prev_type = (s.endswith("{") or bool(TYPE_TAIL.search(s)))
            else:
                prev_type = False
    mapping = {}
    for n in sorted(funcs, key=len, reverse=True):
        p = pascal(n)
        if p in RESERVED or p in mapping.values():
            continue
        mapping[n] = p
    print("target=%s files=%d funcs=%d mapped=%d" % (
        target, len(files), len(funcs), len(mapping)))
    if dry:
        for n in sorted(mapping):
            print("  %s -> %s" % (n, mapping[n]))
        return
    changed = 0
    for f in files:
        t = open(f, encoding="utf-8").read()
        o = t
        for n, p in mapping.items():
            t = re.sub(r"\b%s\s*\(" % re.escape(n), p + "(", t)
            t = re.sub(r"&%s\b" % re.escape(n), "&" + p, t)
            t = re.sub(r"=\s*%s\s*;" % re.escape(n), "= " + p + ";", t)
            t = re.sub(r"\(\s*%s\s*," % re.escape(n), "(" + p + ",", t)
            t = re.sub(r",\s*%s\s*," % re.escape(n), ", " + p + ",", t)
            t = re.sub(r",\s*%s\s*\)" % re.escape(n), ", " + p + ")", t)
            t = re.sub(r"\(\s*%s\s*\)" % re.escape(n), "(" + p + ")", t)
        if t != o:
            open(f, "w", encoding="utf-8").write(t)
            changed += 1
    print("files changed: %d" % changed)

if __name__ == "__main__":
    main()
