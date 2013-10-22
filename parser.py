import sys
lines = open(sys.argv[1],'r').read().split("\n");

for line in lines:
	#print(line.find("0x00007"))

	if line.find("0x00007") == 0 or line.find("0x000000007")==0:
			0
	elif len(line) != 0:
		values = line.split("\t")
		base = int("0x00000000004004b0",16)
		off = 12;

		thisAddr = int(values[0],16)

		if thisAddr >= base and thisAddr < (base+off):
			print(values)
