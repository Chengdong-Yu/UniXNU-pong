"""Selected Darwin24 boot command-line ABI guard for candidate packaging."""
LIMIT = 1023  # boot_args.CommandLine[1024], including its NUL terminator


def require_fits(arguments):
    text = arguments if isinstance(arguments, str) else ' '.join(arguments)
    try:
        encoded = text.encode('ascii')
    except UnicodeEncodeError as error:
        raise ValueError('DarwinBoot24 command line requires ASCII') from error
    if b'\0' in encoded or len(encoded) > LIMIT:
        raise ValueError(f'Darwin24 boot command has {len(encoded)} bytes; maximum {LIMIT}. '
                         'Shorten optional log filters; preserve the boot_args ABI.')
    return len(encoded)
