"""Optional native Yaz0 reader; the pipeline builds it with its existing NDK."""
import ctypes
import functools
import os


@functools.lru_cache(maxsize=None)
def _decoder(path):
    library = ctypes.CDLL(path)
    decode = library.dm_yaz0_decompress
    decode.argtypes = [ctypes.c_void_p, ctypes.c_ulonglong, ctypes.c_void_p, ctypes.c_ulonglong]
    decode.restype = ctypes.c_longlong
    return library, decode


def decode(data):
    """None without the opt-in library; otherwise the fully decoded bytes or an error."""
    path = os.environ.get('DM_YAZ0_LIB')
    if not path: return None
    if len(data) < 16 or data[:4] != b'Yaz0': raise ValueError('invalid Yaz0 header')
    size = int.from_bytes(data[4:8], 'big')
    output = ctypes.create_string_buffer(max(size, 1))
    library, function = _decoder(path)
    result = function(ctypes.c_char_p(data), len(data), output, size)
    if result != size: raise ValueError('incomplete or invalid Yaz0 stream')
    return output.raw[:size]
