"""Packaging mirror of options.cpp's host/trust rules; testers run only the native exe."""
import re
import socket
import sys


def literal(address):
    for family in (socket.AF_INET, socket.AF_INET6):
        try:
            socket.inet_pton(family, address)
            return True
        except (OSError, ValueError):
            pass
    return False


def valid_address(address):
    # Keep in parity with valid_address in options.cpp (both use inet_pton).
    if not address or len(address) > 253 or any(ord(c) < 33 or ord(c) > 126 for c in address):
        return False
    if literal(address):
        return True
    if all(c in '0123456789.' for c in address):
        return False
    return all(0 < len(label) <= 63 and not label.startswith('-') and not label.endswith('-')
               and re.fullmatch(r'[A-Za-z0-9-]+', label) for label in address.split('.'))


def validate(address, port, trust):
    if address == 'play.example.org' or not valid_address(address):
        return 'ONLINE-TEST-ADDRESS: package needs a valid real service address (play.example.org is a placeholder)'
    if not re.fullmatch(r'[0-9]{1,5}', port) or not 1 <= int(port) <= 65535:
        return 'ONLINE-TEST-ARGUMENT: package port must be 1-65535'
    if trust != 'system-ca' and not re.fullmatch(r'pin:[0-9a-fA-F]{64}', trust):
        return 'ONLINE-TEST-TRUST: use system-ca or pin:<64 hex digits>'
    if trust == 'system-ca' and literal(address):
        return 'ONLINE-TEST-TRUST: system-ca requires a DNS certificate name; use a pin for an IP literal'
    return ''


if __name__ == '__main__':
    if len(sys.argv) != 4:
        sys.exit('expected address, port and trust')
    error = validate(*sys.argv[1:])
    if error:
        sys.exit(error)
