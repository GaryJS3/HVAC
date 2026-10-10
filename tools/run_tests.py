"""Build and run the actual portable C++ implementation. Requires ziglang 0.13.0."""
import pathlib
import subprocess
import sys
import ziglang

root = pathlib.Path(__file__).resolve().parents[1]
zig = pathlib.Path(ziglang.__file__).parent / ('zig.exe' if sys.platform == 'win32' else 'zig')
out = root / 'build' / ('host_tests.exe' if sys.platform == 'win32' else 'host_tests')
out.parent.mkdir(exist_ok=True)
subprocess.run([str(zig), 'c++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                '-I' + str(root / 'lib/Protocol'), str(root / 'test/host_tests.cpp'),
                str(root / 'lib/Protocol/S1S2FrameParser.cpp'),
                str(root / 'lib/Protocol/S1S2Decoder.cpp'), '-o', str(out)], check=True)
subprocess.run([str(out), *sys.argv[1:]], check=True)
