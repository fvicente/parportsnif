#!/usr/bin/python
import sys


def main(filename, out):
    with open(filename) as f:
        with open(out, "w+") as fout:
            first = 0
            for line in f:
                if "@" not in line:
                    fout.write(line)
                    continue
                val, step = line.split("@")
                step = int(step)
                if first == 0:
                    first = step
                    step = 0
                else:
                    step -= first
                fout.write("%s@%d\n" % (val, step))


if __name__ == "__main__":
    if len(sys.argv) > 1:
        filename = sys.argv[1]
        main(filename, "%s.out" % filename)
    else:
        print "Missing parameter\nUsage:\n    rebase.py [filename]"
