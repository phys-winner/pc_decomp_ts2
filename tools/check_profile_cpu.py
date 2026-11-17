"""Reject profiling a foreign process or UID before staging the recorder."""
from profile_cpu import PACKAGE, validate_target

def main():
    status = "Uid:\t10123\t10123\t10123\t10123\n"
    validate_target(PACKAGE.encode()+b"\0", status, 10123)
    bad = [(b"org.other.game\0", status, 10123),
           ((PACKAGE+":launcher").encode()+b"\0", status, 10123),
           (PACKAGE.encode()+b".suffix\0", status, 10123),
           (PACKAGE.encode()+b"\0", status, 10124),
           (PACKAGE.encode()+b"\0", "Uid: 10123 10123 10123 0\n", 10123),
           (PACKAGE.encode()+b"\0", "Uid: 10123 10123\n", 10123),
           (PACKAGE.encode()+b"\0", "Name: test\n", 10123)]
    for args in bad:
        try:
            validate_target(*args)
        except ValueError:
            pass
        else:
            raise AssertionError(args)
    print("PASS profile target: exact process, four matching UIDs, 7 rejection cases")

if __name__ == "__main__":
    main()
