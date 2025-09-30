import os
import sys
import binascii

infile_content = open(sys.argv[1], "rb").read()
crc = binascii.crc32(infile_content)

size = os.path.getsize(sys.argv[1])

out_file_content = """struct appData {{
    static constexpr std::size_t        size{{{}}};
    static constexpr typename Crc::type crc{{{}}};
}};
""".format(size, crc)

filename = sys.argv[2]
os.makedirs(os.path.dirname(filename), exist_ok=True)
with open(filename, "w") as f:
    f.write(out_file_content)
