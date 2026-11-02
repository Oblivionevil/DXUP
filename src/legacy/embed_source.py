# A text file as a byte array in a header: embed_source.py input output name
import sys

data = open(sys.argv[1], 'rb').read()
with open(sys.argv[2], 'w', newline='\n') as f:
    f.write('static const char %s[] = {\n' % sys.argv[3])
    for start in range(0, len(data), 32):
        f.write(','.join(str(b) for b in data[start:start + 32]) + ',\n')
    f.write('};\n')
